#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

int main(int argc, char **argv) {
  openlog("writer", LOG_PID, LOG_USER);

  if (argc < 3) {
    printf("Usage: writer <file> <string>\n");
    syslog(LOG_ERR, "Invalid arguments");
    return 1;
  }

  char *file = argv[1];
  char *text = argv[2];

  int fd = open(file, O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    printf("Failed to open file %s", file);
    syslog(LOG_ERR, "Failed to open file %s", file);
    return 1;
  }

  syslog(LOG_DEBUG, "Writing %s to %s", text, file);

  size_t bytes = write(fd, text, strlen(text));
  if (bytes < 0) {
    printf("Failed to write to file %s", file);
    syslog(LOG_ERR, "Failed to write to file %s", file);
    return 1;
  }

  if (close(fd) < 0) {
    printf("Failed to close file %s", file);
    syslog(LOG_ERR, "Failed to close file %s", file);
    return 1;
  }

  return 0;
}
