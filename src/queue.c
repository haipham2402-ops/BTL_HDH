#include <stdio.h>
#include <stdlib.h>
#include "queue.h"

int empty(struct queue_t *q)
{
        if (q == NULL)
                return 1;
        return (q->size == 0);
}

void enqueue(struct queue_t *q, struct pcb_t *proc)
{
        /* Put a new process to the tail of queue [q] (FIFO order) */
        if (q == NULL || proc == NULL)
                return;
        if (q->size >= MAX_QUEUE_SIZE) {
                fprintf(stderr, "enqueue: queue full, drop PID %d\n",
                        proc->pid);
                return;
        }
        q->proc[q->size++] = proc;
}

/* Effective priority of a process: smaller value = higher priority */
static uint32_t
proc_priority(struct pcb_t *proc)
{
#ifdef MLQ_SCHED
        return proc->prio;
#else
        return proc->priority;
#endif
}

/* Remove the element at index [idx] and keep the order of the others */
static struct pcb_t *
remove_at(struct queue_t *q, int idx)
{
        struct pcb_t *proc = q->proc[idx];
        int i;

        for (i = idx; i < q->size - 1; i++)
                q->proc[i] = q->proc[i + 1];
        q->proc[--q->size] = NULL;
        return proc;
}

struct pcb_t *dequeue(struct queue_t *q)
{
        /* Return the pcb with the highest priority (smallest value) in
         * queue [q] and remove it from q. Ties are broken by arrival
         * order, so a queue holding a single priority level (MLQ) behaves
         * as a plain FIFO, which gives round-robin inside one level.
         */
        int i, best;

        if (empty(q))
                return NULL;

        best = 0;
        for (i = 1; i < q->size; i++)
                if (proc_priority(q->proc[i]) < proc_priority(q->proc[best]))
                        best = i;

        return remove_at(q, best);
}

struct pcb_t *purgequeue(struct queue_t *q, struct pcb_t *proc)
{
        /* Remove a specific item from queue [q].
         * Return the removed pcb, or NULL if it is not in the queue.
         */
        int i;

        if (empty(q) || proc == NULL)
                return NULL;

        for (i = 0; i < q->size; i++)
                if (q->proc[i] == proc)
                        return remove_at(q, i);

        return NULL;
}