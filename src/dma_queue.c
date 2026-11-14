#include "dma_queue.h"
#include <stddef.h>

static struct dma_req_t *head = NULL, *tail = NULL;
static pthread_mutex_t dma_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dma_nonempty = PTHREAD_COND_INITIALIZER;
static int dma_stopping = 0;
static int dma_outstanding = 0;

void dma_queue_init(void)
{
	pthread_mutex_lock(&dma_lock);
	head = NULL;
	tail = NULL;
	dma_stopping = 0;
	dma_outstanding = 0;
	pthread_mutex_unlock(&dma_lock);
}

int dma_submit(struct dma_req_t *req)
{
	if (req == NULL)
		return -1;
	req->next = NULL;
	return dma_submit_batch(req, 1);
}

int dma_submit_batch(struct dma_req_t *first, size_t count)
{
	struct dma_req_t *last;

	if (first == NULL || count == 0)
		return -1;
	last = first;
	for (size_t i = 1; i < count; i++) {
		if (last->next == NULL)
			return -1;
		last = last->next;
	}
	last->next = NULL;
	pthread_mutex_lock(&dma_lock);
	if (dma_stopping) {
		pthread_mutex_unlock(&dma_lock);
		return -1;
	}

	if (tail == NULL)
		head = first;
	else
		tail->next = first;
	tail = last;
	dma_outstanding += (int) count;
	pthread_cond_broadcast(&dma_nonempty);
	pthread_mutex_unlock(&dma_lock);
	return 0;
}

int dma_wait(struct dma_req_t *req)
{
	if (req == NULL)
		return -1;

	pthread_mutex_lock(&dma_lock);
	while (!req->done)
		pthread_cond_wait(&req->done_cv, &dma_lock);
	int status = req->status;
	pthread_mutex_unlock(&dma_lock);
	return status;
}

struct dma_req_t *dma_fetch(void)
{
	struct dma_req_t *req;

	pthread_mutex_lock(&dma_lock);
	while (head == NULL && !dma_stopping)
		pthread_cond_wait(&dma_nonempty, &dma_lock);

	req = head;
	if (req != NULL) {
		head = req->next;
		if (head == NULL)
			tail = NULL;
		req->next = NULL;
	}
	pthread_mutex_unlock(&dma_lock);
	return req;
}

void dma_complete(struct dma_req_t *req, int status)
{
	if (req == NULL)
		return;

	pthread_mutex_lock(&dma_lock);
	req->status = status;
	req->done = 1;
	if (dma_outstanding > 0)
		dma_outstanding--;
	pthread_cond_broadcast(&req->done_cv);
	pthread_mutex_unlock(&dma_lock);
}

void dma_queue_shutdown(void)
{
	pthread_mutex_lock(&dma_lock);
	dma_stopping = 1;
	pthread_cond_broadcast(&dma_nonempty);
	pthread_mutex_unlock(&dma_lock);
}

int dma_queue_pending(void)
{
	int pending;

	pthread_mutex_lock(&dma_lock);
	pending = dma_outstanding;
	pthread_mutex_unlock(&dma_lock);
	return pending;
}
