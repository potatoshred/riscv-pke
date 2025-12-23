/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"

#include "spike_interface/spike_utils.h"

// Global semaphore table. added @lab3_challenge_semaphore
#define MAX_SEMAPHORES_GLOBAL 128
semaphore global_semaphores[MAX_SEMAPHORES_GLOBAL];
int sem_alloc_id = 0;

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in direct mapping).
  assert( current );
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)buf);
  sprint(pa);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  // reclaim the current process, and reschedule. added @lab3_1
  free_process( current );
  schedule();
  return 0;
}

//
// maybe, the simplest implementation of malloc in the world ... added @lab2_2
//
uint64 sys_user_allocate_page() {
  void* pa = alloc_page();
  uint64 va;
  // if there are previously reclaimed pages, use them first (this does not change the
  // size of the heap)
  if (current->user_heap.free_pages_count > 0) {
    va =  current->user_heap.free_pages_address[--current->user_heap.free_pages_count];
    assert(va < current->user_heap.heap_top);
  } else {
    // otherwise, allocate a new page (this increases the size of the heap by one page)
    va = current->user_heap.heap_top;
    current->user_heap.heap_top += PGSIZE;

    current->mapped_info[HEAP_SEGMENT].npages++;
  }
  user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
         prot_to_type(PROT_WRITE | PROT_READ, 1));

  return va;
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_free_page(uint64 va) {
  user_vm_unmap((pagetable_t)current->pagetable, va, PGSIZE, 1);
  // add the reclaimed page to the free page list
  current->user_heap.free_pages_address[current->user_heap.free_pages_count++] = va;
  return 0;
}

//
// kerenl entry point of naive_fork
//
ssize_t sys_user_fork() {
  sprint("User call fork.\n");
  return do_fork( current );
}

//
// kerenl entry point of yield. added @lab3_2
//
ssize_t sys_user_yield() {
  current->status = READY;
  insert_to_ready_queue(current);
  schedule();
  return 0;

  return 0;
}

ssize_t sys_user_sem_new(int value) {
  // Find a free semaphore slot in global semaphore table
  if (sem_alloc_id >= MAX_SEMAPHORES_GLOBAL) {
    panic("Too many semaphores created.\n");
  }
  
  int sem_id = sem_alloc_id++;
  global_semaphores[sem_id].value = value;
  global_semaphores[sem_id].wait_queue = NULL;
  
  return sem_id;
}

ssize_t sys_user_sem_P(int sem_id) {
    // P operation (wait/test): decrement semaphore value
    // If value < 0, block current process and wait on the semaphore
    if (sem_id < 0 || sem_id >= sem_alloc_id) {
      panic("Invalid semaphore id: %d\n", sem_id);
    }
    
    semaphore *sem = &global_semaphores[sem_id];
    sem->value--;
    
    if (sem->value < 0) {
      // Block current process by adding it to the wait queue
      current->status = BLOCKED;
      
      // Add current process to the semaphore's wait queue
      if (sem->wait_queue == NULL) {
        current->queue_next = NULL;
        sem->wait_queue = current;
      } else {
        // Add to the end of wait queue
        process* p = sem->wait_queue;
        while (p->queue_next != NULL) {
          p = p->queue_next;
        }
        current->queue_next = NULL;
        p->queue_next = current;
      }
      
      // Schedule another process
      schedule();
    }
    return 0;
}

ssize_t sys_user_sem_V(int sem_id) {
    // V operation (signal/release): increment semaphore value
    // If there are processes waiting, wake one up
    if (sem_id < 0 || sem_id >= sem_alloc_id) {
      panic("Invalid semaphore id: %d\n", sem_id);
    }
    
    semaphore *sem = &global_semaphores[sem_id];
    sem->value++;
    
    if (sem->value <= 0 && sem->wait_queue != NULL) {
      // Wake up the first process in the wait queue
      process* wake_proc = sem->wait_queue;
      sem->wait_queue = wake_proc->queue_next;
      wake_proc->queue_next = NULL;
      
      // Change process status to READY and insert to ready queue
      wake_proc->status = READY;
      insert_to_ready_queue(wake_proc);
    }
    return 0;
}
  
//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    // added @lab2_2
    case SYS_user_allocate_page:
      return sys_user_allocate_page();
    case SYS_user_free_page:
      return sys_user_free_page(a1);
    case SYS_user_fork:
      return sys_user_fork();
    case SYS_user_yield:
      return sys_user_yield();
    case SYS_user_sem_new:
      return sys_user_sem_new(a1);
    case SYS_user_sem_P:
      return sys_user_sem_P(a1);
    case SYS_user_sem_V:
      return sys_user_sem_V(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
