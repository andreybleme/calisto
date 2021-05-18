/*
 * kthread.c - support for adding and removing kernel threads
 */

#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <base/atomic.h>
#include <base/cpu.h>
#include <base/list.h>
#include <base/lock.h>
#include <base/log.h>
#include <runtime/sync.h>
#include <runtime/timer.h>

#include "defs.h"

/* protects @ks and @nrks below */
DEFINE_SPINLOCK(klock);
/* the maximum number of kthreads */
unsigned int maxks;
/* the total number of attached kthreads (i.e. the size of @ks) */
unsigned int nrks;
/* the number of busy spinning kthreads (threads that don't park) */
unsigned int spinks;
/* the number of guaranteed kthreads (we can always have this many if we want,
 * must be >= 1) */
unsigned int guaranteedks = 1;
/* the number of active kthreads */
static atomic_t runningks;
/* an array of attached kthreads (@nrks in total) */
struct kthread *ks[NCPU];
/* an array of all kthreads, attached or detached (@maxks in total) */
struct kthread *allks[NCPU];
/* kernel thread-local data */
__thread struct kthread *mykthread;
/* Map of cpu to kthread */
struct cpu_record cpu_map[NCPU] __attribute__((aligned(CACHE_LINE_SIZE)));

static struct kthread *allock(void)
{
	struct kthread *k;

	k = aligned_alloc(CACHE_LINE_SIZE,
			  align_up(sizeof(*k), CACHE_LINE_SIZE));
	if (!k)
		return NULL;

	memset(k, 0, sizeof(*k));
	spin_lock_init(&k->lock);
	list_head_init(&k->rq_overflow);
	mbufq_init(&k->txpktq_overflow);
	mbufq_init(&k->txcmdq_overflow);
	spin_lock_init(&k->timer_lock);
	k->park_efd = eventfd(0, 0);
	BUG_ON(k->park_efd < 0);
	k->detached = true;
	return k;
}

/**
 * kthread_init_thread - initializes state for the kthread
 *
 * Returns 0 if successful, or -ENOMEM if out of memory.
 */
int kthread_init_thread(void)
{
	static int allksn = 0;

	mykthread = allock();
	if (!mykthread)
		return -ENOMEM;

	spin_lock_np(&klock);
	allks[allksn++] = mykthread;
	assert(allksn <= maxks);
	spin_unlock_np(&klock);

	return 0;
}

/**
 * kthread_attach - attaches the thread-local kthread to the runtime if it isn't
 * already attached
 *
 * An attached kthread participates in scheduling, RCU, and I/O.
 */
static void kthread_attach(void)
{
	struct kthread *k = myk();

	assert(k->parked == false);
	assert(k->detached == true);

	k->detached = false;

	spin_lock(&klock);
	assert(nrks < maxks);
	ks[nrks] = k;
	store_release(&nrks, nrks + 1);
	spin_unlock(&klock);
}

/**
 * kthread_detach_locked - detaches a kthread from the runtime
 * @r: the remote kthread to detach
 *
 * @r->lock must be held before calling this function.
 *
 * A detached kthread can no longer be stolen from. It must not receive I/O,
 * have outstanding timers, or participate in RCU.
 */
void kthread_detach(struct kthread *r)
{
	struct kthread *k = myk();
	int i;

	assert_spin_lock_held(&r->lock);
	assert(r != k);
	assert(r->parked == true);
	assert(r->detached == false);

	/* make sure the park rxcmd was processed */
	lrpc_poll_send_tail(&r->txcmdq);
	if (unlikely(lrpc_get_cached_length(&r->txcmdq) > 0))
		return;

	/* one last check, an RX cmd could have squeaked in */
	if (unlikely(!lrpc_empty(&r->rxq)))
		return;

	spin_lock(&klock);
	assert(r != k);
	assert(nrks > 0);
	for (i = 0; i < nrks; i++)
		if (ks[i] == r)
			goto found;
	BUG();

found:
	ks[i] = ks[--nrks];
	spin_unlock(&klock);

	/* steal all overflow packets and completions */
	mbufq_merge_to_tail(&k->txpktq_overflow, &r->txpktq_overflow);
	mbufq_merge_to_tail(&k->txcmdq_overflow, &r->txcmdq_overflow);

	/* merge timer queue into our own */
	timer_merge(r);

	/* verify the kthread is correctly detached */
	assert(r->rq_head == r->rq_tail);
	assert(list_empty(&r->rq_overflow));
	assert(mbufq_empty(&r->txpktq_overflow));
	assert(mbufq_empty(&r->txcmdq_overflow));
	assert(r->timern == 0);

	/* set state */
	r->detached = true;
}

/*
 * kthread_yield_to_iokernel - block on eventfd until iokernel wakes us up
 */
static void kthread_yield_to_iokernel(void)
{
	struct kthread *k = myk();
	ssize_t s;
	uint64_t assigned_core, last_core = k->curr_cpu;

	clear_preempt_needed();

	/* yield to the iokernel */
	s = read(k->park_efd, &assigned_core, sizeof(assigned_core));
	while (unlikely(s != sizeof(uint64_t) && errno == EINTR)) {
		/* preempted while yielding, yield again */
		assert(preempt_needed());
		clear_preempt_needed();
		s = read(k->park_efd, &assigned_core, sizeof(assigned_core));
	}
	BUG_ON(s != sizeof(uint64_t));

	k->curr_cpu = assigned_core - 1;
	if (k->curr_cpu != last_core)
		STAT(CORE_MIGRATIONS)++;
	store_release(&cpu_map[assigned_core - 1].recent_kthread, k);
}

/*
 * kthread_park - block this kthread until the iokernel wakes it up.
 * @voluntary: true if this kthread parked because it had no work left
 *
 * This variant must be called with the local kthread lock held. It is intended
 * for use by the scheduler and for use by signal handlers.
 */
void kthread_park(bool voluntary)
{
	struct kthread *k = myk();
	unsigned long payload = 0;
	uint64_t cmd = TXCMD_PARKED, deadline_us;

	if (!voluntary ||
	    !mbufq_empty(&k->txpktq_overflow) ||
	    !mbufq_empty(&k->txcmdq_overflow)) {
		payload = (unsigned long)k;
	}

	assert_spin_lock_held(&k->lock);
	assert(k->parked == false);

	/* atomically verify we have at least @spinks kthreads running */
	if (atomic_read(&runningks) <= spinks)
		return;
	int remaining_ks = atomic_sub_and_fetch(&runningks, 1);
	if (unlikely(remaining_ks < spinks)) {
		atomic_inc(&runningks);
		return;
	}

	uint64_t now = microtime();

	if (!payload && k->timern) {
		if (remaining_ks) {
			payload = (unsigned long)k;
		} else {
			cmd = TXCMD_PARKED_LAST;
			deadline_us = timer_earliest_deadline();
			if (deadline_us) {
				payload = deadline_us - now;
				if ((int64_t)payload <= 0) {
					cmd = TXCMD_PARKED;
					payload = (unsigned long)k;
				}
			}
		}
	}

	k->parked = true;
	k->park_us = now;
	STAT(PARKS)++;
	spin_unlock(&k->lock);

	/* signal to iokernel that we're about to park */
	while (!lrpc_send(&k->txcmdq, cmd, payload))
		cpu_relax();

	kthread_yield_to_iokernel();

	/* iokernel has unparked us */

	spin_lock(&k->lock);
	k->parked = false;
	atomic_inc(&runningks);

	/* reattach kthread if necessary */
	if (k->detached)
		kthread_attach();
}

/**
 * kthread_wait_to_attach - block this kthread until the iokernel wakes it up.
 *
 * This variant is intended for initialization.
 */
void kthread_wait_to_attach(void)
{
	kthread_yield_to_iokernel();

	/* attach the kthread for the first time */
	kthread_attach();
	atomic_inc(&runningks);
}
