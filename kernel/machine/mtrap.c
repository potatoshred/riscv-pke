#include "kernel/riscv.h"
#include "kernel/process.h"
#include "spike_interface/spike_utils.h"
#include "util/string.h"

static void handle_instruction_access_fault() { panic("Instruction access fault!"); }

static void handle_load_access_fault() { panic("Load access fault!"); }

static void handle_store_access_fault() { panic("Store/AMO access fault!"); }

static void handle_illegal_instruction() { 
  // https://zhuanlan.zhihu.com/p/642441074
  char *dir_path = NULL;
  char *file_name= NULL;
  int line_number = 0;

  uint64 pc = read_csr(mepc); 

  // find line 
  for (int i = 0; i < current->line_ind; i++) {
    if(pc == current->line[i].addr){
      line_number = current->line[i].line;
      file_name = current->file[current->line[i].file].file;
      dir_path = current->dir[current->file[current->line[i].file].dir];
      break;
    }
  }
  if (dir_path == NULL || file_name == NULL){
    sprint("handle_illegal_instruction: cannot find debug info of source code!\n");
  }else{
    char full_path[256];
    strcpy(full_path, dir_path);
    strcpy(full_path + strlen(dir_path), "/");
    strcpy(full_path + strlen(dir_path) + 1, file_name);
    sprint("Runtime error at %s:%d\n", full_path, line_number);
    char line_buf[512];
    
    spike_file_t* f = spike_file_open(full_path, O_RDONLY, 0);
    if (IS_ERR_VALUE(f)) {
      sprint("handle_illegal_instruction: cannot open source code file %s!\n", full_path);
      return;
    }
    size_t seek = 0;
    size_t offset = 0;
    size_t read_bytes = 0;
    while (1) {
      char c;
      read_bytes = spike_file_pread(f, &c, 1, seek++);
      if (read_bytes == 0) {
        sprint("handle_illegal_instruction: cannot find line %d in source code file %s!\n", line_number, full_path);
        break;
      }
      if (c == '\n') {
        line_number--;
        if (line_number == 0) {
          line_buf[offset] = '\0';
          sprint("%s\n", line_buf);
          break;
        }
        offset = 0;
      } else {
        line_buf[offset++] = c;
      }
    }
    spike_file_close(f);
  }
  panic("Illegal instruction!"); 

}

static void handle_misaligned_load() { panic("Misaligned Load!"); }

static void handle_misaligned_store() { panic("Misaligned AMO!"); }

// added @lab1_3
static void handle_timer() {
  int cpuid = 0;
  // setup the timer fired at next time (TIMER_INTERVAL from now)
  *(uint64*)CLINT_MTIMECMP(cpuid) = *(uint64*)CLINT_MTIMECMP(cpuid) + TIMER_INTERVAL;

  // setup a soft interrupt in sip (S-mode Interrupt Pending) to be handled in S-mode
  write_csr(sip, SIP_SSIP);
}

//
// handle_mtrap calls a handling function according to the type of a machine mode interrupt (trap).
//
void handle_mtrap() {
  uint64 mcause = read_csr(mcause);
  switch (mcause) {
    case CAUSE_MTIMER:
      handle_timer();
      break;
    case CAUSE_FETCH_ACCESS:
      handle_instruction_access_fault();
      break;
    case CAUSE_LOAD_ACCESS:
      handle_load_access_fault();
    case CAUSE_STORE_ACCESS:
      handle_store_access_fault();
      break;
    case CAUSE_ILLEGAL_INSTRUCTION:
      // TODO (lab1_2): call handle_illegal_instruction to implement illegal instruction
      // interception, and finish lab1_2.
      // panic( "call handle_illegal_instruction to accomplish illegal instruction interception for lab1_2.\n" );
      handle_illegal_instruction();
      break;
    case CAUSE_MISALIGNED_LOAD:
      handle_misaligned_load();
      break;
    case CAUSE_MISALIGNED_STORE:
      handle_misaligned_store();
      break;

    default:
      sprint("machine trap(): unexpected mscause %p\n", mcause);
      sprint("            mepc=%p mtval=%p\n", read_csr(mepc), read_csr(mtval));
      panic( "unexpected exception happened in M-mode.\n" );
      break;
  }
}
