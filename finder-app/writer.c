#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

int main(int argc, char **argv) {
  openlog("writer", LOG_PID, LOG_USER);

  if (argc < 2) {
    printf("Usage: writer <file> <string>\n");
    syslog(LOG_ERR, "Invalid arguments");
    return 1;
  }

  int fd = open(argv[0], O_WRONLY | O_TRUNC | O_CREAT);
  if (fd < 0) {
    printf("Failed to open file %s", argv[0]);
    syslog(LOG_ERR, "Failed to open file %s", argv[0]);
    return 1;
  }

  syslog(LOG_DEBUG, "Writing %s to %s", argv[0], argv[1]);

  size_t bytes = write(fd, argv[1], strlen(argv[1]));
  if (bytes < 0) {
    printf("Failed to write to file %s", argv[0]);
    syslog(LOG_ERR, "Failed to write to file %s", argv[0]);
    return 1;
  }

  if (close(fd) < 0) {
    printf("Failed to close file %s", argv[0]);
    syslog(LOG_ERR, "Failed to close file %s", argv[0]);
    return 1;
  }

  return 0;
}
