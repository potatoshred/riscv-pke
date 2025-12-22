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
#include "spike_interface/spike_utils.h"

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
  // in lab1, PKE considers only one app (one process). 
  // therefore, shutdown the system when the app calls exit()
  shutdown(code);
}



// align up to 8
static inline uint64 align8(uint64 x) { return (x + 7) & ~((uint64)7); }

// Pascal-style user heap management
uint64 sys_user_allocate_page(uint64 n) {
  assert(current);
  // header stores payload size
  const uint64 hdr = sizeof(uint64);
  uint64 need = align8(n);
  uint64 total = need + hdr;

  // search free list for suitable block
  struct free_block *prev = NULL;
  struct free_block *fb = current->free_list;
  while (fb) {
    if (fb->size >= total) {
      // use this block
      uint64 ret_va = fb->va + hdr; // return pointer after header

      // split
      if (fb->size >= total + 16) {
        uint64 new_va = fb->va + total;
        uint64 new_size = fb->size - total;
        fb->va = new_va;
        fb->size = new_size;
      } else {
        // remove fb from list
        if (prev) prev->next = fb->next; else current->free_list = fb->next;
        
      }
      // write header
      char *hpa = (char*)user_va_to_pa((pagetable_t)current->pagetable, (void*)(ret_va - hdr));
      *(uint64*)hpa = need;
      return ret_va;
    }
    prev = fb;
    fb = fb->next;
  }

  // no free block; bump allocate in current heap page
  // If no current heap page, or remaining in current page insufficient,
  // allocate enough new physical pages. Try to allocate contiguously in
  // virtual space so large allocations can span pages and be adjacent to
  // previous allocations.
  if (current->heap_page_va == 0) {
    // need to allocate enough pages to cover total
    uint64 npages = (total + PGSIZE - 1) / PGSIZE;
    // allocate physical pages
    void *first_pa = alloc_page();
    if (first_pa == 0) return 0;
    void **allocated = 0; // we won't store pointers, but free if later fail
    for (uint64 i = 1; i < npages; i++) {
      void *p = alloc_page();
      if (p == 0) {
        // if allocation fails, return 0 (note: no cleanup of already allocated pages)
        return 0;
      }
    }

    uint64 va = g_ufree_page;
    // map all pages (size is page-aligned)
    user_vm_map((pagetable_t)current->pagetable, va, npages * PGSIZE, (uint64)first_pa,
           prot_to_type(PROT_WRITE | PROT_READ, 1));
    g_ufree_page += npages * PGSIZE;

    // insert remaining space as free block (unused bytes at end)
    uint64 used = total;
    uint64 free_bytes = npages * PGSIZE - used;
    if (free_bytes > 0) {
      if (current->free_pool_count < MAX_FRAGMENTS) {
        struct free_block *node = &current->free_pool[current->free_pool_count++];
        node->va = va + used;
        node->size = free_bytes;
        node->next = current->free_list;
        current->free_list = node;
      }
    }

    // set heap_page_va to last page and heap_offset to offset in last page
    current->heap_page_va = va + (npages - 1) * PGSIZE;
    current->heap_offset = used <= PGSIZE ? used : (used - (npages - 1) * PGSIZE);

    uint64 alloc_va = va;
    // write header (at alloc_va)
    char *hpa = (char*)user_va_to_pa((pagetable_t)current->pagetable, (void*)alloc_va);
    *(uint64*)hpa = need;
    return alloc_va + hdr;
  } else if ((PGSIZE - current->heap_offset) >= total) {
    // enough space in current page
    uint64 alloc_va = current->heap_page_va + current->heap_offset;
    char *hpa = (char*)user_va_to_pa((pagetable_t)current->pagetable, (void*)alloc_va);
    *(uint64*)hpa = need;
    current->heap_offset += total;
    return alloc_va + hdr;
  } else {
    // need extra pages beyond current page
    uint64 alloc_va = current->heap_page_va + current->heap_offset;
    uint64 remaining = PGSIZE - current->heap_offset;
    uint64 need_extra = total - remaining;
    uint64 npages_extra = (need_extra + PGSIZE - 1) / PGSIZE;

    void *first_pa = alloc_page();
    if (first_pa == 0) return 0;
    for (uint64 i = 1; i < npages_extra; i++) {
      void *p = alloc_page();
      if (p == 0) {
        return 0;
      }
    }

    uint64 new_va = g_ufree_page;
    user_vm_map((pagetable_t)current->pagetable, new_va, npages_extra * PGSIZE, (uint64)first_pa,
           prot_to_type(PROT_WRITE | PROT_READ, 1));
    g_ufree_page += npages_extra * PGSIZE;

    // insert remaining bytes from the newly allocated pages (if any)
    uint64 free_bytes = npages_extra * PGSIZE - need_extra;
    if (free_bytes > 0) {
      if (current->free_pool_count < MAX_FRAGMENTS) {
        struct free_block *node = &current->free_pool[current->free_pool_count++];
        node->va = new_va + need_extra;
        node->size = free_bytes;
        node->next = current->free_list;
        current->free_list = node;
      }
    }

    // update heap_page_va to last allocated page and heap_offset inside it
    current->heap_page_va = new_va + (npages_extra - 1) * PGSIZE;
    current->heap_offset = need_extra % PGSIZE;
    if (current->heap_offset == 0) current->heap_offset = 0;

    // write header at alloc_va
    char *hpa = (char*)user_va_to_pa((pagetable_t)current->pagetable, (void*)alloc_va);
    *(uint64*)hpa = need;
    return alloc_va + hdr;
  }
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_free_page(uint64 va) {
  assert(current);

  // rough implementation: no coalescing, no page freeing

  // va is pointer to user payload; header stored just before it
  uint64 hdr_va = va - sizeof(uint64);
  char *hpa = (char*)user_va_to_pa((pagetable_t)current->pagetable, (void*)hdr_va);
  if (!hpa) return 0;
  uint64 payload = *(uint64*)hpa;
  uint64 total = payload + sizeof(uint64);

  // insert a free_block node
  if (current->free_pool_count < MAX_FRAGMENTS) {
    struct free_block *node = &current->free_pool[current->free_pool_count++];
    node->va = hdr_va;
    node->size = total;
    node->next = current->free_list;
    current->free_list = node;
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
      return sys_user_allocate_page(a1);
    case SYS_user_free_page:
      return sys_user_free_page(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
