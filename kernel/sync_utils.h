#ifndef _SYNC_UTILS_H_
#define _SYNC_UTILS_H_

static inline void sync_barrier(volatile int *counter, int all) {

  int local;

  asm volatile("amoadd.w %0, %2, (%1)\n"
               : "=r"(local)
               : "r"(counter), "r"(1)
               : "memory");

  if (local + 1 < all) {
    do {
      asm volatile("lw %0, (%1)\n" : "=r"(local) : "r"(counter) : "memory");
    } while (local < all);
  }
}


typedef struct { int lock; } spinlock_t;

static inline void acquire_lock(spinlock_t *l) {
    int tmp;
    asm volatile (
        "1: lr.w %0, (%1)\n"          // 1. 监听并加载锁的值
        "   bnez %0, 1b\n"            // 2. 如果不为0（已被占用），跳回1继续等
        "   sc.w %0, %2, (%1)\n"      // 3. 尝试将1写入锁中
        "   bnez %0, 1b\n"            // 4. 如果写入失败（被别人抢先了），重试
        "   fence rw, rw\n"           // 5. 成功拿锁，加内存屏障
        : "=&r"(tmp)
        : "r"(&(l->lock)), "r"(1)
        : "memory"
    );
}

static inline void release_lock(spinlock_t *l) {
    asm volatile (
        "fence rw, rw\n"
        "sw zero, (%0)"               // 将锁置0
        : : "r"(&(l->lock)) 
        : "memory"
    );
}
#endif