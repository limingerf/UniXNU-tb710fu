/* Stub: see IOKit/OSObject.h. */
#ifndef STUB_KERN_THREAD_H
#define STUB_KERN_THREAD_H
#include <stdint.h>

typedef int kern_return_t;
#define KERN_SUCCESS 0

typedef struct thread *thread_t;
typedef int wait_result_t;
typedef void (*thread_continuation_t)(void *parameter, wait_result_t result);

thread_t current_thread();
kern_return_t kernel_thread_start(thread_continuation_t continuation, void *parameter, thread_t *new_thread);
void thread_terminate(thread_t thread);
void thread_deallocate(thread_t thread);

#endif
