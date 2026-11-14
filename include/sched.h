#ifndef SCHED_H
#define SCHED_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MLQ_SCHED
#define MLQ_SCHED
#endif

#define MAX_PRIO 140

int queue_empty(void);

void init_scheduler(void);
void finish_scheduler(void);

/* Get the next process from ready queue */
struct pcb_t * get_proc(void);

/* Put a process back to run queue */
void put_proc(struct pcb_t * proc);

/* Add a new process to ready queue */
void add_proc(struct pcb_t * proc);

/* Find a live process by PID (kernel side, used by system calls) */
struct pcb_t * find_proc_by_pid(uint32_t pid);

/* Remove a finished process from the kernel lists before freeing it */
void finish_proc(struct pcb_t * proc);

/* DMA process state transitions; these functions serialize with dispatch. */
int scheduler_dma_begin(struct pcb_t *proc);
int scheduler_dma_block(struct pcb_t *proc, int cpu_id);
int scheduler_dma_wake(struct pcb_t *proc);
void scheduler_dma_cancel(struct pcb_t *proc);

#ifdef __cplusplus
}
#endif

#endif
