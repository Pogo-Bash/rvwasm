/* __nq_set_stack_pointer(top): move the C stack (crt/nqstart.c). */
	.globaltype __stack_pointer, i64
	.hidden __nq_set_stack_pointer
	.globl __nq_set_stack_pointer
	.type __nq_set_stack_pointer,@function
__nq_set_stack_pointer:
	.functype __nq_set_stack_pointer (i64) -> ()
	local.get 0
	global.set __stack_pointer
	end_function
