/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * DMA Interface Header
 */

#ifndef __DMA_H__
#define __DMA_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct pcb_t;

/* Engine calls use byte offsets into the OS MEMRAM and block until completion. */
int dma_engine_init(void *ram, uint64_t ram_size);
int dma_engine_copy(uint64_t src_phys, uint64_t dst_phys, uint64_t len);

/* Return value from libmem_dma_memcpy when the process was queued for DMA. */
#define DMA_MEMCPY_PENDING 2

#ifdef __cplusplus
}
#endif

#endif /* __DMA_H__ */