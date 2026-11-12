/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

//#ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Virtual memory module mm/mm-vm.c
 */

#include "string.h"
#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

/*get_vma_by_num - get vm area by numID
 *@mm: memory region
 *@vmaid: ID vm area to alloc memory region
 *
 */
struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  struct vm_area_struct *pvma = (mm != NULL) ? mm->mmap : NULL;

  /* Walk the vma list; the original loop dereferenced NULL when the
   * requested vmaid did not exist */
  while (pvma != NULL && pvma->vm_id != (unsigned long) vmaid)
    pvma = pvma->vm_next;

  return pvma;
}

int __mm_swap_page(struct pcb_t *caller, addr_t vicfpn , addr_t swpfpn)
{
    __swap_cp_page(caller->krnl->mram, vicfpn, caller->krnl->active_mswp, swpfpn);
    return 0;
}

/*get_vm_area_node - get vm area for a number of pages
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid, addr_t alignedsz)
{
  struct vm_rg_struct *newrg;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (cur_vma == NULL)
    return NULL;

  /* The new area starts at the current break and grows upward */
  newrg = malloc(sizeof(struct vm_rg_struct));
  newrg->vmaid = vmaid;
  newrg->rg_start = cur_vma->sbrk;
  newrg->rg_end = cur_vma->sbrk + alignedsz;
  newrg->rg_next = NULL;

  return newrg;
}

/*validate_overlap_vm_area
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  struct vm_area_struct *vma;

  if (vmastart >= vmaend)
    return -1;

  if (caller == NULL || caller->mm == NULL || caller->mm->mmap == NULL)
    return -1;

  if (get_vma_by_num(caller->mm, vmaid) == NULL)
    return -1;

  /* The planned range of area vmaid must not touch any other area
   * (the original code compared the current area with itself) */
  for (vma = caller->mm->mmap; vma != NULL; vma = vma->vm_next) {
    if (vma->vm_id == (unsigned long) vmaid)
      continue;
    if (OVERLAP(vmastart, vmaend, vma->vm_start, vma->vm_end))
      return -1;
  }

  return 0;
}

/*inc_vma_limit - increase vm area limits to reserve space for new variable
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@inc_sz: increment size
 *
 */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
  struct vm_rg_struct newrg;
  struct vm_rg_struct *area;
  struct vm_area_struct *cur_vma;
  addr_t inc_amt;
  int incnumpage;

  if (caller == NULL || inc_sz == 0)
    return -1;

  cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL)
    return -1;

  /* With the 64-bit scheme the raw size is aligned to whole pages */
  inc_amt = PAGING64_PAGE_ALIGNSZ(inc_sz);
  incnumpage = inc_amt / PAGING64_PAGESZ;

  /* New area right at the break */
  area = get_vm_area_node_at_brk(caller, vmaid, inc_amt);
  if (area == NULL)
    return -1;

  if (validate_overlap_vm_area(caller, vmaid, area->rg_start, area->rg_end) < 0) {
    free(area);
    return -1;  /* Overlap and failed allocation */
  }

  /* Get frames and map them; the area only grows when mapping works */
  if (vm_map_ram(caller, area->rg_start, area->rg_end,
                 cur_vma->vm_end, incnumpage, &newrg) != 0) {
    free(area);
    return -1;  /* out of RAM and swap */
  }

  cur_vma->vm_end += inc_amt;
  cur_vma->sbrk += inc_amt;

  free(area);
  return 0;
}

// #endif
