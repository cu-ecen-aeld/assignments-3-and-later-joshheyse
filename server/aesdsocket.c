#include <arpa/inet.h>
#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

// global vars are ugly
int server_fd;
int accepted_fd;
int file_fd;
int should_exit = 0;
const char *file_path = "/tmp/aesdsocket";

struct options {
  in_port_t port;
};

void printUsage(char *argv[]) {
  fprintf(stderr, "Usage: %s -p <port>\n", argv[0]);
  exit(EXIT_FAILURE);
}

void parseArgs(int argc, char *argv[], struct options *options) {
  int opt = -1;
  while ((opt = getopt(argc, argv, "p:")) != -1) {
    switch (opt) {
    case 'p':
      options->port = (in_port_t)strtol(optarg, NULL, 10);
      break;
    case '?':
    case 'h':
      fprintf(stderr, "Unknow option or missing argument: %c %c\n", opt, optopt);
      exit(EXIT_FAILURE);
    default:
      printUsage(argv);
    }
  }
}

void cleanUpAndExit(int status) {
  syslog(LOG_INFO, "Exiting with status %d", status);

  if (accepted_fd > 0) {
    close(server_fd);
    accepted_fd = -1;
  }

  if (server_fd > 0) {
    close(server_fd);
    server_fd = -1;
  }

  if (file_fd > 0) {
    close(file_fd);
    // remove(file_path);
    file_fd = -1;
  }

  closelog();
  exit(status);
}

void signal_handler(int signal) {
  syslog(LOG_INFO, "Caught signal, exiting");
  should_exit = 1;
  if (accepted_fd <= 0) {
    cleanUpAndExit(EXIT_SUCCESS);
  }
}

int write_buffer(int fd, char *buffer, int buffer_len) {
  int bytes_written = 0;
  while (bytes_written < buffer_len) {
    printf("Writing %d bytes to %d\n", buffer_len - bytes_written, fd);
    int ret = write(fd, buffer + bytes_written, buffer_len - bytes_written);
    if (ret < 0) {
      return 0;
    }
    bytes_written += ret;
  }
  return 1;
}

int main(int argc, char *argv[]) {

  struct options options = {
      .port = 0,
  };
  parseArgs(argc, argv, &options);

  char *base_name = basename(argv[0]);
  fprintf(stdout, "Starting %s\n", base_name);
  openlog(base_name, LOG_PID, LOG_USER);
  setlogmask(LOG_UPTO(LOG_INFO));

  file_fd = open(file_path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (file_fd < 0) {
    syslog(LOG_ERR, "Failed to open file %s", file_path);
    cleanUpAndExit(EXIT_FAILURE);
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    syslog(LOG_ERR, "Failed to create socket");
    cleanUpAndExit(EXIT_FAILURE);
  }

  int ret;
  int option_value = 1;
  ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option_value, sizeof(option_value));
  if (ret < 0) {
    syslog(LOG_ERR, "Failed to set socket option");
    cleanUpAndExit(EXIT_FAILURE);
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(options.port);

  ret = bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
  if (ret < 0) {
    syslog(LOG_ERR, "Failed to bind socket");
    cleanUpAndExit(EXIT_FAILURE);
  }

  ret = listen(server_fd, 10);
  if (ret < 0) {
    syslog(LOG_ERR, "Failed to listen on socket");
    cleanUpAndExit(EXIT_FAILURE);
  }

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  char ip_address[16];
  char in_buffer[1024];
  int in_buffer_len = sizeof(in_buffer) - 1;
  memset(in_buffer, 0, sizeof(in_buffer));

  char out_buffer[1024];

  while (!should_exit) {

    lseek(file_fd, 0, SEEK_END);

    fprintf(stdout, "Waiting for connection on port %d\n", options.port);
    struct sockaddr_in client_addr;
    socklen_t socket_addr_len = sizeof(client_addr);

    accepted_fd = accept(server_fd, (struct sockaddr *)&server_addr, &socket_addr_len);
    if (accepted_fd < 0) {
      syslog(LOG_ERR, "Failed to accept connection");
      continue;
    }

    inet_ntop(AF_INET, &client_addr.sin_addr, ip_address, sizeof(ip_address));
    syslog(LOG_INFO, "Accepted connection from %s", ip_address);

    ssize_t in_bytes_read = -1;
    while ((in_bytes_read = read(accepted_fd, in_buffer, in_buffer_len)) > 0) {
      fprintf(stdout, "Received %ld bytes '%s'\n", in_bytes_read, in_buffer);

      char *newline_char = in_buffer;
      char *prev_newline_char = in_buffer;

      while ((newline_char = strchr(prev_newline_char, '\n')) != NULL) {
        fprintf(stdout, "Found newline at %p %p %p\n", in_buffer, prev_newline_char, newline_char);

        if (!write_buffer(file_fd, prev_newline_char, newline_char - prev_newline_char + 1)) {
          syslog(LOG_ERR, "Failed to write to file");
          cleanUpAndExit(EXIT_FAILURE);
        }

        lseek(file_fd, 0, SEEK_SET);
        ssize_t file_bytes_read = 0;
        while ((file_bytes_read = read(file_fd, out_buffer, sizeof(out_buffer))) > 0) {
          if (!write_buffer(accepted_fd, out_buffer, file_bytes_read)) {
            syslog(LOG_ERR, "Failed to write to socket");
            cleanUpAndExit(EXIT_FAILURE);
          }
        }
        lseek(file_fd, 0, SEEK_END);
        prev_newline_char = newline_char + 1;
      }

      if (!write_buffer(file_fd, prev_newline_char, in_bytes_read - (prev_newline_char - in_buffer))) {
        syslog(LOG_ERR, "Failed to write to file");
        cleanUpAndExit(EXIT_FAILURE);
      }
    }

    if (in_bytes_read < 0) {
      syslog(LOG_ERR, "Failed to read from socket");
    } else {
      syslog(LOG_INFO, "Connection closed");
    }

    close(accepted_fd);
    accepted_fd = -1;
  }

  cleanUpAndExit(EXIT_SUCCESS);
}