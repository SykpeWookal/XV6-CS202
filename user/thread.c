#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/thread.h"

// Lab3: User-level thread library implementation

#define PGSIZE 4096

// Create a new thread
// Returns 0 on success for parent, -1 on failure
// Child thread calls start_routine(arg) and then exits
int thread_create(void *(*start_routine)(void*), void *arg)
{
  // Allocate a user stack of PGSIZE bytes
  void *stack = malloc(PGSIZE);
  if(stack == 0)
    return -1;

  // Stack grows downward in RISC-V, so pass the top of the stack
  // (stack + PGSIZE) to clone()
  int ret = clone((char*)stack + PGSIZE);
  
  if(ret == 0) {
    // Child thread: execute the start routine and exit
    (*start_routine)(arg);
    exit(0);
  } else if(ret > 0) {
    // Parent: clone succeeded, return 0
    return 0;
  } else {
    // Clone failed, free the allocated stack
    free(stack);
    return -1;
  }
}

// Initialize a lock
void lock_init(lock_t* lock)
{
  lock->locked = 0;
}

// Acquire a lock using atomic test-and-set
void lock_acquire(lock_t* lock)
{
  // Spin until we acquire the lock
  // __sync_lock_test_and_set atomically sets lock->locked to 1
  // and returns the previous value
  while(__sync_lock_test_and_set(&lock->locked, 1) != 0)
    ;  // spin
}

// Release a lock
void lock_release(lock_t* lock)
{
  // __sync_lock_release atomically sets lock->locked to 0
  __sync_lock_release(&lock->locked);
}

