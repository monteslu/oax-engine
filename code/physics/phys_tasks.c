/*
===========================================================================
oax engine
Copyright (C) 2026 Luis Montes

This file is part of the oax engine, a fork of ioquake3.
It is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version. The combined engine is distributed under GPLv3 (see
COPYING-GPLv3.txt).

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
more details.
===========================================================================
*/

/*
===========================================================================
phys_tasks.c: worker threads for Box3D.

Native builds let Box3D run its own scheduler (b3WorldDef.workerCount with
no task callbacks: Box3D starts workerCount - 1 threads per world).

The cart cannot: Box3D's scheduler makes Emscripten pthreads, which need
Emscripten's JavaScript runtime, and a cart has none. wasmcart gives carts
WASI threads instead (SPEC.md "Threads": the `wasi.thread-spawn` import
plus a shared, imported memory; the host runs the same module on a new
thread and calls its `wasi_thread_start(tid, arg)` export). So on the cart
this file is Box3D's task system:

  - Up to OAX_PHYS_MAX_WORKERS - 1 threads, spawned the first time a world
    asks for more than one worker, and kept for the life of the cart (a
    wasm thread cannot be stopped from outside).
  - wc_thread_start.S sets each thread's stack pointer to the stack this
    file allocated for it, then calls Phys_ThreadMain. Workers run only
    Box3D task callbacks: no libc beyond memcpy/memset, and Box3D's
    allocations go through phys_main.c's spinlock.
  - Enqueue writes the task into a slot and bumps a futex word; idle
    workers sleep in memory.atomic.wait32 on it. The main thread never
    waits (a browser main thread may not): in finishTask it runs pending
    tasks itself until its task is done, as Box3D's own scheduler does.

Which thread runs which task does not change the result: Box3D's solver
partitions work by worker index and merges in a fixed order, so a step is
bit-identical for any worker count (the physics-determinism test proves it
on both builds).
===========================================================================
*/

#include "phys_local.h"

#ifndef WASMCART_THREADS

qboolean Phys_TasksAvailable( void ) {
#if defined( WASMCART ) || defined( __EMSCRIPTEN__ )
	// a cart has no Emscripten runtime, and a plain Emscripten build is not
	// built with -pthread: Box3D's pthread_create would fail
	return qfalse;
#else
	return qtrue;
#endif
}

void Phys_TasksSetup( b3WorldDef *def, int workerCount ) {
	(void)def;
	(void)workerCount;	// Box3D's internal scheduler
}

void Phys_TasksBeginStep( void ) {
}

void Phys_TasksPublish( void ) {
}

#else

#include <stdlib.h>

#define PHYS_TASK_MAX			B3_MAX_TASKS
#define PHYS_THREAD_STACK		( 1024 * 1024 )

enum {
	PHYS_TASK_FREE,
	PHYS_TASK_PENDING,
	PHYS_TASK_CLAIMED,
	PHYS_TASK_DONE
};

typedef struct {
	b3TaskCallback	*callback;
	void			*context;
	int				status;
} physTask_t;

// what wasi_thread_start reads: the stack pointer FIRST (wc_thread_start.S)
typedef struct {
	uint32_t		stackTop;
	int				index;
} physThreadArg_t;

__attribute__(( import_module( "wasi" ), import_name( "thread-spawn" ) ))
int32_t phys_wasi_thread_spawn( void *arg );

static physTask_t		phys_tasks[PHYS_TASK_MAX];
static int				phys_nextSlot;
static int				phys_wake;			// futex word: bumped by every enqueue
static int				phys_numThreads;
static int				phys_threadsFailed;
static physThreadArg_t	phys_threadArgs[OAX_PHYS_MAX_WORKERS];
static int				phys_tasksRun;		// every task run since the cart started
static int				phys_tasksOnWorkers;	// the ones a worker thread ran

static qboolean Phys_RunOne( qboolean worker ) {
	int n = __atomic_load_n( &phys_nextSlot, __ATOMIC_ACQUIRE );
	int i;

	if ( n > PHYS_TASK_MAX ) {
		n = PHYS_TASK_MAX;
	}
	for ( i = 0; i < n; i++ ) {
		physTask_t *t = &phys_tasks[i];
		int expect = PHYS_TASK_PENDING;
		if ( __atomic_load_n( &t->status, __ATOMIC_ACQUIRE ) != PHYS_TASK_PENDING ) {
			continue;
		}
		if ( !__atomic_compare_exchange_n( &t->status, &expect, PHYS_TASK_CLAIMED, qfalse,
				__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ) ) {
			continue;
		}
		t->callback( t->context );
		__atomic_fetch_add( &phys_tasksRun, 1, __ATOMIC_RELAXED );
		if ( worker ) {
			__atomic_fetch_add( &phys_tasksOnWorkers, 1, __ATOMIC_RELAXED );
		}
		__atomic_store_n( &t->status, PHYS_TASK_DONE, __ATOMIC_RELEASE );
		return qtrue;
	}
	return qfalse;
}

// every worker thread lives here (called from wc_thread_start.S)
void Phys_ThreadMain( int tid, physThreadArg_t *arg ) {
	(void)tid;
	(void)arg;
	for ( ;; ) {
		int seen = __atomic_load_n( &phys_wake, __ATOMIC_ACQUIRE );
		while ( Phys_RunOne( qtrue ) ) {
		}
		// sleeps only if nothing was enqueued since `seen`
		__builtin_wasm_memory_atomic_wait32( &phys_wake, seen, -1 );
	}
}

static void *Phys_Enqueue( b3TaskCallback *task, void *taskContext, void *userContext, const char *name ) {
	int slot = __atomic_fetch_add( &phys_nextSlot, 1, __ATOMIC_ACQ_REL );
	physTask_t *t;

	(void)userContext;
	(void)name;
	if ( slot >= PHYS_TASK_MAX ) {
		task( taskContext );	// out of slots: run it here (NULL = done, no finish call)
		return NULL;
	}
	t = &phys_tasks[slot];
	t->callback = task;
	t->context = taskContext;
	__atomic_store_n( &t->status, PHYS_TASK_PENDING, __ATOMIC_RELEASE );
	__atomic_fetch_add( &phys_wake, 1, __ATOMIC_ACQ_REL );
	__builtin_wasm_memory_atomic_notify( &phys_wake, 1 );
	return t;
}

static void Phys_Finish( void *userTask, void *userContext ) {
	physTask_t *t = userTask;

	(void)userContext;
	if ( !t ) {
		return;
	}
	while ( __atomic_load_n( &t->status, __ATOMIC_ACQUIRE ) != PHYS_TASK_DONE ) {
		Phys_RunOne( qfalse );
	}
}

static void Phys_SpawnThreads( int want ) {
	while ( phys_numThreads < want && !phys_threadsFailed ) {
		physThreadArg_t *a = &phys_threadArgs[phys_numThreads];
		char *stack = aligned_alloc( 16, PHYS_THREAD_STACK );
		int tid;

		if ( !stack ) {
			phys_threadsFailed = 1;
			break;
		}
		a->stackTop = (uint32_t)(uintptr_t)( stack + PHYS_THREAD_STACK );
		a->index = phys_numThreads + 1;
		tid = phys_wasi_thread_spawn( a );
		if ( tid <= 0 ) {
			Com_Printf( S_COLOR_YELLOW "physics: wasi thread-spawn failed (%d); worlds run one worker\n", tid );
			free( stack );
			phys_threadsFailed = 1;
			break;
		}
		phys_numThreads++;
	}
	Com_DebugSetInt( "phys_threads", phys_numThreads );
}

qboolean Phys_TasksAvailable( void ) {
	return !phys_threadsFailed;
}

void Phys_TasksSetup( b3WorldDef *def, int workerCount ) {
	Phys_SpawnThreads( OAX_PHYS_MAX_WORKERS - 1 );
	if ( phys_numThreads == 0 ) {
		def->workerCount = 1;
		return;
	}
	def->workerCount = workerCount;
	def->enqueueTask = Phys_Enqueue;
	def->finishTask = Phys_Finish;
	def->userTaskContext = NULL;
}

// the cart's task counters as debug values: a multi-worker world must have
// tasks run on the worker threads, or the threads are not doing anything
void Phys_TasksPublish( void ) {
	Com_DebugSetInt( "phys_tasks", __atomic_load_n( &phys_tasksRun, __ATOMIC_RELAXED ) );
	Com_DebugSetInt( "phys_tasks_on_workers", __atomic_load_n( &phys_tasksOnWorkers, __ATOMIC_RELAXED ) );
}

// all of the previous step's tasks are done (Box3D finished each of them),
// so the slots start over
void Phys_TasksBeginStep( void ) {
	__atomic_store_n( &phys_nextSlot, 0, __ATOMIC_RELEASE );
}

#endif
