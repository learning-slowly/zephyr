/*
 * Copyright (c) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <kernel.h>
#include <ksched.h>
#include <spinlock.h>
#include <kernel/sched_priq.h>
#include <wait_q.h>
#include <kswap.h>
#include <kernel_arch_func.h>
#include <syscall_handler.h>
#include <drivers/timer/system_timer.h>
#include <stdbool.h>
#include <kernel_internal.h>
#include <logging/log.h>
#include <sys/atomic.h>
LOG_MODULE_DECLARE(os, CONFIG_KERNEL_LOG_LEVEL);

#if defined(CONFIG_SCHED_DUMB)
#define _priq_run_add		z_priq_dumb_add
#define _priq_run_remove	z_priq_dumb_remove
# if defined(CONFIG_SCHED_CPU_MASK)
#  define _priq_run_best	_priq_dumb_mask_best
# else
#  define _priq_run_best	z_priq_dumb_best
# endif
#elif defined(CONFIG_SCHED_SCALABLE)
#define _priq_run_add		z_priq_rb_add
#define _priq_run_remove	z_priq_rb_remove
#define _priq_run_best		z_priq_rb_best
#elif defined(CONFIG_SCHED_MULTIQ)
#define _priq_run_add		z_priq_mq_add
#define _priq_run_remove	z_priq_mq_remove
#define _priq_run_best		z_priq_mq_best
#endif

#if defined(CONFIG_WAITQ_SCALABLE)
#define z_priq_wait_add		z_priq_rb_add
#define _priq_wait_remove	z_priq_rb_remove
#define _priq_wait_best		z_priq_rb_best
#elif defined(CONFIG_WAITQ_DUMB)
#define z_priq_wait_add		z_priq_dumb_add
#define _priq_wait_remove	z_priq_dumb_remove
#define _priq_wait_best		z_priq_dumb_best
#endif

struct k_spinlock sched_spinlock;

static void update_cache(int preempt_ok);
static void end_thread(struct k_thread *thread);

static inline int is_preempt(struct k_thread *thread)
{
	/* explanation in kernel_struct.h */
	return thread->base.preempt <= _PREEMPT_THRESHOLD;
}

static inline int is_metairq(struct k_thread *thread)
{
#if CONFIG_NUM_METAIRQ_PRIORITIES > 0
	return (thread->base.prio - K_HIGHEST_THREAD_PRIO)
		< CONFIG_NUM_METAIRQ_PRIORITIES;
#else
	return 0;
#endif
}

#if CONFIG_ASSERT
static inline bool is_thread_dummy(struct k_thread *thread)
{
	return (thread->base.thread_state & _THREAD_DUMMY) != 0U;
}
#endif

/*
 * Return value same as e.g. memcmp
 * > 0 -> thread 1 priority  > thread 2 priority
 * = 0 -> thread 1 priority == thread 2 priority
 * < 0 -> thread 1 priority  < thread 2 priority
 * Do not rely on the actual value returned aside from the above.
 * (Again, like memcmp.)
 */
int32_t z_sched_prio_cmp(struct k_thread *thread_1,
	struct k_thread *thread_2)
{
	/* `prio` is <32b, so the below cannot overflow. */
	int32_t b1 = thread_1->base.prio;
	int32_t b2 = thread_2->base.prio;

	if (b1 != b2) {
		return b2 - b1;
	}

#ifdef CONFIG_SCHED_DEADLINE
	/* If we assume all deadlines live within the same "half" of
	 * the 32 bit modulus space (this is a documented API rule),
	 * then the latest deadline in the queue minus the earliest is
	 * guaranteed to be (2's complement) non-negative.  We can
	 * leverage that to compare the values without having to check
	 * the current time.
	 */
	uint32_t d1 = thread_1->base.prio_deadline;
	uint32_t d2 = thread_2->base.prio_deadline;

	if (d1 != d2) {
		/* Sooner deadline means higher effective priority.
		 * Doing the calculation with unsigned types and casting
		 * to signed isn't perfect, but at least reduces this
		 * from UB on overflow to impdef.
		 */
		return (int32_t) (d2 - d1);
	}
#endif
	return 0;
}

static ALWAYS_INLINE bool should_preempt(struct k_thread *thread,
					 int preempt_ok)
{
	/* Preemption is OK if it's being explicitly allowed by
	 * software state (e.g. the thread called k_yield())
	 */
	if (preempt_ok != 0) {
		return true;
	}

	__ASSERT(_current != NULL, "");

	/* Or if we're pended/suspended/dummy (duh) */
	if (z_is_thread_prevented_from_running(_current)) {
		return true;
	}

	/* Edge case on ARM where a thread can be pended out of an
	 * interrupt handler before the "synchronous" swap starts
	 * context switching.  Platforms with atomic swap can never
	 * hit this.
	 */
	if (IS_ENABLED(CONFIG_SWAP_NONATOMIC)
	    && z_is_thread_timeout_active(thread)) {
		return true;
	}

	/* Otherwise we have to be running a preemptible thread or
	 * switching to a metairq
	 */
	if (is_preempt(_current) || is_metairq(thread)) {
		return true;
	}

	return false;
}

#ifdef CONFIG_SCHED_CPU_MASK
static ALWAYS_INLINE struct k_thread *_priq_dumb_mask_best(sys_dlist_t *pq)
{
	/* With masks enabled we need to be prepared to walk the list
	 * looking for one we can run
	 */
	struct k_thread *thread;

	SYS_DLIST_FOR_EACH_CONTAINER(pq, thread, base.qnode_dlist) {
		if ((thread->base.cpu_mask & BIT(_current_cpu->id)) != 0) {
			return thread;
		}
	}
	return NULL;
}
#endif

/* _current is never in the run queue until context switch on
 * SMP configurations, see z_requeue_current()
 */
static inline bool should_queue_thread(struct k_thread *th)
{
	return !IS_ENABLED(CONFIG_SMP) || th != _current;
}

static ALWAYS_INLINE void queue_thread(void *pq,
				       struct k_thread *thread)
{
	thread->base.thread_state |= _THREAD_QUEUED;
	if (should_queue_thread(thread)) {
		_priq_run_add(pq, thread);
	}
#ifdef CONFIG_SMP
	if (thread == _current) {
		/* add current to end of queue means "yield" */
		_current_cpu->swap_ok = true;
	}
#endif
}

static ALWAYS_INLINE void dequeue_thread(void *pq,
					 struct k_thread *thread)
{
	thread->base.thread_state &= ~_THREAD_QUEUED;
	if (should_queue_thread(thread)) {
		_priq_run_remove(pq, thread);
	}
}

#ifdef CONFIG_SMP
/* Called out of z_swap() when CONFIG_SMP.  The current thread can
 * never live in the run queue until we are inexorably on the context
 * switch path on SMP, otherwise there is a deadlock condition where a
 * set of CPUs pick a cycle of threads to run and wait for them all to
 * context switch forever.
 */
void z_requeue_current(struct k_thread *curr)
{
	if (z_is_thread_queued(curr)) {
		_priq_run_add(&_kernel.ready_q.runq, curr);
	}
}
#endif

static inline bool is_aborting(struct k_thread *thread)
{
	return (thread->base.thread_state & _THREAD_ABORTING) != 0U;
}

static ALWAYS_INLINE struct k_thread *next_up(void)
{
	struct k_thread *thread;

	thread = _priq_run_best(&_kernel.ready_q.runq);

#if (CONFIG_NUM_METAIRQ_PRIORITIES > 0) && (CONFIG_NUM_COOP_PRIORITIES > 0)
	/* MetaIRQs must always attempt to return back to a
	 * cooperative thread they preempted and not whatever happens
	 * to be highest priority now. The cooperative thread was
	 * promised it wouldn't be preempted (by non-metairq threads)!
	 */
	struct k_thread *mirqp = _current_cpu->metairq_preempted;

	if (mirqp != NULL && (thread == NULL || !is_metairq(thread))) {
		if (!z_is_thread_prevented_from_running(mirqp)) {
			thread = mirqp;
		} else {
			_current_cpu->metairq_preempted = NULL;
		}
	}
#endif

#ifndef CONFIG_SMP
	/* In uniprocessor mode, we can leave the current thread in
	 * the queue (actually we have to, otherwise the assembly
	 * context switch code for all architectures would be
	 * responsible for putting it back in z_swap and ISR return!),
	 * which makes this choice simple.
	 */
	return (thread != NULL) ? thread : _current_cpu->idle_thread;
#else
	/* Under SMP, the "cache" mechanism for selecting the next
	 * thread doesn't work, so we have more work to do to test
	 * _current against the best choice from the queue.  Here, the
	 * thread selected above represents "the best thread that is
	 * not current".
	 *
	 * Subtle note on "queued": in SMP mode, _current does not
	 * live in the queue, so this isn't exactly the same thing as
	 * "ready", it means "is _current already added back to the
	 * queue such that we don't want to re-add it".
	 */
	if (is_aborting(_current)) {
		end_thread(_current);
	}

	int queued = z_is_thread_queued(_current);
	int active = !z_is_thread_prevented_from_running(_current);

	if (thread == NULL) {
		thread = _current_cpu->idle_thread;
	}

	if (active) {
		int32_t cmp = z_sched_prio_cmp(_current, thread);

		/* Ties only switch if state says we yielded */
		if ((cmp > 0) || ((cmp == 0) && !_current_cpu->swap_ok)) {
			thread = _current;
		}

		if (!should_preempt(thread, _current_cpu->swap_ok)) {
			thread = _current;
		}
	}

	/* Put _current back into the queue */
	if (thread != _current && active &&
		!z_is_idle_thread_object(_current) && !queued) {
		queue_thread(&_kernel.ready_q.runq, _current);
	}

	/* Take the new _current out of the queue */
	if (z_is_thread_queued(thread)) {
		dequeue_thread(&_kernel.ready_q.runq, thread);
	}

	_current_cpu->swap_ok = false;
	return thread;
#endif
}

static void move_thread_to_end_of_prio_q(struct k_thread *thread)
{
	if (z_is_thread_queued(thread)) {
		dequeue_thread(&_kernel.ready_q.runq, thread);
	}
	queue_thread(&_kernel.ready_q.runq, thread);
	update_cache(thread == _current);
}

#ifdef CONFIG_TIMESLICING

static int slice_time;
static int slice_max_prio;

#ifdef CONFIG_SWAP_NONATOMIC
/* If z_swap() isn't atomic, then it's possible for a timer interrupt
 * to try to timeslice away _current after it has already pended
 * itself but before the corresponding context switch.  Treat that as
 * a noop condition in z_time_slice().
 */
static struct k_thread *pending_current;
#endif

void z_reset_time_slice(void)
{
	/* Add the elapsed time since the last announced tick to the
	 * slice count, as we'll see those "expired" ticks arrive in a
	 * FUTURE z_time_slice() call.
	 */
	if (slice_time != 0) {
		_current_cpu->slice_ticks = slice_time + sys_clock_elapsed();
		z_set_timeout_expiry(slice_time, false);
	}
}

void k_sched_time_slice_set(int32_t slice, int prio)
{
	LOCKED(&sched_spinlock) {
		/* LS: 타임 슬라이스는 스레드별이 아니라 시스템 전역으로 적용됨 */
		_current_cpu->slice_ticks = 0;
		slice_time = k_ms_to_ticks_ceil32(slice);
		if (IS_ENABLED(CONFIG_TICKLESS_KERNEL) && slice > 0) {
			/* It's not possible to reliably set a 1-tick
			 * timeout if ticks aren't regular.
			 */
			slice_time = MAX(2, slice_time);
		}
		slice_max_prio = prio;
		z_reset_time_slice();
	}
}

static inline int sliceable(struct k_thread *thread)
{
	return is_preempt(thread)
		&& !z_is_thread_prevented_from_running(thread)
		&& !z_is_prio_higher(thread->base.prio, slice_max_prio)
		&& !z_is_idle_thread_object(thread);
}

/* Called out of each timer interrupt */
void z_time_slice(int ticks)
{
	/* Hold sched_spinlock, so that activity on another CPU
	 * (like a call to k_thread_abort() at just the wrong time)
	 * won't affect the correctness of the decisions made here.
	 * Also prevents any nested interrupts from changing
	 * thread state to avoid similar issues, since this would
	 * normally run with IRQs enabled.
	 */
	k_spinlock_key_t key = k_spin_lock(&sched_spinlock);

#ifdef CONFIG_SWAP_NONATOMIC
	if (pending_current == _current) {
		z_reset_time_slice();
		k_spin_unlock(&sched_spinlock, key);
		return;
	}
	pending_current = NULL;
#endif

	if (slice_time && sliceable(_current)) {
		if (ticks >= _current_cpu->slice_ticks) {
			move_thread_to_end_of_prio_q(_current);
			z_reset_time_slice();
		} else {
			_current_cpu->slice_ticks -= ticks;
		}
	} else {
		_current_cpu->slice_ticks = 0;
	}
	k_spin_unlock(&sched_spinlock, key);
}
#endif

/* Track cooperative threads preempted by metairqs so we can return to
 * them specifically.  Called at the moment a new thread has been
 * selected to run.
 */
static void update_metairq_preempt(struct k_thread *thread)
{
#if (CONFIG_NUM_METAIRQ_PRIORITIES > 0) && (CONFIG_NUM_COOP_PRIORITIES > 0)
	if (is_metairq(thread) && !is_metairq(_current) &&
	    !is_preempt(_current)) {
		/* Record new preemption */
		_current_cpu->metairq_preempted = _current;
	} else if (!is_metairq(thread) && !z_is_idle_thread_object(thread)) {
		/* Returning from existing preemption */
		_current_cpu->metairq_preempted = NULL;
	}
#endif
}

static void update_cache(int preempt_ok)
{
    /* LS: 단일 CPU 시스템에서 커널의 레디큐 "cache" 에 실행할 다음 스레드를 유지한다.  
     * 문맥 전환시 실행할 스레드를 선택하기 위해서 해당 "cache"에서 가져오기만 하면 된다.
     * "cache" 사용으로 문맥 전환시 시간 지연을 줄일 수 있지 않을까?
     * SMP 시스템에서 각각의 CPU 마다 스케쥴링이 이루어지기 때문에,
     * 글로벌한 커널 레디큐 "cache" 사용이 필요하지 않다.
     */
#ifndef CONFIG_SMP
	struct k_thread *thread = next_up();

	if (should_preempt(thread, preempt_ok)) {
#ifdef CONFIG_TIMESLICING
		if (thread != _current) {
			z_reset_time_slice();
		}
#endif
		update_metairq_preempt(thread);
		_kernel.ready_q.cache = thread;
	} else {
		_kernel.ready_q.cache = _current;
	}

#else
	/* The way this works is that the CPU record keeps its
	 * "cooperative swapping is OK" flag until the next reschedule
	 * call or context switch.  It doesn't need to be tracked per
	 * thread because if the thread gets preempted for whatever
	 * reason the scheduler will make the same decision anyway.
	 */
	_current_cpu->swap_ok = preempt_ok;
#endif
}

static bool thread_active_elsewhere(struct k_thread *thread)
{
	/* True if the thread is currently running on another CPU.
	 * There are more scalable designs to answer this question in
	 * constant time, but this is fine for now.
	 */
#ifdef CONFIG_SMP
	int currcpu = _current_cpu->id;

	for (int i = 0; i < CONFIG_MP_NUM_CPUS; i++) {
		if ((i != currcpu) &&
		    (_kernel.cpus[i].current == thread)) {
			return true;
		}
	}
#endif
	return false;
}

static void ready_thread(struct k_thread *thread)
{
#ifdef CONFIG_KERNEL_COHERENCE
	__ASSERT_NO_MSG(arch_mem_coherent(thread));
#endif

	/* If thread is queued already, do not try and added it to the
	 * run queue again
	 */
	if (!z_is_thread_queued(thread) && z_is_thread_ready(thread)) {
		SYS_PORT_TRACING_OBJ_FUNC(k_thread, sched_ready, thread);

		queue_thread(&_kernel.ready_q.runq, thread);
		update_cache(0);
#if defined(CONFIG_SMP) &&  defined(CONFIG_SCHED_IPI_SUPPORTED)
		arch_sched_ipi();
#endif
	}
}

void z_ready_thread(struct k_thread *thread)
{
	LOCKED(&sched_spinlock) {
		if (!thread_active_elsewhere(thread)) {
			ready_thread(thread);
		}
	}
}

void z_move_thread_to_end_of_prio_q(struct k_thread *thread)
{
	LOCKED(&sched_spinlock) {
		move_thread_to_end_of_prio_q(thread);
	}
}

void z_sched_start(struct k_thread *thread)
{
	k_spinlock_key_t key = k_spin_lock(&sched_spinlock);

	if (z_has_thread_started(thread)) {
		k_spin_unlock(&sched_spinlock, key);
		return;
	}

	z_mark_thread_as_started(thread);
	ready_thread(thread);
	z_reschedule(&sched_spinlock, key);
}

void z_impl_k_thread_suspend(struct k_thread *thread)
{
	SYS_PORT_TRACING_OBJ_FUNC_ENTER(k_thread, suspend, thread);

	(void)z_abort_thread_timeout(thread);

	LOCKED(&sched_spinlock) {
		if (z_is_thread_queued(thread)) {
			dequeue_thread(&_kernel.ready_q.runq, thread);
		}
		z_mark_thread_as_suspended(thread);
		update_cache(thread == _current);
	}

	if (thread == _current) {
		z_reschedule_unlocked();
	}

	SYS_PORT_TRACING_OBJ_FUNC_EXIT(k_thread, suspend, thread);
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_k_thread_suspend(struct k_thread *thread)
{
	Z_OOPS(Z_SYSCALL_OBJ(thread, K_OBJ_THREAD));
	z_impl_k_thread_suspend(thread);
}
#include <syscalls/k_thread_suspend_mrsh.c>
#endif

void z_impl_k_thread_resume(struct k_thread *thread)
{
	SYS_PORT_TRACING_OBJ_FUNC_ENTER(k_thread, resume, thread);

	k_spinlock_key_t key = k_spin_lock(&sched_spinlock);

	/* Do not try to resume a thread that was not suspended */
	if (!z_is_thread_suspended(thread)) {
		k_spin_unlock(&sched_spinlock, key);
		return;
	}

	z_mark_thread_as_not_suspended(thread);
	ready_thread(thread);

	z_reschedule(&sched_spinlock, key);

	SYS_PORT_TRACING_OBJ_FUNC_EXIT(k_thread, resume, thread);
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_k_thread_resume(struct k_thread *thread)
{
	Z_OOPS(Z_SYSCALL_OBJ(thread, K_OBJ_THREAD));
	z_impl_k_thread_resume(thread);
}
#include <syscalls/k_thread_resume_mrsh.c>
#endif

static _wait_q_t *pended_on_thread(struct k_thread *thread)
{
	__ASSERT_NO_MSG(thread->base.pended_on);

	return thread->base.pended_on;
}

static void unready_thread(struct k_thread *thread)
{
	if (z_is_thread_queued(thread)) {
		dequeue_thread(&_kernel.ready_q.runq, thread);
	}
	update_cache(thread == _current);
}

/* sched_spinlock must be held */
static void add_to_waitq_locked(struct k_thread *thread, _wait_q_t *wait_q)
{
	unready_thread(thread);
	z_mark_thread_as_pending(thread);

	SYS_PORT_TRACING_FUNC(k_thread, sched_pend, thread);

	if (wait_q != NULL) {
		thread->base.pended_on = wait_q;
		z_priq_wait_add(&wait_q->waitq, thread);
	}
}

static void add_thread_timeout(struct k_thread *thread, k_timeout_t timeout)
{
	if (!K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		z_add_thread_timeout(thread, timeout);
	}
}

static void pend(struct k_thread *thread, _wait_q_t *wait_q,
		 k_timeout_t timeout)
{
#ifdef CONFIG_KERNEL_COHERENCE
	__ASSERT_NO_MSG(wait_q == NULL || arch_mem_coherent(wait_q));
#endif

	LOCKED(&sched_spinlock) {
		add_to_waitq_locked(thread, wait_q);
	}

	add_thread_timeout(thread, timeout);
}

void z_pend_thread(struct k_thread *thread, _wait_q_t *wait_q,
		   k_timeout_t timeout)
{
	__ASSERT_NO_MSG(thread == _current || is_thread_dummy(thread));
	pend(thread, wait_q, timeout);
}

static inline void unpend_thread_no_timeout(struct k_thread *thread)
{
	_priq_wait_remove(&pended_on_thread(thread)->waitq, thread);
	z_mark_thread_as_not_pending(thread);
	thread->base.pended_on = NULL;
}

ALWAYS_INLINE void z_unpend_thread_no_timeout(struct k_thread *thread)
{
	LOCKED(&sched_spinlock) {
		unpend_thread_no_timeout(thread);
	}
}

#ifdef CONFIG_SYS_CLOCK_EXISTS
/* Timeout handler for *_thread_timeout() APIs */
void z_thread_timeout(struct _timeout *timeout)
{
	struct k_thread *thread = CONTAINER_OF(timeout,
					       struct k_thread, base.timeout);

	LOCKED(&sched_spinlock) {
		bool killed = ((thread->base.thread_state & _THREAD_DEAD) ||
			       (thread->base.thread_state & _THREAD_ABORTING));

		if (!killed) {
			if (thread->base.pended_on != NULL) {
				unpend_thread_no_timeout(thread);
			}
			z_mark_thread_as_started(thread);
			z_mark_thread_as_not_suspended(thread);
			ready_thread(thread);
		}
	}
}
#endif

int z_pend_curr_irqlock(uint32_t key, _wait_q_t *wait_q, k_timeout_t timeout)
{
	pend(_current, wait_q, timeout);

#if defined(CONFIG_TIMESLICING) && defined(CONFIG_SWAP_NONATOMIC)
	pending_current = _current;

	int ret = z_swap_irqlock(key);
	LOCKED(&sched_spinlock) {
		if (pending_current == _current) {
			pending_current = NULL;
		}
	}
	return ret;
#else
	return z_swap_irqlock(key);
#endif
}

int z_pend_curr(struct k_spinlock *lock, k_spinlock_key_t key,
	       _wait_q_t *wait_q, k_timeout_t timeout)
{
#if defined(CONFIG_TIMESLICING) && defined(CONFIG_SWAP_NONATOMIC)
	pending_current = _current;
#endif
	pend(_current, wait_q, timeout);
	return z_swap(lock, key);
}

struct k_thread *z_unpend1_no_timeout(_wait_q_t *wait_q)
{
	struct k_thread *thread = NULL;

	LOCKED(&sched_spinlock) {
		thread = _priq_wait_best(&wait_q->waitq);

		if (thread != NULL) {
			unpend_thread_no_timeout(thread);
		}
	}

	return thread;
}

struct k_thread *z_unpend_first_thread(_wait_q_t *wait_q)
{
	struct k_thread *thread = NULL;

	LOCKED(&sched_spinlock) {
		thread = _priq_wait_best(&wait_q->waitq);

		if (thread != NULL) {
			unpend_thread_no_timeout(thread);
			(void)z_abort_thread_timeout(thread);
		}
	}

	return thread;
}

void z_unpend_thread(struct k_thread *thread)
{
	z_unpend_thread_no_timeout(thread);
	(void)z_abort_thread_timeout(thread);
}

/* Priority set utility that does no rescheduling, it just changes the
 * run queue state, returning true if a reschedule is needed later.
 */
bool z_set_prio(struct k_thread *thread, int prio)
{
	bool need_sched = 0;

	LOCKED(&sched_spinlock) {
		need_sched = z_is_thread_ready(thread);

		if (need_sched) {
			/* Don't requeue on SMP if it's the running thread */
			if (!IS_ENABLED(CONFIG_SMP) || z_is_thread_queued(thread)) {
				dequeue_thread(&_kernel.ready_q.runq, thread);
				thread->base.prio = prio;
				queue_thread(&_kernel.ready_q.runq, thread);
			} else {
				thread->base.prio = prio;
			}
			update_cache(1);
		} else {
			thread->base.prio = prio;
		}
	}

	SYS_PORT_TRACING_OBJ_FUNC(k_thread, sched_priority_set, thread, prio);

	return need_sched;
}

void z_thread_priority_set(struct k_thread *thread, int prio)
{
	bool need_sched = z_set_prio(thread, prio);

#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_IPI_SUPPORTED)
	arch_sched_ipi();
#endif

	if (need_sched && _current->base.sched_locked == 0U) {
		z_reschedule_unlocked();
	}
}

static inline bool resched(uint32_t key)
{
#ifdef CONFIG_SMP
	_current_cpu->swap_ok = 0;
#endif

	return arch_irq_unlocked(key) && !arch_is_in_isr();
}

/*
 * Check if the next ready thread is the same as the current thread
 * and save the trip if true.
 */
static inline bool need_swap(void)
{
	/* the SMP case will be handled in C based z_swap() */
#ifdef CONFIG_SMP
	return true;
#else
	struct k_thread *new_thread;

	/* Check if the next ready thread is the same as the current thread */
	new_thread = _kernel.ready_q.cache;
	return new_thread != _current;
#endif
}

void z_reschedule(struct k_spinlock *lock, k_spinlock_key_t key)
{
	if (resched(key.key) && need_swap()) {
		z_swap(lock, key);
	} else {
		k_spin_unlock(lock, key);
	}
}

void z_reschedule_irqlock(uint32_t key)
{
	if (resched(key)) {
		z_swap_irqlock(key);
	} else {
		irq_unlock(key);
	}
}

void k_sched_lock(void)
{
	LOCKED(&sched_spinlock) {
		SYS_PORT_TRACING_FUNC(k_thread, sched_lock);

		z_sched_lock();
	}
}

void k_sched_unlock(void)
{
	LOCKED(&sched_spinlock) {
		__ASSERT(_current->base.sched_locked != 0U, "");
		__ASSERT(!arch_is_in_isr(), "");

		++_current->base.sched_locked;
		update_cache(0);
	}

	LOG_DBG("scheduler unlocked (%p:%d)",
		_current, _current->base.sched_locked);

	SYS_PORT_TRACING_FUNC(k_thread, sched_unlock);

	z_reschedule_unlocked();
}

struct k_thread *z_swap_next_thread(void)
{
#ifdef CONFIG_SMP
	return next_up();
#else
	return _kernel.ready_q.cache;
#endif
}

/* Just a wrapper around _current = xxx with tracing */
static inline void set_current(struct k_thread *new_thread)
{
	z_thread_mark_switched_out();
	_current_cpu->current = new_thread;
}

#ifdef CONFIG_USE_SWITCH
void *z_get_next_switch_handle(void *interrupted)
{
	z_check_stack_sentinel();

#ifdef CONFIG_SMP
	void *ret = NULL;

	LOCKED(&sched_spinlock) {
		struct k_thread *old_thread = _current, *new_thread;

		if (IS_ENABLED(CONFIG_SMP)) {
			old_thread->switch_handle = NULL;
		}
		new_thread = next_up();

		if (old_thread != new_thread) {
			update_metairq_preempt(new_thread);
			wait_for_switch(new_thread);
			arch_cohere_stacks(old_thread, interrupted, new_thread);

#ifdef CONFIG_TIMESLICING
			z_reset_time_slice();
#endif
			_current_cpu->swap_ok = 0;
			set_current(new_thread);

#ifdef CONFIG_SPIN_VALIDATE
			/* Changed _current!  Update the spinlock
			 * bookkeeping so the validation doesn't get
			 * confused when the "wrong" thread tries to
			 * release the lock.
			 */
			z_spin_lock_set_owner(&sched_spinlock);
#endif

			/* A queued (runnable) old/current thread
			 * needs to be added back to the run queue
			 * here, and atomically with its switch handle
			 * being set below.  This is safe now, as we
			 * will not return into it.
			 */
			if (z_is_thread_queued(old_thread)) {
				_priq_run_add(&_kernel.ready_q.runq,
					      old_thread);
			}
		}
		old_thread->switch_handle = interrupted;
		ret = new_thread->switch_handle;
		if (IS_ENABLED(CONFIG_SMP)) {
			/* Active threads MUST have a null here */
			new_thread->switch_handle = NULL;
		}
	}
	return ret;
#else
	_current->switch_handle = interrupted;
	set_current(_kernel.ready_q.cache);
	return _current->switch_handle;
#endif
}
#endif

ALWAYS_INLINE void z_priq_dumb_add(sys_dlist_t *pq, struct k_thread *thread)
{
	struct k_thread *t;

	__ASSERT_NO_MSG(!z_is_idle_thread_object(thread));

	SYS_DLIST_FOR_EACH_CONTAINER(pq, t, base.qnode_dlist) {
		if (z_sched_prio_cmp(thread, t) > 0) {
			sys_dlist_insert(&t->base.qnode_dlist,
					 &thread->base.qnode_dlist);
			return;
		}
	}

	sys_dlist_append(pq, &thread->base.qnode_dlist);
}

void z_priq_dumb_remove(sys_dlist_t *pq, struct k_thread *thread)
{
	__ASSERT_NO_MSG(!z_is_idle_thread_object(thread));

	sys_dlist_remove(&thread->base.qnode_dlist);
}

struct k_thread *z_priq_dumb_best(sys_dlist_t *pq)
{
	struct k_thread *thread = NULL;
	sys_dnode_t *n = sys_dlist_peek_head(pq);

	if (n != NULL) {
		thread = CONTAINER_OF(n, struct k_thread, base.qnode_dlist);
	}
	return thread;
}

bool z_priq_rb_lessthan(struct rbnode *a, struct rbnode *b)
{
	struct k_thread *thread_a, *thread_b;
	int32_t cmp;

	thread_a = CONTAINER_OF(a, struct k_thread, base.qnode_rb);
	thread_b = CONTAINER_OF(b, struct k_thread, base.qnode_rb);

	cmp = z_sched_prio_cmp(thread_a, thread_b);

	if (cmp > 0) {
		return true;
	} else if (cmp < 0) {
		return false;
	} else {
		return thread_a->base.order_key < thread_b->base.order_key
			? 1 : 0;
	}
}

void z_priq_rb_add(struct _priq_rb *pq, struct k_thread *thread)
{
	struct k_thread *t;

	__ASSERT_NO_MSG(!z_is_idle_thread_object(thread));

	thread->base.order_key = pq->next_order_key++;

	/* Renumber at wraparound.  This is tiny code, and in practice
	 * will almost never be hit on real systems.  BUT on very
	 * long-running systems where a priq never completely empties
	 * AND that contains very large numbers of threads, it can be
	 * a latency glitch to loop over all the threads like this.
	 */
	if (!pq->next_order_key) {
		RB_FOR_EACH_CONTAINER(&pq->tree, t, base.qnode_rb) {
			t->base.order_key = pq->next_order_key++;
		}
	}

	rb_insert(&pq->tree, &thread->base.qnode_rb);
}

void z_priq_rb_remove(struct _priq_rb *pq, struct k_thread *thread)
{
	__ASSERT_NO_MSG(!z_is_idle_thread_object(thread));

	rb_remove(&pq->tree, &thread->base.qnode_rb);

	if (!pq->tree.root) {
		pq->next_order_key = 0;
	}
}

struct k_thread *z_priq_rb_best(struct _priq_rb *pq)
{
	struct k_thread *thread = NULL;
	struct rbnode *n = rb_get_min(&pq->tree);

	if (n != NULL) {
		thread = CONTAINER_OF(n, struct k_thread, base.qnode_rb);
	}
	return thread;
}

#ifdef CONFIG_SCHED_MULTIQ
# if (K_LOWEST_THREAD_PRIO - K_HIGHEST_THREAD_PRIO) > 31
# error Too many priorities for multiqueue scheduler (max 32)
# endif
#endif

ALWAYS_INLINE void z_priq_mq_add(struct _priq_mq *pq, struct k_thread *thread)
{
	int priority_bit = thread->base.prio - K_HIGHEST_THREAD_PRIO;

	sys_dlist_append(&pq->queues[priority_bit], &thread->base.qnode_dlist);
	pq->bitmask |= BIT(priority_bit);
}

ALWAYS_INLINE void z_priq_mq_remove(struct _priq_mq *pq, struct k_thread *thread)
{
	int priority_bit = thread->base.prio - K_HIGHEST_THREAD_PRIO;

	sys_dlist_remove(&thread->base.qnode_dlist);
	if (sys_dlist_is_empty(&pq->queues[priority_bit])) {
		pq->bitmask &= ~BIT(priority_bit);
	}
}

struct k_thread *z_priq_mq_best(struct _priq_mq *pq)
{
	if (!pq->bitmask) {
		return NULL;
	}

	struct k_thread *thread = NULL;
	sys_dlist_t *l = &pq->queues[__builtin_ctz(pq->bitmask)];
	sys_dnode_t *n = sys_dlist_peek_head(l);

	if (n != NULL) {
		thread = CONTAINER_OF(n, struct k_thread, base.qnode_dlist);
	}
	return thread;
}

int z_unpend_all(_wait_q_t *wait_q)
{
	int need_sched = 0;
	struct k_thread *thread;

	while ((thread = z_waitq_head(wait_q)) != NULL) {
		z_unpend_thread(thread);
		z_ready_thread(thread);
		need_sched = 1;
	}

	return need_sched;
}

void z_sched_init(void)
{
#ifdef CONFIG_SCHED_DUMB
	sys_dlist_init(&_kernel.ready_q.runq);
#endif

#ifdef CONFIG_SCHED_SCALABLE
	_kernel.ready_q.runq = (struct _priq_rb) {
		.tree = {
			.lessthan_fn = z_priq_rb_lessthan,
		}
	};
#endif

#ifdef CONFIG_SCHED_MULTIQ
	for (int i = 0; i < ARRAY_SIZE(_kernel.ready_q.runq.queues); i++) {
		sys_dlist_init(&_kernel.ready_q.runq.queues[i]);
	}
#endif

#ifdef CONFIG_TIMESLICING
	/* LS: 동일 우선순위의 스레들이 서로 선점할 수 있도록 time slice를 설정.
	 * TIMESLICE_PRIORITY 보다 높은 우선순위의 스레드에는 적용되지 않음. 즉,
	 * cooperative 함.
	 *
	 * systick 핸들러에서 elapsed 된 시간만큼 time slice를 줄임. 0 이된
	 * 경우 스케쥴링을 요청하고 새 스레드를 위해 다시 time slice 를 설정.
	 * sys_clock_isr -> sys_clock_announce -> z_time_slice */
	k_sched_time_slice_set(CONFIG_TIMESLICE_SIZE,
		CONFIG_TIMESLICE_PRIORITY);
#endif
}

int z_impl_k_thread_priority_get(k_tid_t thread)
{
	return thread->base.prio;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_k_thread_priority_get(k_tid_t thread)
{
	Z_OOPS(Z_SYSCALL_OBJ(thread, K_OBJ_THREAD));
	return z_impl_k_thread_priority_get(thread);
}
#include <syscalls/k_thread_priority_get_mrsh.c>
#endif

void z_impl_k_thread_priority_set(k_tid_t thread, int prio)
{
	/*
	 * Use NULL, since we cannot know what the entry point is (we do not
	 * keep track of it) and idle cannot change its priority.
	 */
	Z_ASSERT_VALID_PRIO(prio, NULL);
	__ASSERT(!arch_is_in_isr(), "");

	struct k_thread *th = (struct k_thread *)thread;

	z_thread_priority_set(th, prio);
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_k_thread_priority_set(k_tid_t thread, int prio)
{
	Z_OOPS(Z_SYSCALL_OBJ(thread, K_OBJ_THREAD));
	Z_OOPS(Z_SYSCALL_VERIFY_MSG(_is_valid_prio(prio, NULL),
				    "invalid thread priority %d", prio));
	Z_OOPS(Z_SYSCALL_VERIFY_MSG((int8_t)prio >= thread->base.prio,
				    "thread priority may only be downgraded (%d < %d)",
				    prio, thread->base.prio));

	z_impl_k_thread_priority_set(thread, prio);
}
#include <syscalls/k_thread_priority_set_mrsh.c>
#endif

#ifdef CONFIG_SCHED_DEADLINE
void z_impl_k_thread_deadline_set(k_tid_t tid, int deadline)
{
	struct k_thread *thread = tid;

	LOCKED(&sched_spinlock) {
		thread->base.prio_deadline = k_cycle_get_32() + deadline;
		if (z_is_thread_queued(thread)) {
			dequeue_thread(&_kernel.ready_q.runq, thread);
			queue_thread(&_kernel.ready_q.runq, thread);
		}
	}
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_k_thread_deadline_set(k_tid_t tid, int deadline)
{
	struct k_thread *thread = tid;

	Z_OOPS(Z_SYSCALL_OBJ(thread, K_OBJ_THREAD));
	Z_OOPS(Z_SYSCALL_VERIFY_MSG(deadline > 0,
				    "invalid thread deadline %d",
				    (int)deadline));

	z_impl_k_thread_deadline_set((k_tid_t)thread, deadline);
}
#include <syscalls/k_thread_deadline_set_mrsh.c>
#endif
#endif

void z_impl_k_yield(void)
{
	__ASSERT(!arch_is_in_isr(), "");

	SYS_PORT_TRACING_FUNC(k_thread, yield);

	k_spinlock_key_t key = k_spin_lock(&sched_spinlock);

	if (!IS_ENABLED(CONFIG_SMP) ||
	    z_is_thread_queued(_current)) {
		dequeue_thread(&_kernel.ready_q.runq,
			       _current);
	}
	queue_thread(&_kernel.ready_q.runq, _current);
	update_cache(1);
	z_swap(&sched_spinlock, key);
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_k_yield(void)
{
	z_impl_k_yield();
}
#include <syscalls/k_yield_mrsh.c>
#endif

static int32_t z_tick_sleep(k_ticks_t ticks)
{
#ifdef CONFIG_MULTITHREADING
	uint32_t expected_wakeup_ticks;

	__ASSERT(!arch_is_in_isr(), "");

#ifndef CONFIG_TIMEOUT_64BIT
	/* LOG subsys does not handle 64-bit values
	 * https://github.com/zephyrproject-rtos/zephyr/issues/26246
	 */
	LOG_DBG("thread %p for %u ticks", _current, ticks);
#endif

	/* wait of 0 ms is treated as a 'yield' */
	if (ticks == 0) {
		k_yield();
		return 0;
	}

	k_timeout_t timeout = Z_TIMEOUT_TICKS(ticks);
	if (Z_TICK_ABS(ticks) <= 0) {
		expected_wakeup_ticks = ticks + sys_clock_tick_get_32();
	} else {
		expected_wakeup_ticks = Z_TICK_ABS(ticks);
	}

	k_spinlock_key_t key = k_spin_lock(&sched_spinlock);

#if defined(CONFIG_TIMESLICING) && defined(CONFIG_SWAP_NONATOMIC)
	pending_current = _current;
#endif
	unready_thread(_current);
	z_add_thread_timeout(_current, timeout);
	z_mark_thread_as_suspended(_current);

	(void)z_swap(&sched_spinlock, key);

	__ASSERT(!z_is_thread_state_set(_current, _THREAD_SUSPENDED), "");

	ticks = (k_ticks_t)expected_wakeup_ticks - sys_clock_tick_get_32();
	if (ticks > 0) {
		return ticks;
	}
#endif

	return 0;
}

int32_t z_impl_k_sleep(k_timeout_t timeout)
{
	k_ticks_t ticks;

	__ASSERT(!arch_is_in_isr(), "");

	SYS_PORT_TRACING_FUNC_ENTER(k_thread, sleep, timeout);

	/* in case of K_FOREVER, we suspend */
	if (K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		k_thread_suspend(_current);

		SYS_PORT_TRACING_FUNC_EXIT(k_thread, sleep, timeout, (int32_t) K_TICKS_FOREVER);

		return (int32_t) K_TICKS_FOREVER;
	}

	ticks = timeout.ticks;

	ticks = z_tick_sleep(ticks);

	int32_t ret = k_ticks_to_ms_floor64(ticks);

	SYS_PORT_TRACING_FUNC_EXIT(k_thread, sleep, timeout, ret);

	return ret;
}

#ifdef CONFIG_USERSPACE
static inline int32_t z_vrfy_k_sleep(k_timeout_t timeout)
{
	return z_impl_k_sleep(timeout);
}
#include <syscalls/k_sleep_mrsh.c>
#endif

int32_t z_impl_k_usleep(int us)
{
	int32_t ticks;

	SYS_PORT_TRACING_FUNC_ENTER(k_thread, usleep, us);

	ticks = k_us_to_ticks_ceil64(us);
	ticks = z_tick_sleep(ticks);

	SYS_PORT_TRACING_FUNC_EXIT(k_thread, usleep, us, k_ticks_to_us_floor64(ticks));

	return k_ticks_to_us_floor64(ticks);
}

#ifdef CONFIG_USERSPACE
static inline int32_t z_vrfy_k_usleep(int us)
{
	return z_impl_k_usleep(us);
}
#include <syscalls/k_usleep_mrsh.c>
#endif

void z_impl_k_wakeup(k_tid_t thread)
{
	SYS_PORT_TRACING_OBJ_FUNC(k_thread, wakeup, thread);

	if (z_is_thread_pending(thread)) {
		return;
	}

	if (z_abort_thread_timeout(thread) < 0) {
		/* Might have just been sleeping forever */
		if (thread->base.thread_state != _THREAD_SUSPENDED) {
			return;
		}
	}

	z_mark_thread_as_not_suspended(thread);
	z_ready_thread(thread);

#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_IPI_SUPPORTED)
	arch_sched_ipi();
#endif

	if (!arch_is_in_isr()) {
		z_reschedule_unlocked();
	}
}

#ifdef CONFIG_TRACE_SCHED_IPI
extern void z_trace_sched_ipi(void);
#endif

#ifdef CONFIG_SMP
void z_sched_ipi(void)
{
	/* NOTE: When adding code to this, make sure this is called
	 * at appropriate location when !CONFIG_SCHED_IPI_SUPPORTED.
	 */
#ifdef CONFIG_TRACE_SCHED_IPI
	z_trace_sched_ipi();
#endif
}
#endif

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_k_wakeup(k_tid_t thread)
{
	Z_OOPS(Z_SYSCALL_OBJ(thread, K_OBJ_THREAD));
	z_impl_k_wakeup(thread);
}
#include <syscalls/k_wakeup_mrsh.c>
#endif

k_tid_t z_impl_z_current_get(void)
{
#ifdef CONFIG_SMP
	/* In SMP, _current is a field read from _current_cpu, which
	 * can race with preemption before it is read.  We must lock
	 * local interrupts when reading it.
	 */
	unsigned int k = arch_irq_lock();
#endif

	k_tid_t ret = _current_cpu->current;

#ifdef CONFIG_SMP
	arch_irq_unlock(k);
#endif
	return ret;
}

#ifdef CONFIG_USERSPACE
static inline k_tid_t z_vrfy_z_current_get(void)
{
	return z_impl_z_current_get();
}
#include <syscalls/z_current_get_mrsh.c>
#endif

int z_impl_k_is_preempt_thread(void)
{
	return !arch_is_in_isr() && is_preempt(_current);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_k_is_preempt_thread(void)
{
	return z_impl_k_is_preempt_thread();
}
#include <syscalls/k_is_preempt_thread_mrsh.c>
#endif

#ifdef CONFIG_SCHED_CPU_MASK
# ifdef CONFIG_SMP
/* Right now we use a single byte for this mask */
BUILD_ASSERT(CONFIG_MP_NUM_CPUS <= 8, "Too many CPUs for mask word");
# endif


static int cpu_mask_mod(k_tid_t thread, uint32_t enable_mask, uint32_t disable_mask)
{
	int ret = 0;

	LOCKED(&sched_spinlock) {
		if (z_is_thread_prevented_from_running(thread)) {
			thread->base.cpu_mask |= enable_mask;
			thread->base.cpu_mask  &= ~disable_mask;
		} else {
			ret = -EINVAL;
		}
	}
	return ret;
}

int k_thread_cpu_mask_clear(k_tid_t thread)
{
	return cpu_mask_mod(thread, 0, 0xffffffff);
}

int k_thread_cpu_mask_enable_all(k_tid_t thread)
{
	return cpu_mask_mod(thread, 0xffffffff, 0);
}

int k_thread_cpu_mask_enable(k_tid_t thread, int cpu)
{
	return cpu_mask_mod(thread, BIT(cpu), 0);
}

int k_thread_cpu_mask_disable(k_tid_t thread, int cpu)
{
	return cpu_mask_mod(thread, 0, BIT(cpu));
}

#endif /* CONFIG_SCHED_CPU_MASK */

static inline void unpend_all(_wait_q_t *wait_q)
{
	struct k_thread *thread;

	while ((thread = z_waitq_head(wait_q)) != NULL) {
		unpend_thread_no_timeout(thread);
		(void)z_abort_thread_timeout(thread);
		arch_thread_return_value_set(thread, 0);
		ready_thread(thread);
	}
}

static void end_thread(struct k_thread *thread)
{
	/* We hold the lock, and the thread is known not to be running
	 * anywhere.
	 */
	if ((thread->base.thread_state & _THREAD_DEAD) == 0U) {
		thread->base.thread_state |= _THREAD_DEAD;
		thread->base.thread_state &= ~_THREAD_ABORTING;
		if (z_is_thread_queued(thread)) {
			dequeue_thread(&_kernel.ready_q.runq, thread);
		}
		if (thread->base.pended_on != NULL) {
			unpend_thread_no_timeout(thread);
		}
		(void)z_abort_thread_timeout(thread);
		unpend_all(&thread->join_queue);
		update_cache(1);

		SYS_PORT_TRACING_FUNC(k_thread, sched_abort, thread);

		z_thread_monitor_exit(thread);

#ifdef CONFIG_USERSPACE
		z_mem_domain_exit_thread(thread);
		z_thread_perms_all_clear(thread);
		z_object_uninit(thread->stack_obj);
		z_object_uninit(thread);
#endif
	}
}

void z_thread_abort(struct k_thread *thread)
{
	k_spinlock_key_t key = k_spin_lock(&sched_spinlock);

	if ((thread->base.thread_state & _THREAD_DEAD) != 0U) {
		k_spin_unlock(&sched_spinlock, key);
		return;
	}

#ifdef CONFIG_SMP
	if (is_aborting(thread) && thread == _current && arch_is_in_isr()) {
		/* Another CPU is spinning for us, don't deadlock */
		end_thread(thread);
	}

	bool active = thread_active_elsewhere(thread);

	if (active) {
		/* It's running somewhere else, flag and poke */
		thread->base.thread_state |= _THREAD_ABORTING;

#ifdef CONFIG_SCHED_IPI_SUPPORTED
		arch_sched_ipi();
#endif
	}

	if (is_aborting(thread) && thread != _current) {
		if (arch_is_in_isr()) {
			/* ISRs can only spin waiting another CPU */
			k_spin_unlock(&sched_spinlock, key);
			while (is_aborting(thread)) {
			}
		} else if (active) {
			/* Threads can join */
			add_to_waitq_locked(_current, &thread->join_queue);
			z_swap(&sched_spinlock, key);
		}
		return; /* lock has been released */
	}
#endif
	end_thread(thread);
	if (thread == _current && !arch_is_in_isr()) {
		z_swap(&sched_spinlock, key);
		__ASSERT(false, "aborted _current back from dead");
	}
	k_spin_unlock(&sched_spinlock, key);
}

#if !defined(CONFIG_ARCH_HAS_THREAD_ABORT)
void z_impl_k_thread_abort(struct k_thread *thread)
{
	SYS_PORT_TRACING_OBJ_FUNC_ENTER(k_thread, abort, thread);

	z_thread_abort(thread);

	SYS_PORT_TRACING_OBJ_FUNC_EXIT(k_thread, abort, thread);
}
#endif

int z_impl_k_thread_join(struct k_thread *thread, k_timeout_t timeout)
{
	k_spinlock_key_t key = k_spin_lock(&sched_spinlock);
	int ret = 0;

	SYS_PORT_TRACING_OBJ_FUNC_ENTER(k_thread, join, thread, timeout);

	if ((thread->base.thread_state & _THREAD_DEAD) != 0U) {
		ret = 0;
	} else if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		ret = -EBUSY;
	} else if ((thread == _current) ||
		   (thread->base.pended_on == &_current->join_queue)) {
		ret = -EDEADLK;
	} else {
		__ASSERT(!arch_is_in_isr(), "cannot join in ISR");
		add_to_waitq_locked(_current, &thread->join_queue);
		add_thread_timeout(_current, timeout);

		SYS_PORT_TRACING_OBJ_FUNC_BLOCKING(k_thread, join, thread, timeout);
		ret = z_swap(&sched_spinlock, key);
		SYS_PORT_TRACING_OBJ_FUNC_EXIT(k_thread, join, thread, timeout, ret);

		return ret;
	}

	SYS_PORT_TRACING_OBJ_FUNC_EXIT(k_thread, join, thread, timeout, ret);

	k_spin_unlock(&sched_spinlock, key);
	return ret;
}

#ifdef CONFIG_USERSPACE
/* Special case: don't oops if the thread is uninitialized.  This is because
 * the initialization bit does double-duty for thread objects; if false, means
 * the thread object is truly uninitialized, or the thread ran and exited for
 * some reason.
 *
 * Return true in this case indicating we should just do nothing and return
 * success to the caller.
 */
static bool thread_obj_validate(struct k_thread *thread)
{
	struct z_object *ko = z_object_find(thread);
	int ret = z_object_validate(ko, K_OBJ_THREAD, _OBJ_INIT_TRUE);

	switch (ret) {
	case 0:
		return false;
	case -EINVAL:
		return true;
	default:
#ifdef CONFIG_LOG
		z_dump_object_error(ret, thread, ko, K_OBJ_THREAD);
#endif
		Z_OOPS(Z_SYSCALL_VERIFY_MSG(ret, "access denied"));
	}
	CODE_UNREACHABLE; /* LCOV_EXCL_LINE */
}

static inline int z_vrfy_k_thread_join(struct k_thread *thread,
				       k_timeout_t timeout)
{
	if (thread_obj_validate(thread)) {
		return 0;
	}

	return z_impl_k_thread_join(thread, timeout);
}
#include <syscalls/k_thread_join_mrsh.c>

static inline void z_vrfy_k_thread_abort(k_tid_t thread)
{
	if (thread_obj_validate(thread)) {
		return;
	}

	Z_OOPS(Z_SYSCALL_VERIFY_MSG(!(thread->base.user_options & K_ESSENTIAL),
				    "aborting essential thread %p", thread));

	z_impl_k_thread_abort((struct k_thread *)thread);
}
#include <syscalls/k_thread_abort_mrsh.c>
#endif /* CONFIG_USERSPACE */

/*
 * future scheduler.h API implementations
 */
bool z_sched_wake(_wait_q_t *wait_q, int swap_retval, void *swap_data)
{
	struct k_thread *thread;
	bool ret = false;

	LOCKED(&sched_spinlock) {
		thread = _priq_wait_best(&wait_q->waitq);

		if (thread != NULL) {
			z_thread_return_value_set_with_data(thread,
							    swap_retval,
							    swap_data);
			unpend_thread_no_timeout(thread);
			(void)z_abort_thread_timeout(thread);
			ready_thread(thread);
			ret = true;
		}
	}

	return ret;
}

int z_sched_wait(struct k_spinlock *lock, k_spinlock_key_t key,
		 _wait_q_t *wait_q, k_timeout_t timeout, void **data)
{
	int ret = z_pend_curr(lock, key, wait_q, timeout);

	if (data != NULL) {
		*data = _current->base.swap_data;
	}
	return ret;
}
