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
#include "dma_queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

static int get_free_rg_in_vma(struct vm_area_struct *cur_vma, addr_t size,
                              struct vm_rg_struct *newrg);

/*
 * Resident page queue for page replacement (global, all processes).
 * Head = oldest (FIFO) or least recently used (LRU), tail = newest.
 * Protected by mmvm_lock: every user memory operation holds it.
 */
struct resident_node {
  struct mm_struct *mm;
  addr_t pgn;
  unsigned int pins;
  struct resident_node *next;
};
static struct resident_node *res_head = NULL, *res_tail = NULL;

struct dma_pin_ref {
  struct mm_struct *mm;
  addr_t pgn;
  struct dma_pin_ref *next;
};

struct dma_job_t {
  struct pcb_t *caller;
  uint32_t src_rg, dst_rg, offset, size;
  size_t remaining;
  int status;
  struct dma_pin_ref *pins;
};

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
  node->pins = 0;
  node->next = NULL;
  if (res_tail == NULL)
    res_head = node;
  else
    res_tail->next = node;
  res_tail = node;

  enlist_pgn_node(&mm->fifo_pgn, pgn);
}

static int resident_pin(struct mm_struct *mm, addr_t pgn)
{
  struct resident_node *it;

  for (it = res_head; it != NULL; it = it->next)
    if (it->mm == mm && it->pgn == pgn) {
      it->pins++;
      return 0;
    }
  return -1;
}

static void resident_unpin(struct mm_struct *mm, addr_t pgn)
{
  struct resident_node *it;

  for (it = res_head; it != NULL; it = it->next)
    if (it->mm == mm && it->pgn == pgn) {
      if (it->pins > 0)
        it->pins--;
      return;
    }
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
  struct resident_node *victim = res_head, *victim_prev = NULL;
  struct krnl_t *krnl = caller->krnl;
  addr_t vicpte, vicfpn, swpfpn, swppte = 0;

  while (victim != NULL && victim->pins != 0) {
    victim_prev = victim;
    victim = victim->next;
  }
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
  resident_unlink(victim_prev, victim);
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

  if (cur_vma == NULL || sym == NULL || size == 0
      /* do not lose a kernel buffer that still owns this region id */
      || (sym->mode_bit == RG_MODE_KERNEL && sym->rg_start < sym->rg_end)) {
    caller->mm->stat.alloc_fail++;
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* 1. Reuse a free region of the area if one is big enough */
  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    sym->rg_start = rgnode.rg_start;
    sym->rg_end = rgnode.rg_end;
    sym->mode_bit = RG_MODE_USER;
 
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
  sym->mode_bit = RG_MODE_USER;
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

  /* A kernel buffer of this process: the kernel releases it */
  if (rgnode->mode_bit == RG_MODE_KERNEL)
  {
    int ret = __kfree(caller, rgid);
    pthread_mutex_unlock(&mmvm_lock);
    return ret;
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

/* Region rgid of the caller in mode @mode, if [offset] lies inside it */
static struct vm_rg_struct *
region_in_mode(struct pcb_t *caller, int rgid, addr_t offset, int mode)
{
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);

  if (currg == NULL)
    return NULL;
  if (currg->rg_start >= currg->rg_end)          /* not allocated  */
    return NULL;
  if (currg->mode_bit != mode)                   /* wrong space    */
    return NULL;
  if (offset >= currg->rg_end - currg->rg_start)  /* out of bounds */
    return NULL;
  return currg;
}

/* User access check: a user program may only touch user regions whose
 * addresses lie below the kernel base (privileged memory restriction) */
static struct vm_rg_struct *
valid_region(struct pcb_t *caller, int vmaid, int rgid, addr_t offset)
{
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);

  if (get_vma_by_num(caller->mm, vmaid) == NULL)
    return NULL;
  if (currg != NULL && currg->rg_start < currg->rg_end
      && (currg->mode_bit == RG_MODE_KERNEL
          || currg->rg_start >= PAGING64_KRNL_BASE)) {
    caller->mm->stat.priv_violation++;
    printf("[ERROR] PID %d: permission denied, region %d is kernel memory "
           "(use copy_from_user/copy_to_user)\n", caller->pid, rgid);
    return NULL;
  }
  return region_in_mode(caller, rgid, offset, RG_MODE_USER);
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


/*
 * Kernel memory (dual space design)
 *  - one kernel mm (krnl->mm) shared by all processes, area starting at
 *    PAGING64_KRNL_BASE, contiguous allocation with a first-fit free list
 *  - kernel pages are mapped in the kernel page table (krnl->mm->pgd ==
 *    krnl->krnl_pgd) and are never swapped out
 *  - a kernel buffer is named by a region id of the process symbol table
 *    with mode_bit = RG_MODE_KERNEL; user read/write on it is refused
 * Locks: mmvm_lock (user mm + frames) -> kmm_lock (kernel mm)
 */

/* Map @npages new kernel pages at @start (kernel page table) */
static int kmap_pages(struct pcb_t *caller, addr_t start, int npages)
{
  struct krnl_t *krnl = caller->krnl;
  struct framephy_struct *frm_lst = NULL, *fp;
  addr_t pgn = PAGING64_ADDR_PGN(start);
  int ret = 0;

  /* Frames from MEMRAM; when RAM is full a user victim page is swapped
   * out (kernel pages themselves are not in the replacement queue) */
  if (alloc_pages_range(caller, npages, &frm_lst) != 0)
    return -1;

  for (fp = frm_lst; fp != NULL; fp = fp->fp_next, pgn++)
    if (pte_set_fpn(krnl->mm, pgn, fp->fpn, caller) != 0)
      ret = -1;

  while (frm_lst != NULL) {
    fp = frm_lst->fp_next;
    free(frm_lst);
    frm_lst = fp;
  }
  return ret;
}

/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 * Caller holds mmvm_lock and kmm_lock.
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  struct krnl_t *krnl = caller->krnl;
  struct vm_area_struct *kvma = get_vma_by_num(krnl->mm, vmaid);
  struct vm_rg_struct *sym = get_symrg_byid(caller->mm, rgid);
  struct vm_rg_struct rgnode;

  if (kvma == NULL || sym == NULL || size == 0)
    return -1;
  if (sym->rg_start < sym->rg_end)
    return -1;  /* the region id is in use: refuse instead of leaking it */

  /* 1. Reuse a free kernel range */
  if (get_free_rg_in_vma(kvma, size, &rgnode) != 0) {
    /* 2. Grow the kernel area (contiguous strategy) and map the pages */
    addr_t inc = PAGING64_PAGE_ALIGNSZ(size);
    addr_t start = kvma->sbrk;

    if (kmap_pages(caller, start, inc / PAGING64_PAGESZ) != 0)
      return -1;
    kvma->vm_end += inc;
    kvma->sbrk += inc;
    rgnode.rg_start = start;
    rgnode.rg_end = start + size;
    if (start + size < kvma->sbrk)
      enlist_vm_freerg_list(krnl->mm, init_vm_rg(start + size, kvma->sbrk));
  }

  /* The kernel buffer is named in the process symbol table */
  sym->rg_start = rgnode.rg_start;
  sym->rg_end = rgnode.rg_end;
  sym->mode_bit = RG_MODE_KERNEL;
  sym->rg_next = NULL;

  *alloc_addr = rgnode.rg_start;
  return 0;
}

/* Release kernel buffer @rgid of the caller. Caller holds mmvm_lock. */
int __kfree(struct pcb_t *caller, int rgid)
{
  struct vm_rg_struct *sym = get_symrg_byid(caller->mm, rgid);

  if (sym == NULL || sym->rg_start >= sym->rg_end || sym->mode_bit != RG_MODE_KERNEL)
    return -1;

  pthread_mutex_lock(&kmm_lock);
  enlist_vm_freerg_list(caller->krnl->mm, init_vm_rg(sym->rg_start, sym->rg_end));
  pthread_mutex_unlock(&kmm_lock);

  sym->rg_start = sym->rg_end = 0;
  sym->mode_bit = RG_MODE_USER;
  caller->mm->stat.kfree_ok++;
  return 0;
}

/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

int libkmem_malloc(struct pcb_t * caller, uint32_t size, uint32_t reg_index)
{
  addr_t addr = 0;
  int val;

  /* OS level management: validate, then forward to the kernel helper */
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  pthread_mutex_lock(&kmm_lock);
  val = (int) __kmalloc(caller, 0, reg_index, size, &addr);
  pthread_mutex_unlock(&kmm_lock);
  if (val == 0)
    caller->mm->stat.kmalloc_ok++;
  pthread_mutex_unlock(&mmvm_lock);

  if (val != 0) {
    printf("[ERROR] PID %d: kmalloc %u bytes to region %u failed\n",
           caller->pid, size, reg_index);
    return -1;
  }
#ifdef IODUMP
  printf("kmalloc region=%u size=%u kaddr=0x%016lx\n", reg_index, size,
         (unsigned long) addr);
#endif
  return 0;
}


/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 */
/* Physical address of kernel virtual address @kaddr (never swapped) */
static int kmem_phys(struct krnl_t *krnl, addr_t kaddr, addr_t *phys)
{
  addr_t pte = pte_get_entry(krnl->mm, PAGING64_ADDR_PGN(kaddr), NULL);

  if (!PAGING_PAGE_PRESENT(pte))
    return -1;
  *phys = PAGING_FPN(pte) * PAGING64_PAGESZ + PAGING64_ADDR_OFFST(kaddr);
  return 0;
}

static int dma_pin_user_page(struct dma_job_t *job, addr_t pgn)
{
  struct dma_pin_ref *pin;

  for (pin = job->pins; pin != NULL; pin = pin->next)
    if (pin->mm == job->caller->mm && pin->pgn == pgn)
      return 0;

  pin = malloc(sizeof(*pin));
  if (pin == NULL)
    return -1;
  if (resident_pin(job->caller->mm, pgn) != 0) {
    free(pin);
    return -1;
  }
  pin->mm = job->caller->mm;
  pin->pgn = pgn;
  pin->next = job->pins;
  job->pins = pin;
  return 0;
}

static int dma_translate(struct dma_job_t *job, struct vm_rg_struct *rg,
                         addr_t region_offset, addr_t *phys)
{
  addr_t addr = rg->rg_start + region_offset;
  addr_t pgn = PAGING64_ADDR_PGN(addr);
  int fpn;

  if (rg->mode_bit == RG_MODE_KERNEL)
    return kmem_phys(job->caller->krnl, addr, phys);

  if (pg_getpage(job->caller->mm, pgn, &fpn, job->caller) != 0
      || dma_pin_user_page(job, pgn) != 0)
    return -1;
  *phys = (addr_t) fpn * PAGING64_PAGESZ + PAGING64_ADDR_OFFST(addr);
  return 0;
}

static void dma_release_pins(struct dma_job_t *job)
{
  struct dma_pin_ref *pin, *next;

  pthread_mutex_lock(&mmvm_lock);
  for (pin = job->pins; pin != NULL; pin = next) {
    next = pin->next;
    resident_unpin(pin->mm, pin->pgn);
    free(pin);
  }
  pthread_mutex_unlock(&mmvm_lock);
}

int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst,
                      uint32_t offset, uint32_t size)
{
  struct dma_job_t *job = NULL;
  struct dma_req_t *req_head = NULL, *req_tail = NULL, *req;
  struct vm_rg_struct *src_rg, *dst_rg;
  uint64_t remaining, current;
  const char *failure = "invalid";

  if (caller == NULL || caller->krnl == NULL || caller->mm == NULL
      || caller->krnl->mram == NULL || size == 0
      || src >= PAGING_MAX_SYMTBL_SZ || dst >= PAGING_MAX_SYMTBL_SZ) {
    if (caller != NULL)
      printf("[ERROR] PID %d: dma_memcpy src_rg=%u -> dst_rg=%u offset=%u size=%u invalid\n",
             caller->pid, src, dst, offset, size);
    return -1;
  }

  job = calloc(1, sizeof(*job));
  if (job == NULL) {
    failure = "DMA request allocation failed";
    goto cleanup_invalid;
  }
  job->caller = caller;
  job->src_rg = src;
  job->dst_rg = dst;
  job->offset = offset;
  job->size = size;

  pthread_mutex_lock(&mmvm_lock);
  pthread_mutex_lock(&kmm_lock);
  src_rg = get_symrg_byid(caller->mm, src);
  dst_rg = get_symrg_byid(caller->mm, dst);
  if (src_rg == NULL || dst_rg == NULL
      || src_rg->rg_start >= src_rg->rg_end
      || dst_rg->rg_start >= dst_rg->rg_end
      || (src_rg->mode_bit != RG_MODE_USER && src_rg->mode_bit != RG_MODE_KERNEL)
      || (dst_rg->mode_bit != RG_MODE_USER && dst_rg->mode_bit != RG_MODE_KERNEL)
      || (uint64_t) offset > src_rg->rg_end - src_rg->rg_start
      || (uint64_t) offset > dst_rg->rg_end - dst_rg->rg_start
      || (uint64_t) size > src_rg->rg_end - src_rg->rg_start - offset
      || (uint64_t) size > dst_rg->rg_end - dst_rg->rg_start - offset)
    goto unlock_invalid;

  remaining = size;
  current = offset;
  while (remaining > 0) {
    addr_t src_phys, dst_phys;
    uint64_t src_addr = src_rg->rg_start + current;
    uint64_t dst_addr = dst_rg->rg_start + current;
    uint64_t chunk = remaining;
    uint64_t src_page_left = PAGING64_PAGESZ - PAGING64_ADDR_OFFST(src_addr);
    uint64_t dst_page_left = PAGING64_PAGESZ - PAGING64_ADDR_OFFST(dst_addr);

    if (chunk > src_page_left)
      chunk = src_page_left;
    if (chunk > dst_page_left)
      chunk = dst_page_left;
    if (dma_translate(job, src_rg, current, &src_phys) != 0
        || dma_translate(job, dst_rg, current, &dst_phys) != 0) {
      failure = "DMA address translation failed";
      goto unlock_invalid;
    }

    req = calloc(1, sizeof(*req));
    if (req == NULL) {
      failure = "DMA request allocation failed";
      goto unlock_invalid;
    }
    if (pthread_cond_init(&req->done_cv, NULL) != 0) {
      free(req);
      failure = "DMA request initialization failed";
      goto unlock_invalid;
    }
    req->caller = caller;
    req->job = job;
    req->src = src_phys;
    req->dst = dst_phys;
    req->len = chunk;
    if (req_tail == NULL)
      req_head = req;
    else
      req_tail->next = req;
    req_tail = req;
    job->remaining++;

    current += chunk;
    remaining -= chunk;
  }

  pthread_mutex_unlock(&kmm_lock);
  pthread_mutex_unlock(&mmvm_lock);

  if (scheduler_dma_begin(caller) != 0) {
    failure = "DMA scheduler rejected request";
    goto submit_invalid;
  }
  for (req = req_head; req != NULL; req = req->next)
    printf("DMA SUBMIT pid=%d src=0x%lx dst=0x%lx len=%lu\n",
           caller->pid, (unsigned long) req->src, (unsigned long) req->dst,
           (unsigned long) req->len);
  if (dma_submit_batch(req_head, job->remaining) != 0) {
    scheduler_dma_cancel(caller);
    failure = "DMA queue rejected request";
    goto submit_invalid;
  }
  return DMA_MEMCPY_PENDING;

unlock_invalid:
  pthread_mutex_unlock(&kmm_lock);
  pthread_mutex_unlock(&mmvm_lock);
  goto cleanup_invalid;

submit_invalid:
  scheduler_dma_cancel(caller);

cleanup_invalid:
  req = req_head;
  while (req != NULL) {
    struct dma_req_t *next = req->next;
    pthread_cond_destroy(&req->done_cv);
    free(req);
    req = next;
  }
  if (job != NULL) {
    if (job->pins != NULL)
      dma_release_pins(job);
    free(job);
  }
  if (strcmp(failure, "invalid") == 0)
    printf("[ERROR] PID %d: dma_memcpy src_rg=%u -> dst_rg=%u offset=%u size=%u invalid\n",
           caller->pid, src, dst, offset, size);
  else
    printf("[ERROR] PID %d: %s for dma_memcpy src_rg=%u -> dst_rg=%u offset=%u size=%u\n",
           caller->pid, failure, src, dst, offset, size);
  return -1;
}

void libmem_dma_complete(struct dma_req_t *req, int status)
{
  struct dma_job_t *job;

  if (req == NULL)
    return;
  job = req->job;
  printf("DMA IRQ pid=%d len=%lu status=%d\n", req->caller->pid,
         (unsigned long) req->len, status);
  if (status != 0)
    job->status = status;

  if (job->remaining > 0)
    job->remaining--;
  if (job->remaining == 0) {
    dma_release_pins(job);
    scheduler_dma_wake(job->caller);
    if (job->status == 0)
      printf("dma_memcpy src_rg=%u -> dst_rg=%u offset=%u size=%u done\n",
             job->src_rg, job->dst_rg, job->offset, job->size);
    free(job);
  }

  dma_complete(req, status);
  pthread_cond_destroy(&req->done_cv);
  free(req);
}

/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 * Kernel side; caller holds kmm_lock.
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  struct vm_rg_struct *rg = region_in_mode(caller, rgid, offset, RG_MODE_KERNEL);
  addr_t phys;

  (void) vmaid;
  if (rg == NULL || kmem_phys(caller->krnl, rg->rg_start + offset, &phys) != 0)
    return -1;
  return MEMPHY_read(caller->krnl->mram, phys, data);
}

/*__write_kernel_mem - write a kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 * Kernel side; caller holds kmm_lock.
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  struct vm_rg_struct *rg = region_in_mode(caller, rgid, offset, RG_MODE_KERNEL);
  addr_t phys;

  (void) vmaid;
  if (rg == NULL || kmem_phys(caller->krnl, rg->rg_start + offset, &phys) != 0)
    return -1;
  return MEMPHY_write(caller->krnl->mram, phys, value);
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 * Kernel side access to user memory through the process page table
 * (may page fault); caller holds mmvm_lock.
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  struct vm_rg_struct *rg = region_in_mode(caller, rgid, offset, RG_MODE_USER);
  addr_t addr;
  int fpn;

  (void) vmaid;
  if (rg == NULL)
    return -1;
  addr = rg->rg_start + offset;
  if (pg_getpage(caller->mm, PAGING64_ADDR_PGN(addr), &fpn, caller) != 0)
    return -1;
  caller->mm->stat.mem_read++;
  return MEMPHY_read(caller->krnl->mram,
                     (addr_t) fpn * PAGING64_PAGESZ + PAGING64_ADDR_OFFST(addr), data);
}


/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 * Kernel side; caller holds mmvm_lock.
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  struct vm_rg_struct *rg = region_in_mode(caller, rgid, offset, RG_MODE_USER);
  addr_t addr;
  int fpn;

  (void) vmaid;
  if (rg == NULL)
    return -1;
  addr = rg->rg_start + offset;
  if (pg_getpage(caller->mm, PAGING64_ADDR_PGN(addr), &fpn, caller) != 0)
    return -1;
  caller->mm->stat.mem_write++;
  return MEMPHY_write(caller->krnl->mram,
                      (addr_t) fpn * PAGING64_PAGESZ + PAGING64_ADDR_OFFST(addr), value);
}

/* Both ranges [offset, offset+size) exist in the expected spaces */
static int kcopy_check(struct pcb_t *caller, int user_rg, int kernel_rg,
                       uint32_t offset, uint32_t size)
{
  if (size == 0)
    return -1;
  return (region_in_mode(caller, user_rg, offset + size - 1, RG_MODE_USER) != NULL
          && region_in_mode(caller, kernel_rg, offset + size - 1, RG_MODE_KERNEL) != NULL)
         ? 0 : -1;
}

/*libkmem_copy_from_user - copy @size bytes user region @source -> kernel
 * region @destination, both at @offset
 */
int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  uint32_t i;
  int ret = 0;
  BYTE b;

  pthread_mutex_lock(&mmvm_lock);
  pthread_mutex_lock(&kmm_lock);
  /* Validate the whole transfer first: all or nothing */
  if (kcopy_check(caller, source, destination, offset, size) != 0)
    ret = -1;
  for (i = 0; ret == 0 && i < size; i++)
    if (__read_user_mem(caller, 0, source, offset + i, &b) != 0
        || __write_kernel_mem(caller, 0, destination, offset + i, b) != 0)
      ret = -1;
  if (ret == 0)
    caller->mm->stat.kcopy_bytes += size;
  else
    caller->mm->stat.invalid_access++;
  pthread_mutex_unlock(&kmm_lock);
  pthread_mutex_unlock(&mmvm_lock);

  if (ret != 0)
    printf("[ERROR] PID %d: copy_from_user user_rg=%u -> kernel_rg=%u offset=%u size=%u invalid\n",
           caller->pid, source, destination, offset, size);
#ifdef IODUMP
  else
    printf("copy_from_user user_rg=%u -> kernel_rg=%u offset=%u size=%u\n",
           source, destination, offset, size);
#endif
  return ret;
}

/*libkmem_copy_to_user - copy @size bytes kernel region @source -> user
 * region @destination, both at @offset
 */
int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  uint32_t i;
  int ret = 0;
  BYTE b;

  pthread_mutex_lock(&mmvm_lock);
  pthread_mutex_lock(&kmm_lock);
  if (kcopy_check(caller, destination, source, offset, size) != 0)
    ret = -1;
  for (i = 0; ret == 0 && i < size; i++)
    if (__read_kernel_mem(caller, 0, source, offset + i, &b) != 0
        || __write_user_mem(caller, 0, destination, offset + i, b) != 0)
      ret = -1;
  if (ret == 0)
    caller->mm->stat.kcopy_bytes += size;
  else
    caller->mm->stat.invalid_access++;
  pthread_mutex_unlock(&kmm_lock);
  pthread_mutex_unlock(&mmvm_lock);

  if (ret != 0)
    printf("[ERROR] PID %d: copy_to_user kernel_rg=%u -> user_rg=%u offset=%u size=%u invalid\n",
           caller->pid, source, destination, offset, size);
#ifdef IODUMP
  else
    printf("copy_to_user kernel_rg=%u -> user_rg=%u offset=%u size=%u\n",
           source, destination, offset, size);
#endif
  return ret;
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
    printf("\tMMSTAT PID %d: kmalloc=%lu kfree=%lu kcopy_bytes=%lu priv_violation=%lu\n",
           proc->pid, st->kmalloc_ok, st->kfree_ok, st->kcopy_bytes, st->priv_violation);
  }
#endif

  /* Kernel buffers still held by the process go back to the kernel */
  {
    int rgid;
    pthread_mutex_lock(&mmvm_lock);
    for (rgid = 0; rgid < PAGING_MAX_SYMTBL_SZ; rgid++)
      if (proc->mm->symrgtbl[rgid].mode_bit == RG_MODE_KERNEL
          && proc->mm->symrgtbl[rgid].rg_start < proc->mm->symrgtbl[rgid].rg_end)
        __kfree(proc, rgid);
    pthread_mutex_unlock(&mmvm_lock);
  }

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

/* First-fit search of @size bytes in the free list of @cur_vma */
static int get_free_rg_in_vma(struct vm_area_struct *cur_vma, addr_t size, struct vm_rg_struct *newrg)
{
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

  if (newrg->rg_start == (addr_t) -1) // new region not found
    return -1;

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
  return get_free_rg_in_vma(get_vma_by_num(caller->mm, vmaid), size, newrg);
}

// #endif
