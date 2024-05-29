
#include "threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

// maximum number of threads allowed in a pool
#define MAXT_IN_POOL 200


typedef struct {
    //threadpool *from_me;
    int client_socket;
    // Add other necessary members
} Data;
int counter = 0;



//initialization
threadpool *create_threadpool(int num_threads_in_pool) {
    // Input sanity check
    if (num_threads_in_pool <= 0 || num_threads_in_pool > MAXT_IN_POOL) {
        fprintf(stderr, "Usage: from_me <from_me-size> <number-of-tasks> <max-number-of-request>\n");
        exit(EXIT_FAILURE);
    }

    // Allocate memory for the threadpool structure
    threadpool *from_me = (threadpool *) malloc(sizeof(threadpool));
    if (from_me == NULL) {
        perror("Failed to allocate memory for threadpool");
        return NULL;
    }

    // Initialize other fields in the threadpool structure
    from_me->num_threads = num_threads_in_pool;
    from_me->qsize = 0;

    // Allocate memory for the threads array
    from_me->threads = (pthread_t *) malloc(num_threads_in_pool * sizeof(pthread_t));
    if (from_me->threads == NULL) {
        perror("Failed to allocate memory for threads array");
        free(from_me);
        exit(EXIT_FAILURE);
    }

    from_me->qhead = NULL;
    from_me->qtail = NULL;

    // Initialize mutex and condition variables
    if (pthread_mutex_init(&from_me->qlock, NULL) != 0) {
        perror("error: pthread_mutex_init");
        free(from_me->threads);
        free(from_me);
        return NULL;
    }
    if (pthread_cond_init(&from_me->q_not_empty, NULL) != 0) {
        perror("error: pthread_cond_init");
        pthread_mutex_destroy(&from_me->qlock);
        free(from_me->threads);
        free(from_me);
        return NULL;
    }
    if (pthread_cond_init(&from_me->q_empty, NULL) != 0) {
        perror("error: pthread_cond_init");
        pthread_mutex_destroy(&from_me->qlock);
        pthread_cond_destroy(&from_me->q_not_empty);
        free(from_me->threads);
        free(from_me);
        return NULL;
    }

    from_me->shutdown = 0;
    from_me->dont_accept = 0;


    // Create threads
    for (int i = 0; i < num_threads_in_pool; i++) {
        if (pthread_create(&from_me->threads[i], NULL, do_work, from_me) != 0) {
            perror("Thread creation failed");
            // Handle error and clean up
            destroy_threadpool(from_me);
            return NULL;
        }
    }





    return from_me;
}

void *do_work(void *p) {
    threadpool *from_me = (threadpool *) p;

    while (1) {
        // Lock the queue
        pthread_mutex_lock(&from_me->qlock);

        // Wait until there is work to do
        while (from_me->qsize == 0 && !from_me->shutdown) {
            // If the from_me is not shutting down and the queue is empty, wait
            pthread_cond_wait(&from_me->q_not_empty, &from_me->qlock);
        }

        // Check if the from_me is shutting down
        if (from_me->shutdown) {
            pthread_mutex_unlock(&from_me->qlock);
            pthread_exit(NULL);
        }

        // Take the first element from the queue
        work_t *work = from_me->qhead;
        from_me->qhead = work->next;

        // If the queue becomes empty, signal that it's empty
        if (from_me->qhead == NULL) {
            pthread_cond_signal(&from_me->q_empty);
        }

        // Decrement the queue size
        from_me->qsize--;

        // Unlock the queue
        pthread_mutex_unlock(&from_me->qlock);

        // Execute the thread routine
        work->routine(work->arg);

        // Free the memory allocated for the work element
        free(work);
    }

    // Unreachable point
    // pthread_exit(NULL);
}

//enter work to the line
void dispatch(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg) {
    // Cast the argument back to DispatchData*

    // Now you can access the individual members of the structure
    // Create and initialize a new work_t element
    work_t *new_work = (work_t *) malloc(sizeof(work_t));
    if (new_work == NULL) {
        perror("Failed to allocate memory for new work element");
        exit(EXIT_FAILURE);
    }

    new_work->routine = dispatch_to_here;
    new_work->arg = arg;
    new_work->next = NULL;

    // Lock the queue
    pthread_mutex_lock(&from_me->qlock);

    if(from_me->dont_accept)
    {
        pthread_mutex_unlock(&from_me->qlock);
        free(new_work);
        return;
    }

    // Add the new work element to the queue
    if (from_me->qsize == 0) {
        // If the queue is empty, update both head and tail pointers
        from_me->qhead = new_work;
        from_me->qtail = new_work;
        pthread_cond_signal(&from_me->q_not_empty);  // Signal that the queue is not empty
    } else {
        // Otherwise, add the new work element to the tail
        from_me->qtail->next = new_work;
        from_me->qtail = new_work;
    }

    // Increment the queue size
    from_me->qsize++;

    // Unlock the queue
    pthread_mutex_unlock(&from_me->qlock);
}

void destroy_threadpool(threadpool *destroyme) {
    // Lock the queue before making any changes
    pthread_mutex_lock(&destroyme->qlock);

    // Check if the destruction process has already begun
    if (destroyme->dont_accept) {
        // Unlock the queue and return if already in the destruction process
        pthread_mutex_unlock(&destroyme->qlock);
        return;
    }

    // Set the destruction flag to indicate the beginning of the destruction process
    destroyme->dont_accept = 1;

    destroyme->shutdown=1;
    // Wake up any waiting threads
    pthread_cond_broadcast(&destroyme->q_not_empty);

    // Unlock the queue
    pthread_mutex_unlock(&destroyme->qlock);

    // Wait for all threads to finish
    for (int i = 0; i < destroyme->num_threads; i++) {
        if (pthread_join(destroyme->threads[i], NULL) != 0) {
            perror("pthread_join failed");
            exit(EXIT_FAILURE);
        }
    }
    // Free the memory associated with the threads array
    free(destroyme->threads);

    // Free the memory associated with the thread from_me structure
    free(destroyme);
}

