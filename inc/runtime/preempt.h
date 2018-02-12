/*
 * preempt.h - support for kthread preemption
 */

#pragma once

#include <base/stddef.h>
#include <runtime/thread.h>

extern volatile __thread unsigned int preempt_cnt;

#define PREEMPT_NOT_PENDING	(1 << 31)

/**
 * preempt_disable - disables preemption
 *
 * Can be nested.
 */
static inline void preempt_disable(void)
{
	asm volatile("incl %%fs:preempt_cnt@tpoff" : : : "memory", "cc");
	barrier();
}

/**
 * preempt_enable_nocheck - reenables preemption without checking for conditions
 *
 * Can be nested.
 */
static inline void preempt_enable_nocheck(void)
{
	barrier();
	asm volatile("decl %%fs:preempt_cnt@tpoff" : : : "memory", "cc");
}

/**
 * preempt_enable - reenables preemption
 *
 * Can be nested.
 */
static inline void preempt_enable(void)
{
	preempt_enable_nocheck();
	if (unlikely(preempt_cnt == 0))
		thread_yield();
}

/**
 * preempt_needed - returns true if a preemption event is stuck waiting
 */
static inline bool preempt_needed(void)
{
	return (preempt_cnt & PREEMPT_NOT_PENDING) == 0;
}

/**
 * assert_preempt_disabled - asserts that preemption is disabled
 */
static inline void assert_preempt_disabled(void)
{
	assert((preempt_cnt & ~PREEMPT_NOT_PENDING) > 0);
}