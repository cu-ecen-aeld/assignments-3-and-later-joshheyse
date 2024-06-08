#include "threading.h"
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <unistd.h>

// Optional: use these functions to add debug or error prints to your
// application
// #define DEBUG_LOG(msg, ...)
#define DEBUG_LOG(msg, ...) printf("threading: " msg "\n", ##__VA_ARGS__)
#define ERROR_LOG(msg, ...) printf("threading ERROR: " msg "\n", ##__VA_ARGS__)

void *threadfunc(void *thread_param) {

  // TODO: wait, obtain mutex, wait, release mutex as described by thread_data
  // structure hint: use a cast like the one below to obtain thread arguments
  // from your parameter
  // struct thread_data* thread_func_args = (struct thread_data *) thread_param;
  //
  struct thread_data *data = (struct thread_data *)thread_param;
  data->thread_complete_success = false;
  DEBUG_LOG("%s: wait_to_obtain_ms: %d, wait_to_release_ms: %d", __func__,
            data->wait_to_obtain_ms, data->wait_to_release_ms);

  usleep(data->wait_to_obtain_ms * 1000);
  DEBUG_LOG("%s: Obtaining mutex", __func__);
  pthread_mutex_lock(data->mutex);
  DEBUG_LOG("%s: Obtained mutex", __func__);
  usleep(data->wait_to_release_ms * 1000);
  DEBUG_LOG("%s: Releasing mutex", __func__);
  pthread_mutex_unlock(data->mutex);
  DEBUG_LOG("%s: Released mutex", __func__);
  data->thread_complete_success = true;
  return thread_param;
}

bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,
                                  int wait_to_obtain_ms,
                                  int wait_to_release_ms) {
  /**
   * TODO: allocate memory for thread_data, setup mutex and wait arguments, pass
   * thread_data to created thread using threadfunc() as entry point.
   *
   * return true if successful.
   *
   * See implementation details in threading.h file comment block
   */
  struct thread_data *data =
      (struct thread_data *)malloc(sizeof(struct thread_data));
  data->wait_to_obtain_ms = wait_to_obtain_ms;
  data->wait_to_release_ms = wait_to_release_ms;
  data->mutex = mutex;
  return pthread_create(thread, NULL, threadfunc, data) == 0;
}
