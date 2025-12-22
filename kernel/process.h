#ifndef _PROC_H_
#define _PROC_H_

#include "riscv.h"

#define MAX_FRAGMENTS 64

typedef struct trapframe_t {
  // space to store context (all common registers)
  /* offset:0   */ riscv_regs regs;

  // process's "user kernel" stack
  /* offset:248 */ uint64 kernel_sp;
  // pointer to smode_trap_handler
  /* offset:256 */ uint64 kernel_trap;
  // saved user process counter
  /* offset:264 */ uint64 epc;

  // kernel page table. added @lab2_1
  /* offset:272 */ uint64 kernel_satp;
}trapframe;

// the extremely simple definition of process, used for begining labs of PKE
typedef struct process_t {
  // pointing to the stack used in trap handling.
  uint64 kstack;
  // user page table
  pagetable_t pagetable;
  // trapframe storing the context of a (User mode) process.
  trapframe* trapframe;

  // simple heap management (Pascal-style)
  // virtual address of current heap page (highest allocated page)
  uint64 heap_page_va;
  // offset into current heap page 
  uint64 heap_offset;
  // free list 
  struct free_block {
    uint64 va;   // virtual address of block (header start)
    uint64 size; // total size including header
    struct free_block* next;
  } *free_list;
  // pool for free_block nodes 
  struct free_block free_pool[MAX_FRAGMENTS];
  int free_pool_count;
  
}process;

// switch to run user app
void switch_to(process*);

// current running process
extern process* current;

// address of the first free page in our simple heap. added @lab2_2
extern uint64 g_ufree_page;

#endif
