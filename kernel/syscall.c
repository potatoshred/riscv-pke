/*
 * contains the implementation of all syscalls.
 */

#include <errno.h>
#include <stdint.h>

#include "process.h"
#include "string.h"
#include "syscall.h"
#include "util/functions.h"
#include "util/types.h"

#include "elf.h"

#include "spike_interface/spike_utils.h"

// from kernel/elf.c
typedef union {
    uint64 buf[MAX_CMDLINE_ARGS];
    char* argv[MAX_CMDLINE_ARGS];
} arg_buf;

extern size_t parse_args(arg_buf* arg_bug_msg);

// offset[20|10:1|11|19:12]
static uint64_t imm_permute(uint64 pc, uint32 imm) {
    uint32 mask1 = (1 << 1) - 1;
    uint32 mask2 = (1 << 10) - 1;
    uint32 mask3 = (1 << 1) - 1;
    uint32 mask4 = (1 << 8) - 1;

    uint32 seg1 = (imm >> 19) & mask1;
    uint32 seg2 = (imm >> 9) & mask2;
    uint32 seg3 = (imm >> 8) & mask3;
    uint32 seg4 = (imm >> 0) & mask4;

    uint32 seg0 = 0;  // for signExt
    if (seg1) {
        seg0 = 0xffe << 20;
    }

    uint32 imm_permu =
        seg0 | (seg1 << 20) | (seg2 << 1) | (seg3 << 11) | (seg4 << 12);
    // sprint("imm: %x imm_permu: %x ", imm, imm_permu);

    return pc + imm_permu;
}

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
    sprint(buf);
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

ssize_t sys_user_backtrace(size_t n) {
    process* p = current;

    arg_buf arg_bug_msg;

    // from kernel/elf.c
    // retrieve command line arguements
    size_t argc = parse_args(&arg_bug_msg);
    if (!argc)
        panic("You need to specify the application program!\n");

    // symbol lookup
    spike_file_t* f = spike_file_open(arg_bug_msg.argv[0], O_RDONLY, 0);
    if (IS_ERR_VALUE(f))
        panic("Cannot open current process's elf file: %s\n",
              arg_bug_msg.argv[0]);

    // sprint("Backtrace:\n");

    // parse elf header to get section info
    elf_header eh;
    spike_file_pread(f, &eh, sizeof(eh), 0);

    // locate symtab and its associated strtab by scanning section headers
    uint64 shoff = eh.shoff;
    uint16 shnum = eh.shnum;
    uint16 shentsize = eh.shentsize;
    uint16 shstrndx = eh.shstrndx;

    uint64 shstrtaboff = shoff + shstrndx * shentsize;

    Elf64_Shdr shstrtab;  // section header of .shstrtab
    Elf64_Shdr shdr;      // for traverse
    spike_file_pread(f, &shstrtab, sizeof(shstrtab), shstrtaboff);

    char shstrtab_buf[4096];  // copy of .shstrtab
    spike_file_pread(f, shstrtab_buf, shstrtab.sh_size, shstrtab.sh_offset);
    int symtabndx = -1;
    int strtabndx = -1;
    int textndx = -1;
    for (int i = 0; i < shnum; i++) {
        uint64 off = shoff + (uint64)i * shentsize;
        spike_file_pread(f, &shdr, sizeof(shdr), off);
        // sprint("shname %d: %s\n", i, shstrtab_buf + shdr.sh_name);
        char* shname = shstrtab_buf + shdr.sh_name;
        if (strcmp(shname, ".symtab") == 0) {
            // sprint("Found .symtab at index %d\n", i);
            // store symtab index
            symtabndx = i;
        } else if (strcmp(shname, ".strtab") == 0) {
            // sprint("Found .strtab at index %d\n", i);
            // store strtab index
            strtabndx = i;
        } else if (strcmp(shname, ".text") == 0) {
            // sprint("Found .strtab at index %d\n", i);
            // store text index
            textndx = i;
        }
    }

    // read symtab section header
    Elf64_Shdr symtab;
    uint64 symtab_off = shoff + symtabndx * shentsize;
    spike_file_pread(f, &symtab, sizeof(symtab), symtab_off);

    // read strtab section
    Elf64_Shdr strtab;
    uint64 strtab_off = shoff + strtabndx * shentsize;
    char strtab_buf[8192];  // copy of .strtab
    spike_file_pread(f, &strtab, sizeof(strtab), strtab_off);
    spike_file_pread(f, strtab_buf, strtab.sh_size, strtab.sh_offset);

    uint32_t symnames[128];  // offset in .strtab
    uint64_t symvals[128];   // vaddr of symbol
    // traverse symbols for function name looking up
    for (int i = 0; i < symtab.sh_size / symtab.sh_entsize; i++) {
        Elf64_Sym sym;
        uint64 sym_off = symtab.sh_offset + i * symtab.sh_entsize;
        spike_file_pread(f, &sym, sizeof(sym), sym_off);
        symvals[i] = sym.st_value;
        symnames[i] = sym.st_name;
        // sprint("symname %d: %016x, %s\n", i, symvals[i], strtab_buf +
        // symnames[i]);
    }

    int cnt = 0;
    uint64 fp = p->trapframe->regs.s0;
    while (cnt < n + 2) {
        if (fp == USER_STACK) {
            sprint("main\n");
            break;
        }
        uint64 prev_fp;
        uint64 ret_addr;
        if (cnt == 0) {
            ret_addr = -1;  // first frame, no return address
            prev_fp = *((uint64*)fp - 1);
        } else {
            ret_addr = *((uint64*)fp - 1) - 4;
            prev_fp = *((uint64*)fp - 2);
            if (cnt != 1) {
                /* RISC‑V 中有压缩指令（16 位），因此返回地址附近
                的指令地址不一定对齐。执行四次单字节访问，字节访问
                对齐要求最低、不会触发 Misaligned Load*/
                uint32 jal_off;  // .text属于LOAD，直接访存

                uint8_t* ip = (uint8_t*)(uintptr_t)ret_addr;
                jal_off = (uint32)ip[0] | ((uint32)ip[1] << 8) |
                          ((uint32)ip[2] << 16) | ((uint32)ip[3] << 24);

                // sprint("jal_off:%x symaddr:%x ", jal_off,
                //        imm_permute(ret_addr, jal_off >> 12));
                uint32 symaddr = imm_permute(ret_addr, jal_off >> 12);

                // 0x81000000同时指向.text和f8，从2开始避免冲突
                for (int i = 2; i < 128; i++) {
                    if (symaddr == symvals[i]) {
                        sprint("%s\n", strtab_buf + symnames[i]);
                        if (strcmp(strtab_buf + symnames[i], "main") == 0) {
                            return 0;
                        }
                        i = 128;
                    }
                }
            }
        }
        cnt++;
        fp = prev_fp;
    }

    spike_file_close(f);

    return 0;
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0,
                long a1,
                long a2,
                long a3,
                long a4,
                long a5,
                long a6,
                long a7) {
    switch (a0) {
        case SYS_user_print:
            return sys_user_print((const char*)a1, a2);
        case SYS_user_exit:
            return sys_user_exit(a1);
        case SYS_user_backtrace:
            return sys_user_backtrace(a1);
        default:
            panic("Unknown syscall %ld \n", a0);
    }
}
