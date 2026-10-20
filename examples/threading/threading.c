#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
    struct thread_data* thread_func_args = (struct thread_data *) thread_param;
    thread_func_args->thread_complete_success = false;

    if (usleep(thread_func_args->wait_to_obtain_ms * 1000) != 0) {
        ERROR_LOG("usleep before obtaining mutex failed");
        return thread_param;
    }

    int rc = pthread_mutex_lock(thread_func_args->mutex);
    if (rc != 0) {
        ERROR_LOG("pthread_mutex_lock failed with %d", rc);
        return thread_param;
    }
    DEBUG_LOG("mutex obtained");

    bool slept = (usleep(thread_func_args->wait_to_release_ms * 1000) == 0);
    if (!slept)
        ERROR_LOG("usleep before releasing mutex failed");

    // always release the mutex, even if the sleep was interrupted
    rc = pthread_mutex_unlock(thread_func_args->mutex);
    if (rc != 0) {
        ERROR_LOG("pthread_mutex_unlock failed with %d", rc);
        return thread_param;
    }
    DEBUG_LOG("mutex released");

    thread_func_args->thread_complete_success = slept;
    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    // freed by the thread joiner, using the pointer returned by threadfunc()
    struct thread_data *data = malloc(sizeof(struct thread_data));
    if (data == NULL) {
        ERROR_LOG("could not allocate thread_data");
        return false;
    }

    data->mutex = mutex;
    data->wait_to_obtain_ms = wait_to_obtain_ms;
    data->wait_to_release_ms = wait_to_release_ms;
    data->thread_complete_success = false;

    int rc = pthread_create(thread, NULL, threadfunc, data);
    if (rc != 0) {
        ERROR_LOG("pthread_create failed with %d", rc);
        free(data);
        return false;
    }

    return true;
}

