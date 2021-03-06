/*
 * Copyright (c) 2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief New thread creation for ARCv2
 *
 * Core thread related primitives for the ARCv2 processor architecture.
 */

#include <kernel.h>
#include <toolchain.h>
#include <kernel_structs.h>
#include <offsets_short.h>
#include <wait_q.h>
#ifdef CONFIG_INIT_STACKS
#include <string.h>
#endif /* CONFIG_INIT_STACKS */

#ifdef CONFIG_USERSPACE
#include <arch/arc/v2/mpu/arc_core_mpu.h>
#endif

/*  initial stack frame */
struct init_stack_frame {
	u32_t pc;
#ifdef CONFIG_ARC_HAS_SECURE
	u32_t sec_stat;
#endif
	u32_t status32;
	u32_t r3;
	u32_t r2;
	u32_t r1;
	u32_t r0;
};

/*
 * @brief Initialize a new thread from its stack space
 *
 * The thread control structure is put at the lower address of the stack. An
 * initial context, to be "restored" by __return_from_coop(), is put at
 * the other end of the stack, and thus reusable by the stack when not
 * needed anymore.
 *
 * The initial context is a basic stack frame that contains arguments for
 * _thread_entry() return address, that points at _thread_entry()
 * and status register.
 *
 * <options> is currently unused.
 *
 * @param pStackmem the pointer to aligned stack memory
 * @param stackSize the stack size in bytes
 * @param pEntry thread entry point routine
 * @param parameter1 first param to entry point
 * @param parameter2 second param to entry point
 * @param parameter3 third param to entry point
 * @param priority thread priority
 * @param options thread options: K_ESSENTIAL
 *
 * @return N/A
 */
void _new_thread(struct k_thread *thread, k_thread_stack_t *stack,
		 size_t stackSize, k_thread_entry_t pEntry,
		 void *parameter1, void *parameter2, void *parameter3,
		 int priority, unsigned int options)
{
	char *pStackMem = K_THREAD_STACK_BUFFER(stack);
	_ASSERT_VALID_PRIO(priority, pEntry);

	char *stackEnd;
	struct init_stack_frame *pInitCtx;

#if CONFIG_USERSPACE
#if CONFIG_ARC_MPU_VER == 2
	stackSize = POW2_CEIL(STACK_SIZE_ALIGN(stackSize));
#elif CONFIG_ARC_MPU_VER == 3
	stackSize = ROUND_UP(stackSize, STACK_ALIGN);
#endif
#endif
	stackEnd = pStackMem + stackSize;

#if CONFIG_USERSPACE
	/* for kernel thread, the privilege stack is merged into thread stack */
	if (!(options & K_USER)) {
	/* if MPU_STACK_GUARD is enabled, reserve the the stack area
	 * |---------------------|    |----------------|
	 * |  user stack         |    | stack guard    |
	 * |---------------------| to |----------------|
	 * |  stack guard        |    | kernel thread  |
	 * |---------------------|    | stack          |
	 * |  privilege stack    |    |                |
	 * ---------------------------------------------
	 */
		pStackMem += STACK_GUARD_SIZE;
		stackSize = stackSize + CONFIG_PRIVILEGED_STACK_SIZE;
		stackEnd += CONFIG_PRIVILEGED_STACK_SIZE + STACK_GUARD_SIZE;
	}
#endif
	_new_thread_init(thread, pStackMem, stackSize, priority, options);

	/* carve the thread entry struct from the "base" of the stack */
	pInitCtx = (struct init_stack_frame *)(STACK_ROUND_DOWN(stackEnd) -
				       sizeof(struct init_stack_frame));
#if CONFIG_USERSPACE
	if (options & K_USER) {
		pInitCtx->pc = ((u32_t)_user_thread_entry_wrapper);
	} else {
		pInitCtx->pc = ((u32_t)_thread_entry_wrapper);
	}
#else
	pInitCtx->pc = ((u32_t)_thread_entry_wrapper);
#endif

#ifdef CONFIG_ARC_HAS_SECURE
	pInitCtx->sec_stat = _arc_v2_aux_reg_read(_ARC_V2_SEC_STAT);
#endif

	pInitCtx->r0 = (u32_t)pEntry;
	pInitCtx->r1 = (u32_t)parameter1;
	pInitCtx->r2 = (u32_t)parameter2;
	pInitCtx->r3 = (u32_t)parameter3;
	/*
	 * For now set the interrupt priority to 15
	 * we can leave interrupt enable flag set to 0 as
	 * seti instruction in the end of the _Swap() will
	 * enable the interrupts based on intlock_key
	 * value.
	 */
#ifdef CONFIG_ARC_STACK_CHECKING
	pInitCtx->status32 = _ARC_V2_STATUS32_SC |
		 _ARC_V2_STATUS32_E(_ARC_V2_DEF_IRQ_LEVEL);
	thread->arch.stack_base = (u32_t) stackEnd;
#else
	pInitCtx->status32 = _ARC_V2_STATUS32_E(_ARC_V2_DEF_IRQ_LEVEL);
#endif

#if CONFIG_USERSPACE
	/*
	 * enable US bit, US is read as zero in user mode. This will allow use
	 * mode sleep instructions, and it enables a form of denial-of-service
	 * attack by putting the processor in sleep mode, but since interrupt
	 * level/mask can't be set from user space that's not worse than
	 * executing a loop without yielding.
	 */
	pInitCtx->status32 |= _ARC_V2_STATUS32_US;

	if (options & K_USER) {
		thread->arch.priv_stack_start =
			(u32_t) (stackEnd + STACK_GUARD_SIZE);
		thread->arch.priv_stack_size =
			(u32_t)CONFIG_PRIVILEGED_STACK_SIZE;
	} else {
		thread->arch.priv_stack_start = 0;
		thread->arch.priv_stack_size = 0;
	}
#endif

#ifdef CONFIG_THREAD_MONITOR
	/*
	 * In debug mode thread->entry give direct access to the thread entry
	 * and the corresponding parameters.
	 */
	thread->entry = (struct __thread_entry *)(pInitCtx);
#endif

	/*
	 * intlock_key is constructed based on ARCv2 ISA Programmer's
	 * Reference Manual CLRI instruction description:
	 * dst[31:6] dst[5] dst[4]       dst[3:0]
	 *    26'd0    1    STATUS32.IE  STATUS32.E[3:0]
	 */
	thread->arch.intlock_key = 0x3F;
	thread->arch.relinquish_cause = _CAUSE_COOP;
	thread->callee_saved.sp =
		(u32_t)pInitCtx - ___callee_saved_stack_t_SIZEOF;

	/* initial values in all other regs/k_thread entries are irrelevant */

	thread_monitor_init(thread);
}


#ifdef CONFIG_USERSPACE

FUNC_NORETURN void _arch_user_mode_enter(k_thread_entry_t user_entry,
	void *p1, void *p2, void *p3)
{
	_current->base.user_options |= K_USER;

	/*
	 * ajust the thread stack layout
	 * |----------------|    |---------------------|
	 * | stack guard    |    |  user stack         |
	 * |----------------| to |---------------------|
	 * | kernel thread  |    |  stack guard        |
	 * | stack          |    |---------------------|
	 * |                |    |  privilege stack    |
	 * ---------------------------------------------
	 */
	_current->stack_info.start = (u32_t)_current->stack_obj;
	_current->stack_info.size -= CONFIG_PRIVILEGED_STACK_SIZE;

	_current->arch.priv_stack_start =
			(u32_t) (_current->stack_info.start +
				 _current->stack_info.size + STACK_GUARD_SIZE);
	_current->arch.priv_stack_size =
			(u32_t)CONFIG_PRIVILEGED_STACK_SIZE;

	/* possible optimizaiton: no need to load mem domain anymore */
	/* need to lock cpu here ? */
	configure_mpu_thread(_current);

	_arc_userspace_enter(user_entry, p1, p2, p3,
			     (u32_t)_current->stack_obj,
			     _current->stack_info.size);
	CODE_UNREACHABLE;
}

#endif
