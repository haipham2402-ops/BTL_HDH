/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

// #ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Memory physical module mm/mm-memphy.c
 */

#include "mm.h"
#include "mm64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* MEMRAM and MEMSWP are shared by all CPUs: protect the frame lists.
 * Lock order (see team notes): mmvm_lock -> kmm_lock -> memphy_lock
 */
static pthread_mutex_t memphy_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 *  MEMPHY_mv_csr - move MEMPHY cursor
 *  @mp: memphy struct
 *  @offset: offset
 */
int MEMPHY_mv_csr(struct memphy_struct *mp, addr_t offset)
{
   int numstep = 0;

   mp->cursor = 0;
   while (numstep < offset && numstep < mp->maxsz)
   {
      /* Traverse sequentially */
      mp->cursor = (mp->cursor + 1) % mp->maxsz;
      numstep++;
   }

   return 0;
}

/*
 *  MEMPHY_seq_read - read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_seq_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL)
      return -1;

   if (!mp->rdmflg)
      return -1; /* Not compatible mode for sequential read */

   MEMPHY_mv_csr(mp, addr);
   *value = (BYTE)mp->storage[addr];

   return 0;
}

/*
 *  MEMPHY_read read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL)
      return -1;

   if (mp->rdmflg)
      *value = mp->storage[addr];
   else /* Sequential access device */
      return MEMPHY_seq_read(mp, addr, value);

   return 0;
}

/*
 *  MEMPHY_seq_write - write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_seq_write(struct memphy_struct *mp, addr_t addr, BYTE value)
{

   if (mp == NULL)
      return -1;

   if (!mp->rdmflg)
      return -1; /* Not compatible mode for sequential read */

   MEMPHY_mv_csr(mp, addr);
   mp->storage[addr] = value;

   return 0;
}

/*
 *  MEMPHY_write-write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_write(struct memphy_struct *mp, addr_t addr, BYTE data)
{
   if (mp == NULL)
      return -1;

   if (mp->rdmflg)
      mp->storage[addr] = data;
   else /* Sequential access device */
      return MEMPHY_seq_write(mp, addr, data);

   return 0;
}

/*
 *  MEMPHY_format-format MEMPHY device
 *  @mp: memphy struct
 */
int MEMPHY_format(struct memphy_struct *mp, int pagesz)
{
   /* This setting come with fixed constant PAGESZ */
   int numfp = mp->maxsz / pagesz;
   struct framephy_struct *newfst, *fst;
   int iter = 0;

   if (numfp <= 0)
      return -1;

   /* Init head of free framephy list */
   fst = malloc(sizeof(struct framephy_struct));
   fst->fpn = iter;
   fst->fp_next = NULL;
   fst->owner = NULL;
   mp->free_fp_list = fst;

   /* We have list with first element, fill in the rest num-1 element member*/
   for (iter = 1; iter < numfp; iter++)
   {
      newfst = malloc(sizeof(struct framephy_struct));
      newfst->fpn = iter;
      newfst->fp_next = NULL;
      newfst->owner = NULL;
      fst->fp_next = newfst;
      fst = newfst;
   }

   return 0;
}

int MEMPHY_get_freefp(struct memphy_struct *mp, addr_t *retfpn)
{
   struct framephy_struct *fp;

   if (mp == NULL || retfpn == NULL)
      return -1;

   pthread_mutex_lock(&memphy_lock);
   fp = mp->free_fp_list;
   if (fp == NULL) {
      /* MEMPHY is used up: no garbage collector, the caller must
       * swap a page out (or report out of memory) */
      pthread_mutex_unlock(&memphy_lock);
      return -1;
   }

   /* Take the head of the free list and keep the node in the used
    * list to track allocated frames */
   mp->free_fp_list = fp->fp_next;
   *retfpn = fp->fpn;
   fp->fp_next = mp->used_fp_list;
   mp->used_fp_list = fp;
   pthread_mutex_unlock(&memphy_lock);

   return 0;
}

/* Number of nodes of a frame list */
static int count_fp(struct framephy_struct *fp)
{
   int n = 0;

   for (; fp != NULL; fp = fp->fp_next)
      n++;
   return n;
}

int MEMPHY_dump(struct memphy_struct *mp)
{
   addr_t addr;
   int nonzero = 0;

   if (mp == NULL || mp->storage == NULL)
      return -1;

   pthread_mutex_lock(&memphy_lock);
   printf("MEMPHY_dump: size=%lu free_frames=%d used_frames=%d\n",
          (unsigned long) mp->maxsz, count_fp(mp->free_fp_list),
          count_fp(mp->used_fp_list));
   pthread_mutex_unlock(&memphy_lock);

   /* Only the non-zero bytes, a full dump would be megabytes */
   for (addr = 0; addr < mp->maxsz; addr++) {
      if (mp->storage[addr] != 0) {
         printf("  [0x%08lx] = 0x%02x\n", (unsigned long) addr,
                (unsigned char) mp->storage[addr]);
         nonzero++;
      }
   }
   printf("MEMPHY_dump: %d non-zero byte(s)\n", nonzero);

   return 0;
}

int MEMPHY_put_freefp(struct memphy_struct *mp, addr_t fpn)
{
   struct framephy_struct *prev = NULL, *node;

   if (mp == NULL)
      return -1;

   pthread_mutex_lock(&memphy_lock);
   /* Reuse the tracking node of the used list if it is there */
   for (node = mp->used_fp_list; node != NULL; node = node->fp_next) {
      if (node->fpn == fpn)
         break;
      prev = node;
   }
   if (node != NULL) {
      if (prev == NULL)
         mp->used_fp_list = node->fp_next;
      else
         prev->fp_next = node->fp_next;
   } else {
      node = malloc(sizeof(struct framephy_struct));
      node->fpn = fpn;
   }

   /* Create new node with value fpn */
   node->owner = NULL;
   node->fp_next = mp->free_fp_list;
   mp->free_fp_list = node;
   pthread_mutex_unlock(&memphy_lock);

   return 0;
}

/*
 *  Init MEMPHY struct
 */
int init_memphy(struct memphy_struct *mp, addr_t max_size, int randomflg)
{
   mp->free_fp_list = NULL;
   mp->used_fp_list = NULL;
   mp->cursor = 0;
   mp->maxsz = max_size;
   mp->storage = NULL;

   if (max_size == 0)  /* deactivated swap device */
      return 0;

   /* calloc instead of malloc + memset: zero pages are only backed by
    * real memory when written, so a 4GB simulated RAM (MM_FIXED_MEMSZ)
    * does not need 4GB of host memory */
   mp->storage = (BYTE *)calloc(max_size, sizeof(BYTE));
   if (mp->storage == NULL) {
      printf("[ERROR] init_memphy: cannot allocate %lu bytes\n",
             (unsigned long) max_size);
      mp->maxsz = 0;
      return -1;
   }

#ifdef MM64
   MEMPHY_format(mp, PAGING64_PAGESZ);
#else
   MEMPHY_format(mp, PAGING_PAGESZ);
#endif

   mp->rdmflg = (randomflg != 0) ? 1 : 0;

   if (!mp->rdmflg) /* Not Ramdom acess device, then it serial device*/
      mp->cursor = 0;

   return 0;
}

// #endif
