/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#ifndef OSMM_H
#define OSMM_H

#include <stdint.h>

#define MM_PAGING
#define PAGING_MAX_MMSWP 4 /* max number of supported swapped space */
#define PAGING_MAX_SYMTBL_SZ 30

/* 
 * @bksysnet: in long address mode of 64bit or original 32bit
 * the address type need to be redefined
 */

#ifdef MM64
#define ADDR_TYPE uint64_t
#else
#define ADDR_TYPE uint32_t
#endif

typedef char BYTE;
typedef ADDR_TYPE addr_t;
//typedef unsigned int uint32_t;

/* 
 * @bksysnet: the format string need to be redefined
 *            based on the address mode
 */
#ifdef MM64
#define FORMAT_ADDR "%lu"
#define FORMATX_ADDR "%16p"
#else
#define FORMAT_ADDR "%d"
#define FORMATX_ADDR "%08x"
#endif

struct pgn_t{
   addr_t pgn;
   struct pgn_t *pg_next; 
};

/*
 *  Memory region struct
 */
/* Privilege mode bit of a region (as in the assignment description) */
#define RG_MODE_KERNEL 0
#define RG_MODE_USER   1

struct vm_rg_struct {
   int vmaid;

   addr_t rg_start;
   addr_t rg_end;

   /* Priviledge mode bit: usermode - 1, kernelmode - 0 */
   int mode_bit;

   struct vm_rg_struct *rg_next;
};

/*
 *  Memory area struct
 */
struct vm_area_struct {
   unsigned long vm_id;
   addr_t vm_start;
   addr_t vm_end;

   addr_t sbrk;
/*
 * Derived field
 * unsigned long vm_limit = vm_end - vm_start
 */
   struct mm_struct *vm_mm;
   struct vm_rg_struct *vm_freerg_list;
   struct vm_area_struct *vm_next;
};


/* 
 * Kernel cache pool struct
 */
struct kcache_pool_struct {
   int size;
   int align;

#ifdef MM64
   addr_t storage;
#else
   uint32_t storage;
#endif
};


/*
 * Memory statistics of one process (report data)
 */
struct mm_stat_struct {
   unsigned long alloc_ok, alloc_fail, free_ok;
   unsigned long mem_read, mem_write;   /* byte accesses through the MMU */
   unsigned long page_hit, page_fault;  /* page present / swapped in      */
   unsigned long swap_in, swap_out;     /* pages moved MEMSWP->RAM, RAM->MEMSWP */
   unsigned long evicted;               /* own pages chosen as victim     */
   unsigned long invalid_access;
   unsigned long kmalloc_ok, kfree_ok;  /* kernel buffers of the process  */
   unsigned long kcopy_bytes;           /* copy_from_user + copy_to_user  */
   unsigned long priv_violation;        /* user access to a kernel region */
};

/* 
 * Memory management struct
 */
struct mm_struct {
#ifdef MM64
   addr_t *pgd;
   addr_t *p4d;
   addr_t *pud;
   addr_t *pmd;
   addr_t *pt;
#else
   uint32_t *pgd;
#endif

   struct vm_area_struct *mmap;

   /* Currently we support a fixed number of symbol */
   struct vm_rg_struct symrgtbl[PAGING_MAX_SYMTBL_SZ];

   /* list of free page */
   struct pgn_t *fifo_pgn;

   /* kmem cache pool */
   struct kcache_pool_struct *kcpooltbl;

   /* statistics for the report */
   struct mm_stat_struct stat;
};


/*
 * FRAME/MEM PHY struct
 */
struct framephy_struct { 
   addr_t fpn;
   struct framephy_struct *fp_next;

   /* Resereed for tracking allocated framed */
   struct mm_struct* owner;
};

struct memphy_struct {
   /* Basic field of data and size */
   BYTE *storage;
   addr_t maxsz;   /* addr_t: a 4GB device does not fit in int */
   
   /* Sequential device fields */ 
   int rdmflg;
   int cursor;

   /* Management structure */
   struct framephy_struct *free_fp_list;
   struct framephy_struct *used_fp_list;
};

#endif
