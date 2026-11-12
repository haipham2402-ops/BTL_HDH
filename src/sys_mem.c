/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "os-mm.h"
#include "syscall.h"
#include "libmem.h"
#include "queue.h"
#include "sched.h"
#include <stdlib.h>
#include <stdio.h>

#ifdef MM64
#include "mm64.h"
#else
#include "mm.h"
#endif

//typedef char BYTE;

int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs* regs)
{
   int memop = regs->a1;
   int ret = 0;
   BYTE value;

   /*
    * @bksysnet: Please note in the dual spacing design
    *            syscall implementations are in kernel space.
    *
    * User space only passes its PID; the kernel looks the process up
    * in its own lists (user processes never hand a PCB to the kernel).
    */
   struct pcb_t *caller = find_proc_by_pid(pid);

   if (caller == NULL) {
      printf("[ERROR] sys_memmap: no live process with PID %u\n", pid);
      return -1;
   }

   switch (memop) {
   case SYSMEM_MAP_OP:
            /* Reserved process case*/
            ret = vmap_pgd_memset(caller, regs->a2, regs->a3);
            break;
   case SYSMEM_INC_OP:
            ret = inc_vma_limit(caller, regs->a2, regs->a3);
            break;
   case SYSMEM_SWP_OP:
            /* MEMRAM frame a2 --> active MEMSWP frame a3 */
            ret = __mm_swap_page(caller, regs->a2, regs->a3);
            break;
   case SYSMEM_SWPIN_OP:
            /* MEMSWP[a4] frame a2 --> MEMRAM frame a3 */
            if (regs->a4 == krnl->active_mswp_id)
               ret = __swap_cp_page(krnl->active_mswp, regs->a2, krnl->mram, regs->a3);
            else if (regs->a4 < PAGING_MAX_MMSWP)
               ret = __swap_cp_page(&((struct memphy_struct *) krnl->mswp)[regs->a4],
                                    regs->a2, krnl->mram, regs->a3);
            else
               ret = -1;
            break;
   case SYSMEM_IO_READ:
            ret = MEMPHY_read(krnl->mram, regs->a2, &value);
            regs->a3 = (unsigned char) value;
            break;
   case SYSMEM_IO_WRITE:
            ret = MEMPHY_write(krnl->mram, regs->a2, regs->a3);
            break;
   default:
            printf("Memop code: %d\n", memop);
            ret = -1;
            break;
   }
   
   return ret;
}