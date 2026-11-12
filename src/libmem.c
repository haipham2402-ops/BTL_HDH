/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System Library
 * Memory Module Library libmem.c
 */

// #ifdef MM_PAGING

#include "string.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Resident page queue for page replacement (global, all processes).
 * Head = oldest (FIFO) or least recently used (LRU), tail = newest.
 * Protected by mmvm_lock: every user memory operation holds it.
 */
struct resident_node {
  struct mm_struct *mm;
  addr_t pgn;
  struct resident_node *next;
};
static struct resident_node *res_head = NULL, *res_tail = NULL;

/* Remove pgn from a per-process page list (mm->fifo_pgn) */
static void pgn_list_remove(struct pgn_t **plist, addr_t pgn)
{
  struct pgn_t *prev = NULL, *it = *plist;

  while (it != NULL && it->pgn != pgn) {
    prev = it;
    it = it->pg_next;
  }
  if (it == NULL)
    return;
  if (prev == NULL)
    *plist = it->pg_next;
  else
    prev->pg_next = it->pg_next;
  free(it);
}

/*mm_page_resident - a page of @mm just got a RAM frame
 * Append it to the replacement queue and to the per-process list.
 */
void mm_page_resident(struct mm_struct *mm, addr_t pgn)
{
  struct resident_node *node = malloc(sizeof(struct resident_node));

  node->mm = mm;
  node->pgn = pgn;
  node->next = NULL;
  if (res_tail == NULL)
    res_head = node;
  else
    res_tail->next = node;
  res_tail = node;

  enlist_pgn_node(&mm->fifo_pgn, pgn);
}

/* Unlink a node from the queue, prev is its predecessor (or NULL) */
static void resident_unlink(struct resident_node *prev, struct resident_node *node)
{
  if (prev == NULL)
    res_head = node->next;
  else
    prev->next = node->next;
  if (res_tail == node)
    res_tail = prev;
  node->next = NULL;
}

#ifdef MM_REPLACE_LRU
/* LRU: an accessed page becomes the most recently used (tail) */
static void resident_touch(struct mm_struct *mm, addr_t pgn)
{
  struct resident_node *prev = NULL, *it = res_head;

  while (it != NULL && !(it->mm == mm && it->pgn == pgn)) {
    prev = it;
    it = it->next;
  }
  if (it == NULL || it == res_tail)
    return;
  resident_unlink(prev, it);
  res_tail->next = it;
  res_tail = it;
}
#endif

/* Drop every queued page of @mm (process exit) */
static void resident_remove_mm(struct mm_struct *mm)
{
  struct resident_node *prev = NULL, *it = res_head, *next;

  while (it != NULL) {
    next = it->next;
    if (it->mm == mm) {
      resident_unlink(prev, it);
      free(it);
    } else {
      prev = it;
    }
    it = next;
  }
}

/* Swap device number @id; krnl->mswp points to the array of devices
 * built in os.c (struct memphy_struct mswp[PAGING_MAX_MMSWP]) */
static struct memphy_struct *swap_dev(struct krnl_t *krnl, int id)
{
  if (id == (int) krnl->active_mswp_id)
    return krnl->active_mswp;
  if (id < 0 || id >= PAGING_MAX_MMSWP)
    return NULL;
  return &((struct memphy_struct *) krnl->mswp)[id];
}

/*
 * evict_one - swap the victim page out and hand its RAM frame over
 * @via_syscall: user library path (copy through SYSCALL 17 memmap) or
 *               kernel path (direct call)
 */
static int evict_one(struct pcb_t *caller, addr_t *retfpn, int via_syscall)
{
  struct resident_node *victim = res_head;
  struct krnl_t *krnl = caller->krnl;
  addr_t vicpte, vicfpn, swpfpn, swppte = 0;

  if (victim == NULL)
    return -1;  /* nothing resident to replace */

  vicpte = pte_get_entry(victim->mm, victim->pgn, caller);
  if (!PAGING_PAGE_PRESENT(vicpte))
    return -1;
  vicfpn = PAGING_FPN(vicpte);

  /* Get free frame in MEMSWP */
  if (MEMPHY_get_freefp(krnl->active_mswp, &swpfpn) != 0)
    return -1;  /* swap is full */

  /* Copy victim frame to swap: SWP(vicfpn --> swpfpn) */
  if (via_syscall) {
    struct sc_regs regs;
    regs.a1 = SYSMEM_SWP_OP;
    regs.a2 = vicfpn;
    regs.a3 = swpfpn;
    _syscall(krnl, caller->pid, 17, &regs); /* SYSCALL 17 sys_memmap */
  } else {
    __mm_swap_page(caller, vicfpn, swpfpn);
  }

  /* Update page table of the victim's owner: swapped, not present */
  SETBIT(swppte, PAGING_PTE_SWAPPED_MASK);
  SETVAL(swppte, (addr_t) krnl->active_mswp_id, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
  SETVAL(swppte, swpfpn, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);
  pte_set_entry(victim->mm, victim->pgn, swppte, caller);

  victim->mm->stat.evicted++;
  caller->mm->stat.swap_out++;
  pgn_list_remove(&victim->mm->fifo_pgn, victim->pgn);
  resident_unlink(NULL, victim);
  free(victim);

  *retfpn = vicfpn;
  return 0;
}

/*vm_evict_page - kernel side: free a RAM frame by swapping out a victim
 * (used by alloc_pages_range when MEMRAM is used up)
 */
int vm_evict_page(struct pcb_t *caller, addr_t *retfpn)
{
  if (caller == NULL || caller->krnl == NULL || retfpn == NULL)
    return -1;
  return evict_one(caller, retfpn, 0);
}

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 * The list is kept sorted by address and adjacent free regions are
 * merged, so freed space can be reused by bigger requests.
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  struct vm_rg_struct **link = &mm->mmap->vm_freerg_list;
  struct vm_rg_struct *it;

  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  /* Drop empty placeholder nodes on the way and find the position */
  while ((it = *link) != NULL) {
    if (it->rg_start >= it->rg_end) {
      *link = it->rg_next;
      free(it);
      continue;
    }
    if (it->rg_start >= rg_elmt->rg_end)
      break;
    link = &it->rg_next;
  }
  rg_elmt->rg_next = *link;
  *link = rg_elmt;

  /* Merge with the next region */
  it = rg_elmt->rg_next;
  if (it != NULL && it->rg_start == rg_elmt->rg_end) {
    rg_elmt->rg_end = it->rg_end;
    rg_elmt->rg_next = it->rg_next;
    free(it);
  }
  /* Merge with the previous region */
  for (it = mm->mmap->vm_freerg_list; it != NULL && it->rg_next != NULL; it = it->rg_next) {
    if (it->rg_next == rg_elmt && it->rg_end == rg_elmt->rg_start) {
      it->rg_end = rg_elmt->rg_end;
      it->rg_next = rg_elmt->rg_next;
      free(rg_elmt);
      break;
    }
  }

  return 0;
}

/*get_symrg_byid - get mem region by region ID
 *@mm: memory region
 *@rgid: region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
  if (mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return NULL;

  return &mm->symrgtbl[rgid];
}

/*__alloc - allocate a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *@alloc_addr: address of allocated memory region
 *
 */
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /*Allocate at the toproof */
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  struct vm_rg_struct *sym = get_symrg_byid(caller->mm, rgid);

  if (cur_vma == NULL || sym == NULL || size == 0) {
    caller->mm->stat.alloc_fail++;
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 1. Reuse a free region of the area if one is big enough */
  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    sym->rg_start = rgnode.rg_start;
    sym->rg_end = rgnode.rg_end;
 
    *alloc_addr = rgnode.rg_start;
    caller->mm->stat.alloc_ok++;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  /* 2. No free region (Fig.7): raise the break with SYSCALL memmap INC,
   *    the kernel aligns the size to pages, takes frames and maps them */
  addr_t old_sbrk = cur_vma->sbrk;
  struct sc_regs regs;
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
  regs.a3 = size;
  if (_syscall(caller->krnl, caller->pid, 17, &regs) != 0) /* SYSCALL 17 sys_memmap */
  {
    caller->mm->stat.alloc_fail++;
    printf("[ERROR] PID %d: alloc %lu bytes failed (out of memory)\n",
           caller->pid, (unsigned long) size);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /*Successful increase limit */
  sym->rg_start = old_sbrk;
  sym->rg_end = old_sbrk + size;
  *alloc_addr = old_sbrk;

  /* The rest of the new pages is free space for later requests */
  if (old_sbrk + size < cur_vma->sbrk)
    enlist_vm_freerg_list(caller->mm, init_vm_rg(old_sbrk + size, cur_vma->sbrk));

  caller->mm->stat.alloc_ok++;
  pthread_mutex_unlock(&mmvm_lock);
  return 0;

}

/*__free - remove a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
  pthread_mutex_lock(&mmvm_lock);

  struct vm_rg_struct *rgnode = get_symrg_byid(caller->mm, rgid);

  if (rgnode == NULL || rgnode->rg_start >= rgnode->rg_end)
  {
    /* free of a register that holds no region */
    caller->mm->stat.invalid_access++;
    printf("[ERROR] PID %d: free region %d not allocated\n", caller->pid, rgid);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* Collect the freed region to freerg_list (pages stay mapped and
   * are reused by later allocations of this process) */
  struct vm_rg_struct *freerg_node = init_vm_rg(rgnode->rg_start, rgnode->rg_end);

  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->rg_next = NULL;

  /*enlist the obsoleted memory region */
  enlist_vm_freerg_list(caller->mm, freerg_node);
  caller->mm->stat.free_ok++;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  addr_t  addr;
  int val = __alloc(proc, 0, reg_index, size, &addr);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  /* By default using vmaid = 0 */
  return val;
}

/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  int val = __free(proc, 0, reg_index);
  if (val == -1)
  {
    return -1;
  }
printf("%s:%d\n",__func__,__LINE__);
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif
  return 0;//val;
}

/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */
int pg_getpage(struct mm_struct *mm, int pgn, int *fpn, struct pcb_t *caller)
{
  addr_t pte = pte_get_entry(mm, pgn, caller);

  if (PAGING_PAGE_PRESENT(pte))
  {
    mm->stat.page_hit++;
#ifdef MM_REPLACE_LRU
    resident_touch(mm, pgn);
#endif
    *fpn = PAGING_FPN(pte);
    return 0;
  }

  if (!(pte & PAGING_PTE_SWAPPED_MASK))
    return -1;  /* never mapped: invalid page access */

  /* Page fault: the page is in MEMSWP, make it actively living */
  struct krnl_t *krnl = caller->krnl;
  int swptyp = PAGING_PTE_SWPTYP(pte);
  addr_t swpoff = PAGING_SWP(pte);
  struct memphy_struct *swp = swap_dev(krnl, swptyp);
  addr_t tgtfpn;
  struct sc_regs regs;

  mm->stat.page_fault++;
  if (swp == NULL)
    return -1;

  /* Target frame: a free RAM frame, otherwise the frame of a victim
   * page that is swapped out first (global FIFO/LRU) */
  if (MEMPHY_get_freefp(krnl->mram, &tgtfpn) != 0
      && evict_one(caller, &tgtfpn, 1) != 0)
    return -1;

  /* Copy the page MEMSWP -> MEMRAM: SYSCALL 17 sys_memmap */
  regs.a1 = SYSMEM_SWPIN_OP;
  regs.a2 = swpoff;
  regs.a3 = tgtfpn;
  regs.a4 = swptyp;
  _syscall(krnl, caller->pid, 17, &regs);
  MEMPHY_put_freefp(swp, swpoff);

  /* Update its online status of the target page */
  pte_set_fpn(mm, pgn, tgtfpn, caller);
  mm_page_resident(mm, pgn);
  mm->stat.swap_in++;

  *fpn = tgtfpn;
  return 0;
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_getval(struct mm_struct *mm, int addr, BYTE *data, struct pcb_t *caller)
{
  addr_t pgn = PAGING64_ADDR_PGN(addr);
  addr_t off = PAGING64_ADDR_OFFST(addr);
  int fpn;
  struct sc_regs regs;

  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  /* MEMPHY READ: SYSCALL 17 sys_memmap with SYSMEM_IO_READ */
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = (addr_t) fpn * PAGING64_PAGESZ + off;
  _syscall(caller->krnl, caller->pid, 17, &regs);
  *data = (BYTE) regs.a3;
  mm->stat.mem_read++;

  return 0;
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, int addr, BYTE value, struct pcb_t *caller)
{
  addr_t pgn = PAGING64_ADDR_PGN(addr);
  addr_t off = PAGING64_ADDR_OFFST(addr);
  int fpn;
  struct sc_regs regs;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  /* MEMPHY WRITE: SYSCALL 17 sys_memmap with SYSMEM_IO_WRITE */
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = (addr_t) fpn * PAGING64_PAGESZ + off;
  regs.a3 = (unsigned char) value;
  _syscall(caller->krnl, caller->pid, 17, &regs);
  mm->stat.mem_write++;

  return 0;
}

/* Region rgid of the caller, if [offset] lies inside it */
static struct vm_rg_struct *
valid_region(struct pcb_t *caller, int vmaid, int rgid, addr_t offset)
{
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);

  if (currg == NULL || get_vma_by_num(caller->mm, vmaid) == NULL)
    return NULL;
  if (currg->rg_start >= currg->rg_end)          /* not allocated  */
    return NULL;
  if (offset >= currg->rg_end - currg->rg_start)  /* out of bounds */
    return NULL;
  return currg;
}

/*__read - read value in region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *currg = valid_region(caller, vmaid, rgid, offset);

  if (currg == NULL || pg_getval(caller->mm, currg->rg_start + offset, data, caller) != 0)
  {
    caller->mm->stat.invalid_access++;
    printf("[ERROR] PID %d: invalid read region %d offset %lu\n",
           caller->pid, rgid, (unsigned long) offset);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc, // Process executing the instruction
    uint32_t source,    // Index of source register
    addr_t offset,    // Source address = [source] + [offset]
    uint32_t* destination)
{
  BYTE data = 0;
printf("%s:%d\n",__func__,__LINE__);
  int val = __read(proc, 0, source, offset, &data);

  *destination = data;
#ifdef IODUMP
  if (val == 0)
    printf("read region=%d offset=%lu value=%d\n", source,
           (unsigned long) offset, (unsigned char) data);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  pthread_mutex_lock(&mmvm_lock);
  /* User data lives in the caller's own mm (the original code used the
   * kernel mm here) */
  struct vm_rg_struct *currg = valid_region(caller, vmaid, rgid, offset);

  if (currg == NULL || pg_setval(caller->mm, currg->rg_start + offset, value, caller) != 0)
  {
    caller->mm->stat.invalid_access++;
    printf("[ERROR] PID %d: invalid write region %d offset %lu\n",
           caller->pid, rgid, (unsigned long) offset);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,   // Process executing the instruction
    BYTE data,            // Data to be wrttien into memory
    uint32_t destination, // Index of destination register
    addr_t offset)
{
  int val = __write(proc, 0, destination, offset, data);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  printf("write region=%d offset=%lu value=%d\n", destination,
         (unsigned long) offset, (unsigned char) data);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}


/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

int libkmem_malloc(struct pcb_t * caller, uint32_t size, uint32_t reg_index)
{
  /* 
   * TODO: provide OS level management
   *       and forward the request to helper
   */
//addr_t  addr;
//int val = ...

  /* 
   * TODO: provide OS kmem allocation validation
   */

  return 0;
}


/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /* TODO: provide OS kernel memory allocation
   *       update krnl_pgd for OS kernel level management */

  //struct krnl_t *krnl = caller->krnl;
  //krnl->symrgtbl...
  //krnl->krnl_pgd ...

  return 0;

}



/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 */
int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst, uint32_t offset, uint32_t size)
{
  /* TODO: provide DMA mempy */

  //struct krnl_t *krnl = caller->krnl;

  /*
   *  TODO: provide asynchronous dma memcpy
   *        handling the dma_done_callback
   */
  //do_dma(caller, (uint64_t) src, (uint64_t) dst, (uint64_t) size);
  //krnl->...
  //krnl->...

  return 0;
}


int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_user_mem(...)
  //__write_kernel_mem(...);

  return 0;
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_kernel_mem(...)
  //__write_user_mem(...);

  return 1;
}


/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy

  return 0;
}

/*__write_kernel_mem - write a kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy

  return 0;
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

   return 0;
}


/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

  return 0;
}


/* Give the frame of one PTE back to its device */
static int release_pte_frame(addr_t pgn, addr_t pte, void *arg)
{
  struct krnl_t *krnl = (struct krnl_t *) arg;

  (void) pgn;
  if (PAGING_PAGE_PRESENT(pte))
    MEMPHY_put_freefp(krnl->mram, PAGING_FPN(pte));
  else if (pte & PAGING_PTE_SWAPPED_MASK) {
    struct memphy_struct *swp = swap_dev(krnl, PAGING_PTE_SWPTYP(pte));
    if (swp != NULL)
      MEMPHY_put_freefp(swp, PAGING_SWP(pte));
  }
  return 0;
}

/*free_pcb_memphy - collect all memphy of pcb
 *@caller: caller
 *
 * Walk the 5-level page table (the original code read pgd as a flat
 * 32-bit table), return every frame, then free the page table.
 */
int free_pcb_memph(struct pcb_t *caller)
{
  if (caller == NULL || caller->mm == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  mm64_for_each_pte(caller->mm, release_pte_frame, caller->krnl);
  resident_remove_mm(caller->mm);
  while (caller->mm->fifo_pgn != NULL)
    pgn_list_remove(&caller->mm->fifo_pgn, caller->mm->fifo_pgn->pgn);
  mm64_free_pgtbl(caller->mm);
  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*mm_report_and_release - called when a process finishes
 * Print its memory statistics (MM_STATS_DUMP) and release its frames.
 */
int mm_report_and_release(struct pcb_t *proc)
{
  if (proc == NULL || proc->mm == NULL)
    return -1;

#ifdef MM_STATS_DUMP
  {
    int ntbl[5];
    struct mm_stat_struct *st = &proc->mm->stat;
    unsigned long acc = st->mem_read + st->mem_write;
    addr_t pgtbl_sz;

    pthread_mutex_lock(&mmvm_lock);
    pgtbl_sz = mm64_pgtbl_stats(proc->mm, ntbl);
    pthread_mutex_unlock(&mmvm_lock);

    printf("\tMMSTAT PID %d: alloc=%lu alloc_fail=%lu free=%lu read=%lu write=%lu "
           "page_hit=%lu page_fault=%lu swap_in=%lu swap_out=%lu evicted=%lu invalid=%lu\n",
           proc->pid, st->alloc_ok, st->alloc_fail, st->free_ok,
           st->mem_read, st->mem_write, st->page_hit, st->page_fault,
           st->swap_in, st->swap_out, st->evicted, st->invalid_access);
    printf("\tMMSTAT PID %d: fault_rate=%.2f%% pgtbl=%lu bytes (PGD=%d P4D=%d PUD=%d PMD=%d PT=%d)\n",
           proc->pid, acc ? 100.0 * st->page_fault / acc : 0.0,
           (unsigned long) pgtbl_sz, ntbl[0], ntbl[1], ntbl[2], ntbl[3], ntbl[4]);
  }
#endif

  if (free_pcb_memph(proc) != 0)
    return -1;

  /* The process is gone: release its areas, free lists and mm */
  struct vm_area_struct *vma = proc->mm->mmap, *nvma;
  while (vma != NULL) {
    struct vm_rg_struct *rg = vma->vm_freerg_list, *nrg;
    while (rg != NULL) {
      nrg = rg->rg_next;
      free(rg);
      rg = nrg;
    }
    nvma = vma->vm_next;
    free(vma);
    vma = nvma;
  }
  free(proc->mm);
  proc->mm = NULL;
  return 0;
}


/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 * Local policy over the pages of @mm: the oldest one (tail of fifo_pgn,
 * new pages are pushed at the head). The simulator uses the global
 * queue (evict_one); this helper is kept for the API.
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  struct pgn_t *pg = mm->fifo_pgn;
  struct pgn_t *prev = NULL;

  if (!pg)
  {
    return -1;
  }
  while (pg->pg_next)
  {
    prev = pg;
    pg = pg->pg_next;
  }
  *retpgn = pg->pgn;
  if (prev == NULL)   /* single page: the original code crashed here */
    mm->fifo_pgn = NULL;
  else
    prev->pg_next = NULL;

  free(pg);

  return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (cur_vma == NULL)
    return -1;

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;

  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start + size <= rgit->rg_end)
    { /* Current region has enough space */
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end)
      {
        rgit->rg_start = rgit->rg_start + size;
      }
      else
      { /*Use up all space, remove current node */
        /*Clone next rg node */
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        if (nextrg != NULL)
        {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;

          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        }
        else
        {                                /*End of free list */
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    }
    else
    {
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == -1) // new region not found
    return -1;

  return 0;
}

// #endif
