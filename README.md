# Lab 3: xv6 Threads

## 1. Introduction

This lab implements kernel-level thread support in xv6. We implement a new system call `clone()` to create kernel-level threads that share the parent's address space. Based on `clone()`, we build a user-level thread library consisting of `thread_create()`, `lock_acquire()`, and `lock_release()` for thread management. The implementation is verified using a multi-threaded frisbee simulation program.

## 2. Design Overview

### 2.1 Thread vs Process

The key difference between threads and processes lies in address space management:

| Aspect | Process (fork) | Thread (clone) |
|--------|---------------|----------------|
| Address Space | New copy | Shared with parent |
| Page Table | New page table | Share parent's page table |
| User Stack | Inherited (copied) | Caller-provided |
| Trapframe | New, mapped at TRAPFRAME | New, mapped at different location |
| Kernel Stack | New | New |

### 2.2 Trapframe Mapping Strategy

Each thread needs its own trapframe for saving/restoring registers during traps. To avoid overlap, we map each thread's trapframe to a unique virtual address:

```
Virtual Address Space (Top):
    ┌───────────────────┐ TRAMPOLINE
    │   trampoline      │ (shared, no re-mapping needed)
    ├───────────────────┤ TRAPFRAME
    │ Parent trapframe  │ (thread_id = 0)
    ├───────────────────┤ TRAPFRAME - PGSIZE
    │ Thread 1 trapframe│ (thread_id = 1)
    ├───────────────────┤ TRAPFRAME - 2*PGSIZE
    │ Thread 2 trapframe│ (thread_id = 2)
    └───────────────────┘
```

Formula: `TRAPFRAME - PGSIZE * thread_id`

## 3. Implementation Details

### 3.1 Data Structure Modifications

**File: `kernel/proc.h`**

Added `thread_id` field to `struct proc`:
```c
struct proc {
  // ... existing fields ...
  
  //********Lab3: Thread support*********** */
  int thread_id;        // 0 for parent process, >0 for child threads
};
```

- `thread_id = 0`: Parent process (normal process)
- `thread_id > 0`: Child thread created by clone()

### 3.2 Process Initialization

**File: `kernel/proc.c` - `allocproc()`**

Initialize `thread_id` to 0 for new processes:
```c
// Lab3: Initialize thread_id to 0 (parent process)
p->thread_id = 0;
```

### 3.3 Clone System Call Implementation

**File: `kernel/proc.c` - `clone()`**

The `clone()` system call creates a child thread:

```c
int clone(uint64 stack)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Basic sanity check: stack must not be null
  if(stack == 0)
    return -1;

  // Allocate process (PCB and trapframe)
  if((np = allocproc()) == 0){
    return -1;
  }

  // Calculate thread_id for the new thread
  int next_tid = 1;
  struct proc *pp;
  acquire(&wait_lock);
  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p && pp->thread_id >= next_tid){
      next_tid = pp->thread_id + 1;
    }
  }
  release(&wait_lock);
  
  np->thread_id = next_tid;

  // Share parent's page table - do NOT create a new one
  proc_freepagetable(np->pagetable, 0);
  np->pagetable = p->pagetable;

  // Map the new thread's trapframe to user space
  if(mappages(np->pagetable, TRAPFRAME - PGSIZE * np->thread_id, PGSIZE,
              (uint64)(np->trapframe), PTE_R | PTE_W) < 0){
    kfree((void*)np->trapframe);
    np->trapframe = 0;
    np->pagetable = 0;
    np->state = UNUSED;
    release(&np->lock);
    return -1;
  }

  // Copy parent's trapframe to child
  *(np->trapframe) = *(p->trapframe);

  // Set child's return value to 0
  np->trapframe->a0 = 0;
  
  // Set child's user stack
  np->trapframe->sp = stack;

  // Share memory size
  np->sz = p->sz;

  // Copy file descriptors
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));
  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}
```

Key implementation details:
1. **Stack parameter check**: Validates that the stack pointer is not null
2. **Thread ID assignment**: Finds the next available thread_id by scanning existing child threads
3. **Page table sharing**: Frees the page table created by `allocproc()` and shares parent's page table
4. **Trapframe mapping**: Maps thread's trapframe to `TRAPFRAME - PGSIZE * thread_id`
5. **Stack setup**: Sets the child's stack pointer to the caller-provided stack address

### 3.4 Modified freeproc()

**File: `kernel/proc.c` - `freeproc()`**

Modified to handle thread-specific cleanup:

```c
static void
freeproc(struct proc *p)
{
  if(p->trapframe) {
    // Lab3: If this is a thread (thread_id > 0), unmap its trapframe first
    if(p->thread_id > 0 && p->pagetable) {
      uvmunmap(p->pagetable, TRAPFRAME - PGSIZE * p->thread_id, 1, 0);
    }
    kfree((void*)p->trapframe);
  }
  p->trapframe = 0;
  
  // Lab3: Only free page table for parent process (thread_id == 0)
  if(p->thread_id == 0 && p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->thread_id = 0;
  p->state = UNUSED;
}
```

Key points:
- **Threads**: Only unmap their own trapframe; do NOT free the shared page table
- **Parent process**: Free the entire page table (including all memory)

### 3.5 Modified usertrapret()

**File: `kernel/trap.c` - `usertrapret()`**

Modified to pass correct trapframe address based on thread_id:

```c
// jump to userret in trampoline.S
uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);

// Lab3: Pass the correct trapframe address based on thread_id
if(p->thread_id == 0) {
  ((void (*)(uint64,uint64))trampoline_userret)(TRAPFRAME, satp);
} else {
  ((void (*)(uint64,uint64))trampoline_userret)(TRAPFRAME - PGSIZE * p->thread_id, satp);
}
```

### 3.6 Modified trampoline.S

**File: `kernel/trampoline.S`**

The new `userret` function accepts two parameters:
- `a0`: Trapframe address (varies by thread)
- `a1`: Page table (satp)

Key changes:
```asm
.globl userret
userret:
    # userret(TRAPFRAME, pagetable)
    # a0: TRAPFRAME, in user page table.
    # a1: user page table, for satp.

    # switch to the user page table.
    csrw satp, a1
    sfence.vma zero, zero

    # ... restore registers from trapframe at a0 ...
```

### 3.7 System Call Registration

**File: `kernel/syscall.h`**
```c
#define SYS_clone 27
```

**File: `kernel/syscall.c`**
```c
extern uint64 sys_clone(void);

static uint64 (*syscalls[])(void) = {
  // ... existing entries ...
  [SYS_clone]   sys_clone,
};
```

**File: `kernel/sysproc.c`**
```c
uint64 sys_clone(void){
  uint64 stack;
  argaddr(0, &stack);
  return clone(stack);
}
```

**File: `kernel/defs.h`**
```c
int             clone(uint64);
```

**File: `user/user.h`**
```c
int clone(void*);
```

**File: `user/usys.pl`**
```perl
entry("clone");
```

## 4. User-Level Thread Library

### 4.1 Thread Library Header

**File: `user/thread.h`**

```c
#ifndef _THREAD_H_
#define _THREAD_H_

typedef struct lock_t {
  uint locked;
} lock_t;

int thread_create(void *(*start_routine)(void*), void *arg);
void lock_init(lock_t* lock);
void lock_acquire(lock_t* lock);
void lock_release(lock_t* lock);

#endif
```

### 4.2 Thread Library Implementation

**File: `user/thread.c`**

**thread_create()**:
```c
int thread_create(void *(*start_routine)(void*), void *arg)
{
  // Allocate a user stack of PGSIZE bytes
  void *stack = malloc(PGSIZE);
  if(stack == 0)
    return -1;

  // Stack grows downward in RISC-V, so pass the top of the stack
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
```

**Spin Lock Implementation**:
```c
void lock_init(lock_t* lock)
{
  lock->locked = 0;
}

void lock_acquire(lock_t* lock)
{
  // Spin until we acquire the lock using atomic test-and-set
  while(__sync_lock_test_and_set(&lock->locked, 1) != 0)
    ;  // spin
}

void lock_release(lock_t* lock)
{
  __sync_lock_release(&lock->locked);
}
```

The spin lock uses GCC's built-in atomic operations:
- `__sync_lock_test_and_set`: Atomically sets the value to 1 and returns the previous value
- `__sync_lock_release`: Atomically sets the value to 0

## 5. Files Modified

1. **`kernel/proc.h`**: Added `thread_id` field to `struct proc`
2. **`kernel/proc.c`**:
   - Modified `allocproc()`: Initialize `thread_id` to 0
   - Added `clone()`: Create child thread with shared address space
   - Modified `freeproc()`: Handle thread-specific cleanup
3. **`kernel/trap.c`**: Modified `usertrapret()` to pass correct trapframe address
4. **`kernel/trampoline.S`**: New version accepting trapframe address as parameter
5. **`kernel/syscall.h`**: Added `SYS_clone` definition
6. **`kernel/syscall.c`**: Registered `sys_clone`
7. **`kernel/sysproc.c`**: Implemented `sys_clone()` handler
8. **`kernel/defs.h`**: Added `clone()` declaration
9. **`user/user.h`**: Added `clone()` user-space declaration
10. **`user/usys.pl`**: Added `clone` entry
11. **`user/thread.h`**: New file - thread library header
12. **`user/thread.c`**: New file - thread library implementation
13. **`user/lab3_test.c`**: New file - test program
14. **`Makefile`**: Set `CPUS := 3`, added `thread.o` to ULIB

## 6. Testing

The implementation was tested using the `lab3_test` frisbee simulation program:

```bash
# Build
make clean && make

# Run with 3 CPUs
make qemu CPUS=3

# Test commands in xv6 shell
$ lab3_test 6 4
$ lab3_test 10 3
$ lab3_test 21 20
```

### Test Results

**Test 1: lab3_test 6 4** (6 rounds, 4 threads)
```
$ lab3_test 6 4
Round 1: thread 0 is passing the token to thread 1
Round 2: thread 1 is passing the token to thread 2
Round 3: thread 2 is passing the token to thread 3
Round 4: thread 3 is passing the token to thread 0
Round 5: thread 0 is passing the token to thread 1
Round 6: thread 1 is passing the token to thread 2
Frisbee simulation has finished, 6 rounds played in total
```

**Test 2: lab3_test 10 3** (10 rounds, 3 threads)
```
$ lab3_test 10 3
Round 1: thread 0 is passing the token to thread 1
Round 2: thread 1 is passing the token to thread 2
Round 3: thread 2 is passing the token to thread 0
...
Round 10: thread 0 is passing the token to thread 1
Frisbee simulation has finished, 10 rounds played in total
```

**Test 3: lab3_test 21 20** (21 rounds, 20 threads)
```
$ lab3_test 21 20
Round 1: thread 0 is passing the token to thread 1
Round 2: thread 1 is passing the token to thread 2
...
Round 20: thread 19 is passing the token to thread 0
Round 21: thread 0 is passing the token to thread 1
Frisbee simulation has finished, 21 rounds played in total
```

All tests run on 3 emulated CPUs (verified by "hart 1 starting" and "hart 2 starting" messages).

