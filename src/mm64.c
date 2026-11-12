/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

/*
 * PAGING based Memory Management
 * Memory management unit mm/mm.c
 */

#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#if defined(MM64)

/*
 * Multi-level page table layout (5 levels, 4KB pages, 57-bit address):
 *
 *   bits 56..48  47..39  38..30  29..21  20..12   11..0
 *        PGD     P4D     PUD     PMD     PT       offset
 *
 * Every directory is an array of PAGING64_PTRS_PER_TBL (512) entries.
 * A non-zero entry of PGD/P4D/PUD/PMD holds the address of the next
 * level table; an entry of PT holds the PTE (format in mm.h).
 * Lower level tables are allocated on demand, so a process only pays
 * for the part of the address space it actually maps.
 * Each process owns its own tree, rooted at mm->pgd.
 */

/* Return the next-level table stored in tbl[idx], allocate it if asked */
static addr_t *
pgtbl_next(addr_t *tbl, addr_t idx, int alloc)
{
  if (tbl[idx] == 0) {
    if (!alloc)
      return NULL;
    addr_t *next = calloc(PAGING64_PTRS_PER_TBL, sizeof(addr_t));
    if (next == NULL)
      return NULL;
    tbl[idx] = (addr_t) next;
  }
  return (addr_t *) tbl[idx];
}

/*
 * pgtbl_walk - walk the 5-level tree of @mm down to the PTE of @pgn
 * @alloc : create missing directories on the way
 * @dirs  : (optional) receives the PGD, P4D, PUD, PMD, PT tables visited
 * Return the PTE address, or NULL if it is not mapped (alloc == 0)
 * or memory is exhausted.
 */
static addr_t *
pgtbl_walk(struct mm_struct *mm, addr_t pgn, int alloc, addr_t *dirs[5])
{
  addr_t idx[5];
  addr_t *tbl;
  int lv;

  if (dirs != NULL)
    for (lv = 0; lv < 5; lv++)
      dirs[lv] = NULL;

  if (mm == NULL || mm->pgd == NULL)
    return NULL;

  get_pd_from_pagenum(pgn, &idx[0], &idx[1], &idx[2], &idx[3], &idx[4]);

  tbl = mm->pgd;
  for (lv = 0; lv < 4; lv++) {
    if (dirs != NULL)
      dirs[lv] = tbl;
    tbl = pgtbl_next(tbl, idx[lv], alloc);
    if (tbl == NULL)
      return NULL;
  }
  if (dirs != NULL)
    dirs[4] = tbl;

  return &tbl[idx[4]];
}

/* The page table of a process lives in its own mm */
static struct mm_struct *
pgtbl_owner(struct mm_struct *mm, struct pcb_t *caller)
{
  if (mm != NULL)
    return mm;
  return (caller != NULL) ? caller->mm : NULL;
}

/*
 * init_pte - Initialize PTE entry
 */
int init_pte(addr_t *pte,
             int pre,    // present
             addr_t fpn,    // FPN
             int drt,    // dirty
             int swp,    // swap
             int swptyp, // swap type
             addr_t swpoff) // swap offset
{
  if (pre != 0) {
    if (swp == 0) { // Non swap ~ page online
      if (fpn == 0)
        return -1;  // Invalid setting

      /* Valid setting with FPN */
      SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
      CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);
      CLRBIT(*pte, PAGING_PTE_DIRTY_MASK);

      SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);
    }
    else
    { // page swapped
      SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
      SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
      CLRBIT(*pte, PAGING_PTE_DIRTY_MASK);

      SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
      SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);
    }
  }

  return 0;
}


/*
 * get_pd_from_pagenum - Parse address to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table 
 */
int get_pd_from_address(addr_t addr, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Extract page direactories */
	*pgd = (addr&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT;
	*p4d = (addr&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT;
	*pud = (addr&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT;
	*pmd = (addr&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT;
	*pt = (addr&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT;

	return 0;
}

/*
 * get_pd_from_pagenum - Parse page number to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table 
 */
int get_pd_from_pagenum(addr_t pgn, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Shift the address to get page num and perform the mapping*/
	return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
                         pgd,p4d,pud,pmd,pt);
}


/*
 * pte_set_swap - Set PTE entry for swapped page
 * @pte    : target page table entry (PTE)
 * @swptyp : swap type
 * @swpoff : swap offset
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  addr_t *pte;

  if (caller == NULL)
    return -1;

  /* The swapped page belongs to the caller's own page table */
  pte = pgtbl_walk(caller->mm, pgn, 1, NULL);
  if (pte == NULL)
    return -1;

  CLRBIT(*pte, PAGING_PTE_PRESENT_MASK);
  SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
  SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);

  return 0;
}

/*
 * pte_set_fpn - Set PTE entry for on-line page
 * @pte   : target page table entry (PTE)
 * @fpn   : frame page number (FPN)
 * @caller: caller
 */
int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller)
{
  addr_t *pte;

  /* FPN field of the PTE is bits 0..12 */
  if (fpn > (PAGING_PTE_FPN_MASK >> PAGING_PTE_FPN_LOBIT)) {
    printf("[ERROR] %s: fpn " FORMAT_ADDR " does not fit in PTE\n",
           __func__, fpn);
    return -1;
  }

  pte = pgtbl_walk(pgtbl_owner(mm, caller), pgn, 1, NULL);
  if (pte == NULL)
    return -1;

  SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
  CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);

  return 0;
}

/* Get PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @caller: caller
 * @ret    : page table entry
 **/
addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  /* Read only: never create directories, an unmapped page reads as 0 */
  addr_t *pte = pgtbl_walk(pgtbl_owner(mm, caller), pgn, 0, NULL);

  return (pte != NULL) ? *pte : 0;
}

/* Set PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @ret    : page table entry
 **/
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
  addr_t *pte = pgtbl_walk(pgtbl_owner(mm, caller), pgn, 1, NULL);

  if (pte == NULL)
    return -1;

  *pte = pte_val;
  return 0;
}

/*
 * vmap_pgd_memset - map a range of page at aligned address
 */
int vmap_pgd_memset(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz
                    int pgnum)                      // num of mapping page
{
  /* Reserve the page table of [addr, addr + pgnum * PAGESZ): build the
   * directories and fill the PTEs with a pattern that is NOT present,
   * so a later access still goes through the normal page fault path.
   */
  addr_t pattern = 0;
  addr_t pgn = PAGING64_ADDR_PGN(addr);
  int pgit;

  if (caller == NULL || pgnum < 0)
    return -1;

  for (pgit = 0; pgit < pgnum; pgit++)
    if (pte_set_entry(caller->mm, pgn + pgit, pattern, caller) != 0)
      return -1;

  return 0;
}

/*
 * vmap_page_range - map a range of page at aligned address
 */
addr_t vmap_page_range(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz // start
                    int pgnum,                      // num of mapping page
                    struct framephy_struct *frames, // list of the mapped frames
                    struct vm_rg_struct *ret_rg)    // return mapped region, the real mapped fp
{                                                   // no guarantee all given pages are mapped
  struct framephy_struct *fpit = frames;
  addr_t pgn = PAGING64_ADDR_PGN(addr);
  int pgit = 0;

  if (caller == NULL || caller->mm == NULL)
    return -1;

  /* Map page by page, in the order of the frame list */
  for (pgit = 0; pgit < pgnum && fpit != NULL; pgit++) {
    if (pte_set_fpn(caller->mm, pgn + pgit, fpit->fpn, caller) != 0)
      break;
    fpit->owner = caller->mm;

    /* Tracking for later page replacement activities */
    enlist_pgn_node(&caller->mm->fifo_pgn, pgn + pgit);
    fpit = fpit->fp_next;
  }

  /* The region really mapped (may be shorter than requested) */
  if (ret_rg != NULL) {
    ret_rg->rg_start = addr;
    ret_rg->rg_end = addr + (addr_t) pgit * PAGING64_PAGESZ;
  }

  return (pgit == pgnum) ? 0 : (addr_t) -1;
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller
 * @req_pgnum : request page num
 * @frm_lst   : frame list
 */

addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
  struct framephy_struct *head = NULL, *tail = NULL, *newfp_str;
  addr_t fpn;
  int pgit;

  if (caller == NULL || caller->krnl == NULL || frm_lst == NULL)
    return -1;

  for (pgit = 0; pgit < req_pgnum; pgit++) {
    if (MEMPHY_get_freefp(caller->krnl->mram, &fpn) != 0) {
      /* Obtained some but not enough frames: give them back so the
       * caller sees an all-or-nothing allocation, report out of memory.
       */
      while (head != NULL) {
        newfp_str = head->fp_next;
        MEMPHY_put_freefp(caller->krnl->mram, head->fpn);
        free(head);
        head = newfp_str;
      }
      *frm_lst = NULL;
      return -3000;
    }

    newfp_str = malloc(sizeof(struct framephy_struct));
    newfp_str->fpn = fpn;
    newfp_str->fp_next = NULL;
    newfp_str->owner = caller->mm;

    /* Keep the request order: page i maps to the i-th frame */
    if (tail == NULL)
      head = newfp_str;
    else
      tail->fp_next = newfp_str;
    tail = newfp_str;
  }

  *frm_lst = head;
  return 0;
}

/*
 * vm_map_ram - do the mapping all vm are to ram storage device
 * @caller    : caller
 * @astart    : vm area start
 * @aend      : vm area end
 * @mapstart  : start mapping point
 * @incpgnum  : number of mapped page
 * @ret_rg    : returned region
 */
addr_t vm_map_ram(struct pcb_t *caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  struct framephy_struct *frm_lst = NULL;
  addr_t ret_alloc = 0;

  /*@bksysnet: author provides a feasible solution of getting frames
   *FATAL logic in here, wrong behaviour if we have not enough page
   *i.e. we request 1000 frames meanwhile our RAM has size of 3 frames
   *Don't try to perform that case in this simple work, it will result
   *in endless procedure of swap-off to get frame and we have not provide
   *duplicate control mechanism, keep it simple
   */
  ret_alloc = alloc_pages_range(caller, incpgnum, &frm_lst);

  if (ret_alloc < 0 && ret_alloc != -3000)
    return -1;

  /* Out of memory */
  if (ret_alloc == -3000)
  {
    return -1;
  }

  /* it leaves the case of memory is enough but half in ram, half in swap
   * do the swaping all to swapper to get the all in ram */
  if (vmap_page_range(caller, mapstart, incpgnum, frm_lst, ret_rg) != 0) {
    /* Unmapped frames go back to RAM */
    struct framephy_struct *fp = frm_lst;
    addr_t mapped = (ret_rg != NULL) ?
      (ret_rg->rg_end - ret_rg->rg_start) / PAGING64_PAGESZ : 0;
    while (fp != NULL && mapped > 0) { fp = fp->fp_next; mapped--; }
    while (fp != NULL) {
      MEMPHY_put_freefp(caller->krnl->mram, fp->fpn);
      fp = fp->fp_next;
    }
    ret_alloc = -1;
  }

  /* The list only carried the frame numbers; the frames themselves are
   * tracked by the PTEs and the used list of MEMRAM */
  while (frm_lst != NULL) {
    struct framephy_struct *next = frm_lst->fp_next;
    free(frm_lst);
    frm_lst = next;
  }

  return (ret_alloc == 0) ? 0 : (addr_t) -1;
}

/* Swap copy content page from source frame to destination frame
 * @mpsrc  : source memphy
 * @srcfpn : source physical page number (FPN)
 * @mpdst  : destination memphy
 * @dstfpn : destination physical page number (FPN)
 **/
int __swap_cp_page(struct memphy_struct *mpsrc, addr_t srcfpn,
                   struct memphy_struct *mpdst, addr_t dstfpn)
{
  int cellidx;
  addr_t addrsrc, addrdst;
#ifdef MM64
  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;
#else
  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;
#endif

    BYTE data;
    MEMPHY_read(mpsrc, addrsrc, &data);
    MEMPHY_write(mpdst, addrdst, data);
  }

  return 0;
}

/*
 *Initialize a empty Memory Management instance
 * @mm:     self mm
 * @caller: mm owner
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  struct vm_area_struct *vma0 = malloc(sizeof(struct vm_area_struct));

  if (mm == NULL || vma0 == NULL)
    return -1;

  /* Page table directory: only the root is allocated, the lower
   * levels are created on demand by pgtbl_walk(). The p4d..pt fields
   * are kept for compatibility and are not used as a cache.
   */
  mm->pgd = calloc(PAGING64_PTRS_PER_TBL, sizeof(addr_t));
  mm->p4d = NULL;
  mm->pud = NULL;
  mm->pmd = NULL;
  mm->pt = NULL;

  /* By default the owner comes with at least one vma */
  vma0->vm_id = 0;
  vma0->vm_start = 0;
  vma0->vm_end = vma0->vm_start;
  vma0->sbrk = vma0->vm_start;
  vma0->vm_freerg_list = NULL;
  struct vm_rg_struct *first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

  vma0->vm_next = NULL;

  /* Point vma owner backward */
  vma0->vm_mm = mm;

  mm->mmap = vma0;
  memset(mm->symrgtbl, 0, sizeof(mm->symrgtbl));
  mm->fifo_pgn = NULL;
  mm->kcpooltbl = NULL;

  (void) caller;
  return 0;
}

struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end)
{
  struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));

  rgnode->rg_start = rg_start;
  rgnode->rg_end = rg_end;
  rgnode->rg_next = NULL;

  return rgnode;
}

int enlist_vm_rg_node(struct vm_rg_struct **rglist, struct vm_rg_struct *rgnode)
{
  rgnode->rg_next = *rglist;
  *rglist = rgnode;

  return 0;
}

int enlist_pgn_node(struct pgn_t **plist, addr_t pgn)
{
  struct pgn_t *pnode = malloc(sizeof(struct pgn_t));

  pnode->pgn = pgn;
  pnode->pg_next = *plist;
  *plist = pnode;

  return 0;
}

int print_list_fp(struct framephy_struct *ifp)
{
  struct framephy_struct *fp = ifp;

  printf("print_list_fp: ");
  if (fp == NULL) { printf("NULL list\n"); return -1;}
  printf("\n");
  while (fp != NULL)
  {
    printf("fp[" FORMAT_ADDR "]\n", fp->fpn);
    fp = fp->fp_next;
  }
  printf("\n");
  return 0;
}

int print_list_rg(struct vm_rg_struct *irg)
{
  struct vm_rg_struct *rg = irg;

  printf("print_list_rg: ");
  if (rg == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (rg != NULL)
  {
    printf("rg[" FORMAT_ADDR "->"  FORMAT_ADDR "]\n", rg->rg_start, rg->rg_end);
    rg = rg->rg_next;
  }
  printf("\n");
  return 0;
}

int print_list_vma(struct vm_area_struct *ivma)
{
  struct vm_area_struct *vma = ivma;

  printf("print_list_vma: ");
  if (vma == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (vma != NULL)
  {
    printf("va[" FORMAT_ADDR "->" FORMAT_ADDR "]\n", vma->vm_start, vma->vm_end);
    vma = vma->vm_next;
  }
  printf("\n");
  return 0;
}

int print_list_pgn(struct pgn_t *ip)
{
  printf("print_list_pgn: ");
  if (ip == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (ip != NULL)
  {
    printf("va[" FORMAT_ADDR "]-\n", ip->pgn);
    ip = ip->pg_next;
  }
  printf("\n");
  return 0;
}

int print_pgtbl(struct pcb_t *caller, addr_t start, addr_t end)
{
  addr_t *dirs[5];

  if (caller == NULL || caller->mm == NULL)
    return -1;

  /* Dump the directories that translate @start (same format as the
   * sample output); missing levels are printed as 0.
   */
  pgtbl_walk(caller->mm, PAGING64_ADDR_PGN(start), 0, dirs);
  printf(" PDG=%016lx P4g=%016lx PUD=%016lx PMD=%016lx\n",
         (unsigned long) dirs[0], (unsigned long) dirs[1],
         (unsigned long) dirs[2], (unsigned long) dirs[3]);

  (void) end;
  return 0;
}

/*
 * print_pgtbl_entries - list every PTE in use of @caller
 * One line per page: virtual page, FPN (present) or swap offset.
 */
int print_pgtbl_entries(struct pcb_t *caller)
{
  addr_t i0, i1, i2, i3, i4;
  addr_t *p4d, *pud, *pmd, *pt;

  if (caller == NULL || caller->mm == NULL || caller->mm->pgd == NULL)
    return -1;

  printf("print_pgtbl_entries: PID %d\n", caller->pid);
  for (i0 = 0; i0 < PAGING64_PTRS_PER_TBL; i0++) {
    if ((p4d = (addr_t *) caller->mm->pgd[i0]) == NULL) continue;
    for (i1 = 0; i1 < PAGING64_PTRS_PER_TBL; i1++) {
      if ((pud = (addr_t *) p4d[i1]) == NULL) continue;
      for (i2 = 0; i2 < PAGING64_PTRS_PER_TBL; i2++) {
        if ((pmd = (addr_t *) pud[i2]) == NULL) continue;
        for (i3 = 0; i3 < PAGING64_PTRS_PER_TBL; i3++) {
          if ((pt = (addr_t *) pmd[i3]) == NULL) continue;
          for (i4 = 0; i4 < PAGING64_PTRS_PER_TBL; i4++) {
            addr_t pte = pt[i4];
            addr_t pgn = (((((i0 << 9 | i1) << 9 | i2) << 9 | i3) << 9) | i4);
            if (PAGING_PAGE_PRESENT(pte))
              printf("  pgn %6lu -> fpn %lu\n",
                     (unsigned long) pgn, (unsigned long) PAGING_FPN(pte));
            else if (pte & PAGING_PTE_SWAPPED_MASK)
              printf("  pgn %6lu -> swap type %lu off %lu\n",
                     (unsigned long) pgn,
                     (unsigned long) PAGING_PTE_SWPTYP(pte),
                     (unsigned long) PAGING_SWP(pte));
          }
        }
      }
    }
  }
  return 0;
}

/*
 * mm64_pgtbl_stats - storage used by the page table tree of @mm
 * @ntbl  : (optional) number of tables per level PGD, P4D, PUD, PMD, PT
 * Return the total size in bytes of all allocated tables.
 */
addr_t mm64_pgtbl_stats(struct mm_struct *mm, int ntbl[5])
{
  int cnt[5] = {0, 0, 0, 0, 0};
  addr_t i0, i1, i2, i3;
  addr_t *p4d, *pud, *pmd;
  int lv;

  if (mm != NULL && mm->pgd != NULL) {
    cnt[0] = 1;
    for (i0 = 0; i0 < PAGING64_PTRS_PER_TBL; i0++) {
      if ((p4d = (addr_t *) mm->pgd[i0]) == NULL) continue;
      cnt[1]++;
      for (i1 = 0; i1 < PAGING64_PTRS_PER_TBL; i1++) {
        if ((pud = (addr_t *) p4d[i1]) == NULL) continue;
        cnt[2]++;
        for (i2 = 0; i2 < PAGING64_PTRS_PER_TBL; i2++) {
          if ((pmd = (addr_t *) pud[i2]) == NULL) continue;
          cnt[3]++;
          for (i3 = 0; i3 < PAGING64_PTRS_PER_TBL; i3++)
            if (pmd[i3] != 0)
              cnt[4]++;
        }
      }
    }
  }

  if (ntbl != NULL)
    for (lv = 0; lv < 5; lv++)
      ntbl[lv] = cnt[lv];

  return (addr_t) (cnt[0] + cnt[1] + cnt[2] + cnt[3] + cnt[4])
         * PAGING64_PTRS_PER_TBL * sizeof(addr_t);
}

/*
 * mm64_free_pgtbl - release every directory of @mm (not the frames)
 */
void mm64_free_pgtbl(struct mm_struct *mm)
{
  addr_t i0, i1, i2, i3;
  addr_t *p4d, *pud, *pmd;

  if (mm == NULL || mm->pgd == NULL)
    return;

  for (i0 = 0; i0 < PAGING64_PTRS_PER_TBL; i0++) {
    if ((p4d = (addr_t *) mm->pgd[i0]) == NULL) continue;
    for (i1 = 0; i1 < PAGING64_PTRS_PER_TBL; i1++) {
      if ((pud = (addr_t *) p4d[i1]) == NULL) continue;
      for (i2 = 0; i2 < PAGING64_PTRS_PER_TBL; i2++) {
        if ((pmd = (addr_t *) pud[i2]) == NULL) continue;
        for (i3 = 0; i3 < PAGING64_PTRS_PER_TBL; i3++)
          free((addr_t *) pmd[i3]);
        free(pmd);
      }
      free(pud);
    }
    free(p4d);
  }
  free(mm->pgd);
  mm->pgd = NULL;
}

#endif  //def MM64
