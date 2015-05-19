#if LAB >= 2
/*
 * PIOS process management definitions.
 *
 * Copyright (C) 2010 Yale University.
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Primary author: Bryan Ford
 */

#ifndef PIOS_KERN_PROC_H
#define PIOS_KERN_PROC_H
#ifndef PIOS_KERNEL
# error "This is a kernel header; user programs should not #include it"
#endif

#include <inc/trap.h>
#include <inc/syscall.h>
#include <inc/label.h>

#include <kern/spinlock.h>
#include <kern/hashtable.h>
#if LAB >= 3
#include <kern/pmap.h>
#endif
#if LAB >= 4
#include <inc/file.h>
#else
#define PROC_CHILDREN	256	// Max # of children a process can have
#endif

typedef enum proc_state {
	PROC_STOP	= 0,	// Passively waiting for parent to run it
	PROC_READY,		// Scheduled to run but not running now
	PROC_RUN,		// Running on some CPU
	PROC_WAIT,		// Waiting to synchronize with child/receiver
	PROC_BLOCK,		// waiting to synchronize with sender
#if LAB >= 5
	PROC_MIGR,		// Migrating to another node
	PROC_AWAY,		// Migrated to another node
	PROC_PULL,		// Migrated to another node
	PROC_SEND,
	PROC_RECV,
#endif
} proc_state;

// Thread control block structure.
// Consumes 1 physical memory page, though we don't use all of it.
typedef struct proc {

	// Master spinlock protecting proc's state.
	spinlock	lock;

	// Process hierarchy information.
	struct proc	*parent;
	struct proc	*child[PROC_CHILDREN];

	// Scheduling state for this process.
	proc_state	state;		// current state
	struct proc	*readynext;	// chain on ready queue
	struct proc	*pacingnext;	// chain on pacing queue
	struct cpu	*runcpu;	// cpu we're running on if running
	struct proc	*waitproc;	// proc waiting for
	uint64_t	ts;		// pacing timestamp

	// Save area for user-visible state when process is not running.
	procstate	sv;
#if LAB >= 3

	// Virtual memory state for this process.

	pte_t		*pml4;		// Working page map level-4
	pte_t		*rpml4;		// Reference page map level-4
#if LAB >= 5

	// Network and process migration state.
	intptr_t	home;		// RR to proc's home node and addr
	intptr_t	rrpml4;		// RR to migration source's page map 
	uint8_t		migrdest;	// Destination we're migrating to
	struct proc	*migrnext;	// Next on list of migrating procs

	// Remote reference pulling state.
	struct proc	*pullnext;	// Next on list of page-pulling procs
	intptr_t	pullva;		// Where we are pulling in our addr spc
	intptr_t	pullrr;		// Current RR we are pulling
	void		*pullpg;	// Local page we are pulling into
	uint8_t		pglev;		// Level: 0=page, 1=page table, 2=pdir
	uint8_t		arrived;	// Bits 0-2: which parts have arrived
#endif
#endif	// LAB >= 3
#if LAB >= 9

	int32_t		pmcmax;		// Max insn count set using perf ctrs
#endif
	uint64_t mid;
	label_t		label;
	label_t		clearance;

	// network remote state
	struct proc	*remotenext;
	uint64_t	remoteid;
	intptr_t	remoteva;
	intptr_t	remotelimit;
} proc;

#define proc_cur()	(cpu_cur()->proc)


// Special "null process" - always just contains zero in all fields.
extern proc proc_null;

// Special "net process" - for net driver
extern proc *proc_net;

// Special root process - the only one that can do direct external I/O.
extern proc *proc_root;

// msgID->proc* mapping
extern hashtable *midtable;

void proc_init(void);	// Initialize process management code
proc *proc_alloc(proc *p, uint32_t cn);	// Allocate new child
void proc_ready(proc *p);	// Make process p ready
void proc_save(proc *p, trapframe *tf, int entry);	// save process state
void proc_wait(proc *p, proc *cp, trapframe *tf, uint64_t wait_ts) gcc_noreturn;
void proc_wake(proc *p, uint64_t time);
void proc_wake_all(uint64_t time);
void proc_sched(void) gcc_noreturn;	// Find and run some ready process
void proc_run(proc *p) gcc_noreturn;	// Run a specific process
void proc_yield(trapframe *tf) gcc_noreturn;	// Yield to another process
void proc_ret(trapframe *tf, int entry) gcc_noreturn;	// Return to parent
void proc_block(proc *p, proc *cp, trapframe *tf) gcc_noreturn; // block cp, wait for p

int proc_set_label(proc *p, tag_t tag);
int proc_set_clearance(proc *p, tag_t tag);

int mid_register(uint64_t mid, proc *p);
void mid_unregister(proc *p);
proc *mid_find(uint64_t mid);

void proc_check(void);			// Check process code


#endif // !PIOS_KERN_PROC_H
#endif // LAB >= 2
