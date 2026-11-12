/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "queue.h"
#include "sched.h"
#include <pthread.h>

#include <stdlib.h>
#include <stdio.h>
static struct queue_t ready_queue;
static struct queue_t run_queue;
static pthread_mutex_t queue_lock;

static struct queue_t running_list;
#ifdef MLQ_SCHED
static struct queue_t mlq_ready_queue[MAX_PRIO];
static int slot[MAX_PRIO];
#endif

int queue_empty(void) {
#ifdef MLQ_SCHED
	unsigned long prio;
	for (prio = 0; prio < MAX_PRIO; prio++)
		if(!empty(&mlq_ready_queue[prio])) 
			return -1;
#endif
	return (empty(&ready_queue) && empty(&run_queue));
}

void init_scheduler(void) {
#ifdef MLQ_SCHED
    int i ;

	for (i = 0; i < MAX_PRIO; i ++) {
		mlq_ready_queue[i].size = 0;
		slot[i] = MAX_PRIO - i; 
	}
#endif
	ready_queue.size = 0;
	run_queue.size = 0;
	running_list.size = 0;
	pthread_mutex_init(&queue_lock, NULL);
}

#ifdef MLQ_SCHED
/* 
 *  Stateful design for routine calling
 *  based on the priority and our MLQ policy
 *  We implement stateful here using transition technique
 *  State representation   prio = 0 .. MAX_PRIO, curr_slot = 0..(MAX_PRIO - prio)
 */
struct pcb_t * get_mlq_proc(void) {
	struct pcb_t * proc = NULL;
	int prio, round;

	pthread_mutex_lock(&queue_lock);
	/* MLQ policy: scan the ready queues from the highest priority
	 * (prio = 0) down. Level prio may dispatch at most
	 * slot[prio] = MAX_PRIO - prio processes per round; once its
	 * slots are used up the CPU goes to the next non-empty level and
	 * the remaining work of that level waits for the next round.
	 * When every non-empty level has used up its slots, a new round
	 * starts and all budgets are refilled.
	 */
	for (round = 0; round < 2 && proc == NULL; round++) {
		for (prio = 0; prio < MAX_PRIO; prio++) {
			if (!empty(&mlq_ready_queue[prio]) && slot[prio] > 0) {
				proc = dequeue(&mlq_ready_queue[prio]);
				slot[prio]--;
				break;
			}
		}
		if (proc == NULL)
			for (prio = 0; prio < MAX_PRIO; prio++)
				slot[prio] = MAX_PRIO - prio;
	}

	if (proc != NULL)
		enqueue(&running_list, proc);
	pthread_mutex_unlock(&queue_lock);
	return proc;	
}

/* Keep prio inside the array of ready queues */
static void
clamp_prio(struct pcb_t *proc)
{
	if (proc->prio >= MAX_PRIO)
		proc->prio = MAX_PRIO - 1;
}

void put_mlq_proc(struct pcb_t * proc) {
	/* The process leaves the CPU: take it out of running_list and
	 * put it back to the ready queue of its own priority level.
	 */
	clamp_prio(proc);
	pthread_mutex_lock(&queue_lock);
	/* os (krnl) is shared by all CPUs and the loader: write it locked */
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->mlq_ready_queue = mlq_ready_queue;
	proc->krnl->running_list = &running_list;
	purgequeue(&running_list, proc);
	enqueue(&mlq_ready_queue[proc->prio], proc);
	pthread_mutex_unlock(&queue_lock);
}

void add_mlq_proc(struct pcb_t * proc) {
	/* A newly loaded process goes to the ready queue of its
	 * priority level; it joins running_list when dispatched.
	 */
	clamp_prio(proc);
	pthread_mutex_lock(&queue_lock);
	/* os (krnl) is shared by all CPUs and the loader: write it locked */
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->mlq_ready_queue = mlq_ready_queue;
	proc->krnl->running_list = &running_list;
	enqueue(&mlq_ready_queue[proc->prio], proc);
	pthread_mutex_unlock(&queue_lock);	
}

struct pcb_t * get_proc(void) {
	return get_mlq_proc();
}

void put_proc(struct pcb_t * proc) {
	return put_mlq_proc(proc);
}

void add_proc(struct pcb_t * proc) {
	return add_mlq_proc(proc);
}
#else
struct pcb_t * get_proc(void) {
	struct pcb_t * proc = NULL;

	pthread_mutex_lock(&queue_lock);
	/* Legacy single-queue mode: when ready_queue runs out, the
	 * processes waiting in run_queue get their turn again.
	 */
	if (empty(&ready_queue)) {
		while (!empty(&run_queue))
			enqueue(&ready_queue, dequeue(&run_queue));
	}
	proc = dequeue(&ready_queue);
	if (proc != NULL)
		enqueue(&running_list, proc);
	pthread_mutex_unlock(&queue_lock);

	return proc;
}

void put_proc(struct pcb_t * proc) {
	/* Leave the CPU: out of running_list, back to run_queue */
	pthread_mutex_lock(&queue_lock);
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;
	purgequeue(&running_list, proc);
	enqueue(&run_queue, proc);
	pthread_mutex_unlock(&queue_lock);
}

void add_proc(struct pcb_t * proc) {
	/* A newly loaded process waits in ready_queue */
	pthread_mutex_lock(&queue_lock);
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;
	enqueue(&ready_queue, proc);
	pthread_mutex_unlock(&queue_lock);	
}
#endif


