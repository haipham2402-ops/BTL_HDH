/*
 * DMA request queue - Task T5b-1.
 * See include/dma_queue.h. Remember to add dma_queue.o to OS_OBJ in Makefile.
 */

#include "dma_queue.h"
#include <stddef.h>

static struct dma_req_t *head = NULL, *tail = NULL;
static pthread_mutex_t dma_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dma_nonempty = PTHREAD_COND_INITIALIZER;
static int dma_stopping = 0;

void dma_queue_init(void)
{
	/* TODO: reset queue state */
}

int dma_submit(struct dma_req_t *req)
{
	/* TODO: append req under dma_lock, signal dma_nonempty */
	(void)req;
	return -1;
}

int dma_wait(struct dma_req_t *req)
{
	/* TODO: wait on req->done_cv until req->done */
	(void)req;
	return -1;
}

struct dma_req_t *dma_fetch(void)
{
	/* TODO: wait on dma_nonempty, pop head; return NULL when stopping */
	return NULL;
}

void dma_complete(struct dma_req_t *req, int status)
{
	/* TODO: set done/status, broadcast req->done_cv (acts as IRQ) */
	(void)req;
	(void)status;
}

void dma_queue_shutdown(void)
{
	/* TODO: set dma_stopping, broadcast dma_nonempty */
	(void)head; (void)tail; (void)dma_lock; (void)dma_nonempty; (void)dma_stopping;
}
