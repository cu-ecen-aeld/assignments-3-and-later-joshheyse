#include "aesd_ioctl.h"
#include "queue.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#ifndef GIT_HASH

#define GIT_HASH "N/A"
#endif

// global vars are ugly
int server_fd;
int file_fd;
int should_exit = 0;
int processing_packet = 0;
pthread_t timer_thread;

pthread_mutex_t file_lock;
#if USE_AESD_CHAR_DEVICE
const char *file_path = "/dev/aesdchar";
#else
const char *file_path = "/tmp/aesdsocket";
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct options {
  in_port_t port;
  int daemonize;
};

typedef struct connection slist_data_t;
struct connection {
  int fd;
  struct sockaddr_in addr;
  pthread_t thread_id;
  int thread_complete;
  SLIST_ENTRY(connection) entries;
};

pthread_mutex_t connections_lock;
SLIST_HEAD(connections_t, connection) connections;

void printUsage(char *argv[]);
void parseArgs(int argc, char *argv[], struct options *options);
void cleanUpAndExit(int status);
int write_buffer(int fd, char *buffer, int buffer_len);
void *handle_client_connection(void *arg);
void deamonize(char *base_name);
void mark_all_threads_complete(struct connections_t *connections);
void collect_complete_threads(struct connections_t *connections);

void printUsage(char *argv[]) {
  fprintf(stderr, "Usage: %s -d -p <port>\n", argv[0]);
  exit(EXIT_FAILURE);
}

void parseArgs(int argc, char *argv[], struct options *options) {
  int opt = -1;
  while ((opt = getopt(argc, argv, "dp:")) != -1) {
    switch (opt) {
    case 'p':
      options->port = (in_port_t)strtol(optarg, NULL, 10);
      break;
    case 'd':
      options->daemonize = 1;
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

  if (server_fd > 0) {
    close(server_fd);
    server_fd = -1;
  }

  if (file_fd > 0) {
    close(file_fd);
    remove(file_path);
    file_fd = -1;
  }

  should_exit = 1;
  collect_complete_threads(&connections);

  pthread_cancel(timer_thread);

  closelog();
  exit(status);
}

void signal_handler(int signal) {
  syslog(LOG_INFO, "Caught signal, exiting");
  cleanUpAndExit(EXIT_SUCCESS);
}

int write_buffer(int fd, char *buffer, int buffer_len) {
  int bytes_written = 0;
  while (bytes_written < buffer_len) {
    int ret = write(fd, buffer + bytes_written, buffer_len - bytes_written);
    if (ret < 0) {
      return 0;
    }
    bytes_written += ret;
  }
  return 1;
}

void *handle_client_connection(void *arg) {
  struct connection *conn = (struct connection *)arg;
  char ip_address[16];
  char in_buffer[1024];
  int in_buffer_len = sizeof(in_buffer) - 1;

  char out_buffer[1024];
  memset(in_buffer, 0, sizeof(in_buffer));
  memset(out_buffer, 0, sizeof(out_buffer));

  inet_ntop(AF_INET, &conn->addr.sin_addr, ip_address, sizeof(ip_address));
  syslog(LOG_INFO, "Accepted connection from %s", ip_address);

  int flags = fcntl(conn->fd, F_GETFL, 0);
  fcntl(conn->fd, F_SETFL, flags | O_NONBLOCK);

  ssize_t in_bytes_read = -1;
  while (!should_exit && (in_bytes_read = read(conn->fd, in_buffer, in_buffer_len)) != 0) {
    if (in_bytes_read == -1) {
      int res = errno;
      if (res == EAGAIN || res == EWOULDBLOCK) {
        usleep(1000000);
        continue;
      }
    }

    { // start file_lock
      if (pthread_mutex_lock(&file_lock)) {
        syslog(LOG_ERR, "Failed to lock file: %s", strerror(errno));
        cleanUpAndExit(EXIT_FAILURE);
      }

      in_buffer[in_bytes_read] = '\0';

      /* lseek(file_fd, 0, SEEK_END); */

      char *newline_char = in_buffer;
      char *prev_newline_char = in_buffer;

      while ((newline_char = strchr(prev_newline_char, '\n')) != NULL) {
        int length = newline_char - prev_newline_char + 1;
        syslog(LOG_INFO, "Received %*.*s", length, length, prev_newline_char);
        struct aesd_seekto seekto;
        if (strncmp(prev_newline_char, "AESDCHAR_IOCSEEKTO", MIN(length, 18)) == 0) {
          if (sscanf(prev_newline_char, "AESDCHAR_IOCSEEKTO:%d,%d", &seekto.write_cmd, &seekto.write_cmd_offset) == 2) {
            if (ioctl(file_fd, AESDCHAR_IOCSEEKTO, &seekto) < 0) {
              syslog(LOG_ERR, "Failed to seek to %d %d", seekto.write_cmd, seekto.write_cmd_offset);
              pthread_mutex_unlock(&file_lock);
              close(conn->fd);
              conn->thread_complete = 1;
              return NULL;
            }
          }
        } else {
          write_buffer(file_fd, prev_newline_char, length);
          fdatasync(file_fd);
          lseek(file_fd, 0, SEEK_SET);
        }

        ssize_t file_bytes_read = 0;
        while ((file_bytes_read = read(file_fd, out_buffer, sizeof(out_buffer))) > 0) {
          if (!write_buffer(conn->fd, out_buffer, file_bytes_read)) {
            syslog(LOG_ERR, "Failed to write to socket");
            close(conn->fd);
            conn->thread_complete = 1;
            return NULL;
          }
        }
        prev_newline_char = newline_char + 1;
      }

      /* lseek(file_fd, 0, SEEK_END); */
      /* write_buffer(file_fd, prev_newline_char, in_bytes_read - (prev_newline_char - in_buffer)); */

      if (pthread_mutex_unlock(&file_lock)) {
        syslog(LOG_ERR, "Failed to unlock file: %s", strerror(errno));
        cleanUpAndExit(EXIT_FAILURE);
      }
    } // end file_lock
  }

  if (in_bytes_read < 0) {
    syslog(LOG_ERR, "Failed to read from socket");
  }
  syslog(LOG_INFO, "Connection closed from %s", ip_address);

  close(conn->fd);
  conn->thread_complete = 1;
  return NULL;
}

void deamonize(char *base_name) {

  pid_t pid = fork();

  if (pid < 0) {
    syslog(LOG_ERR, "Failed to fork");
    cleanUpAndExit(EXIT_FAILURE);
  }

  if (pid > 0) {
    exit(EXIT_SUCCESS);
  }

  if (setsid() < 0) {
    syslog(LOG_ERR, "Failed to create new session");
    cleanUpAndExit(EXIT_FAILURE);
  }

  pid = fork();
  if (pid < 0) {
    syslog(LOG_ERR, "Failed to fork");
    cleanUpAndExit(EXIT_FAILURE);
  }

  if (pid > 0) {
    exit(EXIT_SUCCESS);
  }

  umask(0);
  chdir("/");

  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);

  closelog();
  openlog(base_name, LOG_PID, LOG_DAEMON);
  setlogmask(LOG_UPTO(LOG_INFO));
}

void mark_all_threads_complete(struct connections_t *connections) {
  struct connection *conn;
  pthread_mutex_lock(&connections_lock);
  SLIST_FOREACH(conn, connections, entries) { conn->thread_complete = 1; }
  pthread_mutex_unlock(&connections_lock);
}

void collect_complete_threads(struct connections_t *connections) {
  struct connection *conn;

  pthread_mutex_lock(&connections_lock);
  SLIST_FOREACH(conn, connections, entries) {
    if (conn->thread_complete) {
      syslog(LOG_INFO, "Joining thread %lu", conn->thread_id);
      pthread_join(conn->thread_id, NULL);
      SLIST_REMOVE(connections, conn, connection, entries);
      free(conn);
    }
  }
  pthread_mutex_unlock(&connections_lock);
}

void *timer(void *arg) {
  char out_buffer[64];
  const char preamble[] = "timestamp:";
  strcpy(out_buffer, preamble);
  char *timestamp = out_buffer + sizeof(preamble) - 1;

  while (!should_exit) {
    syslog(LOG_INFO, "Timer tick");
    time_t now = time(NULL);
    struct tm *now_tm = localtime(&now);
    int ret = strftime(timestamp, sizeof(out_buffer) - sizeof(preamble) - 1, "%Y-%m-%d %H:%M:%S\n", now_tm);
    if (ret == 0) {
      syslog(LOG_ERR, "Failed to format time %s", strerror(errno));
    }

    { // start file_lock
      if (pthread_mutex_lock(&file_lock)) {
        syslog(LOG_ERR, "Failed to lock file: %s", strerror(errno));
        cleanUpAndExit(EXIT_FAILURE);
      }
      write(file_fd, out_buffer, strlen(out_buffer));
      if (pthread_mutex_unlock(&file_lock)) {
        syslog(LOG_ERR, "Failed to lock file: %s", strerror(errno));
        cleanUpAndExit(EXIT_FAILURE);
      }
    } // end file_lock

    sleep(10);
  }
  return NULL;
}

int main(int argc, char *argv[]) {

  struct options options = {
      .port = 9000,
  };
  parseArgs(argc, argv, &options);

  char *base_name = basename(argv[0]);
  fprintf(stdout, "Starting %s %s \n", base_name, GIT_HASH);
  syslog(LOG_INFO, "Starting %s %s \n", base_name, GIT_HASH);
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

  if (options.daemonize) {
    deamonize(base_name);
  }

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  struct connection *conn;
  SLIST_INIT(&connections);

#if USE_AESD_CHAR_DEVICE == 0
  pthread_create(&timer_thread, NULL, &timer, NULL);
#endif

  fprintf(stdout, "Waiting for connection on port %d\n", options.port);

  while (!should_exit) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    int accepted_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
    if (accepted_fd < 0) {
      syslog(LOG_ERR, "Failed to accept connection");
      continue;
    }

    conn = malloc(sizeof(struct connection));
    conn->fd = accepted_fd;
    conn->addr = client_addr;
    conn->thread_complete = 0;

    if (pthread_create(&conn->thread_id, NULL, &handle_client_connection, conn) != 0) {
      syslog(LOG_ERR, "Failed to create thread");
      free(conn);
      close(accepted_fd);
      continue;
    }

    pthread_mutex_lock(&connections_lock);
    SLIST_INSERT_HEAD(&connections, conn, entries);
    pthread_mutex_unlock(&connections_lock);

    collect_complete_threads(&connections);
  }

  cleanUpAndExit(EXIT_SUCCESS);
}
