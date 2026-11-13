/*
 * DMA request queue between CPU threads and the co_processor thread.
 * Task T5b-1 (asynchronous operation).
 *
 * NOT YET BUILT: add $(OBJ)/dma_queue.o to OS_OBJ in Makefile
 * once src/dma_queue.c is implemented.
 */

#ifndef DMA_QUEUE_H
#define DMA_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

struct pcb_t;
struct dma_job_t;

/* One DMA transfer request, issued by a CPU, served by co_processor */
struct dma_req_t {
	struct pcb_t *caller;
	struct dma_job_t *job;
	uint64_t src;           /* source bus address      */
	uint64_t dst;           /* destination bus address */
	uint64_t len;           /* bytes                   */
	int done;               /* set to 1 by co_processor (IRQ) */
	int status;             /* 0 = ok, <0 = error      */
	pthread_cond_t done_cv; /* signalled on completion */
	struct dma_req_t *next;
};

void dma_queue_init(void);
/* CPU side: enqueue a request, returns immediately */
int dma_submit(struct dma_req_t *req);
/* Enqueue a prelinked list atomically so the co-processor cannot race setup. */
int dma_submit_batch(struct dma_req_t *first, size_t count);
/* CPU side: block until the request is finished (optional, sync mode) */
int dma_wait(struct dma_req_t *req);
/* co_processor side: block until a request is available */
struct dma_req_t *dma_fetch(void);
/* co_processor side: mark finished and raise "IRQ" */
void dma_complete(struct dma_req_t *req, int status);
/* stop co_processor loop at shutdown */
void dma_queue_shutdown(void);
/* Number of queued or in-flight DMA segments. */
int dma_queue_pending(void);

#endif /* DMA_QUEUE_H */
