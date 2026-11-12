/*
 * Example system call added following section 1.6.3 of the assignment
 * ("Adding a system call to Simple Operating System"), number 440.
 * It runs in kernel space and only receives the PID of the caller.
 */

#include "common.h"
#include "syscall.h"
#include <stdio.h>

int __sys_xxxhandler(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs)
{
   (void) krnl;
   (void) pid;

   printf("The first system call parameter %lu\n", (unsigned long) regs->a1);
   return 0;
}
