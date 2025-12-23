/*
 * Utility functions for trap handling in Supervisor mode.
 */

#include "strap.h"
#include "process.h"
#include "riscv.h"
#include "syscall.h"
#include "pmm.h"
#include "vmm.h"
#include "util/functions.h"
#include "util/string.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"

//
// handling the syscalls. will call do_syscall() defined in kernel/syscall.c
//
static void handle_syscall(trapframe* tf) {
    // tf->epc points to the address that our computer will jump to after the
    // trap handling. for a syscall, we should return to the NEXT instruction
    // after its handling. in RV64G, each instruction occupies exactly 32 bits
    // (i.e., 4 Bytes)
    tf->epc += 4;

    tf->regs.a0 =
        do_syscall(tf->regs.a0, tf->regs.a1, tf->regs.a2, tf->regs.a3,
                   tf->regs.a4, tf->regs.a5, tf->regs.a6, tf->regs.a7);
}

//
// global variable that store the recorded "ticks". added @lab1_3
static uint64 g_ticks = 0;
//
// added @lab1_3
//
void handle_mtimer_trap() {
  sprint("Ticks %d\n", g_ticks);
  ++g_ticks;
  write_csr(sip, 0L);

}

//
// the page fault handler. added @lab2_3. parameters:
// sepc: the pc when fault happens;
// stval: the virtual address that causes pagefault when being accessed.
//
void handle_user_page_fault(uint64 mcause, uint64 sepc, uint64 stval) {
  uint64 hartid = read_tp();
  sprint("handle_page_fault: %lx\n", stval);
  switch (mcause) {
    case CAUSE_STORE_PAGE_FAULT:
    case CAUSE_LOAD_PAGE_FAULT: 
      if (stval <= USER_FREE_ADDRESS_START || stval > USER_STACK_TOP) {
        sprint("stack region: [%lx, %lx] stval=%lx\n", USER_FREE_ADDRESS_START, USER_STACK_TOP, stval);
        panic("panic by stack overflow\n");
      }

      uint64 va = ROUNDDOWN(stval, PGSIZE);
      void *pa = alloc_page();
      if (!pa) panic("out of physical memory when handling page fault\n");
      memset(pa, 0, PGSIZE);
      // map with user read/write permissions
      int perm = prot_to_type(PROT_READ | PROT_WRITE, 1);
      user_vm_map(current[hartid]->pagetable, va, PGSIZE, (uint64)pa, perm);
      break;
    default:
      sprint("unknown page fault.\n");
      break;
  }
}

//
// kernel/smode_trap.S will pass control to smode_trap_handler, when a trap happens
// in S-mode.
//
void smode_trap_handler(void) {
  uint64 hartid = read_tp();
    // make sure we are in User mode before entering the trap handling.
    // we will consider other previous case in lab1_3 (interrupt).
    if ((read_csr(sstatus) & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    assert(current[hartid]);
    // save user process counter.
    current[hartid]->trapframe->epc = read_csr(sepc);

  // if the cause of trap is syscall from user application.
  // read_csr() and CAUSE_USER_ECALL are macros defined in kernel/riscv.h
  uint64 cause = read_csr(scause);

  // use switch-case instead of if-else, as there are many cases since lab2_3.
  switch (cause) {
    case CAUSE_USER_ECALL:
      handle_syscall(current[hartid]->trapframe);
      break;
    case CAUSE_MTIMER_S_TRAP:
      handle_mtimer_trap();
      break;
    case CAUSE_STORE_PAGE_FAULT:
    case CAUSE_LOAD_PAGE_FAULT:
      // the address of missing page is stored in stval
      // call handle_user_page_fault to process page faults
      handle_user_page_fault(cause, read_csr(sepc), read_csr(stval));
      break;
    default:
      sprint("smode_trap_handler(): unexpected scause %p\n", read_csr(scause));
      sprint("            sepc=%p stval=%p\n", read_csr(sepc), read_csr(stval));
      panic( "unexpected exception happened.\n" );
      break;
  }

    // continue (come back to) the execution of current process.
    switch_to(current[hartid]);
}
