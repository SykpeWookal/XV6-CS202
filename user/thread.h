#ifndef _THREAD_H_
#define _THREAD_H_

// Lab3: User-level thread library

typedef struct lock_t {
  uint locked;
} lock_t;

int thread_create(void *(*start_routine)(void*), void *arg);
void lock_init(lock_t* lock);
void lock_acquire(lock_t* lock);
void lock_release(lock_t* lock);

#endif

