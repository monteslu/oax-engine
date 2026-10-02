/*
 * wc_thread_start.S: the entry point a wasmcart host calls on a thread the
 * cart spawned with wasi.thread-spawn (wasmcart SPEC.md "Threads").
 *
 * The new thread runs the same module in a fresh instance whose
 * __stack_pointer global still holds the main thread's initial value, so
 * no C may run until it points at this thread's own stack. `arg` is a
 * physThreadArg_t (phys_tasks.c) whose first word is that stack's top.
 */
	.globaltype __stack_pointer, i32
	.functype Phys_ThreadMain (i32, i32) -> ()

	.globl wasi_thread_start
	.export_name wasi_thread_start, wasi_thread_start
	.type wasi_thread_start,@function
wasi_thread_start:
	.functype wasi_thread_start (i32, i32) -> ()
	local.get 1
	i32.load 0
	global.set __stack_pointer
	local.get 0
	local.get 1
	call Phys_ThreadMain
	end_function
