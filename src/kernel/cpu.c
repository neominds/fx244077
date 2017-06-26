/* CPU control.
 * (C) 2001, 2002, 2003, 2004 Rusty Russell
 *
 * This code is licenced under the GPL.
 */
#include <linux/proc_fs.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/sched.h>
#include <linux/unistd.h>
#include <linux/cpu.h>
#include <linux/oom.h>
#include <linux/rcupdate.h>
#include <linux/export.h>
#include <linux/bug.h>
#include <linux/kthread.h>
#include <linux/stop_machine.h>
#include <linux/mutex.h>
#include <linux/gfp.h>
#include <linux/suspend.h>
#include <linux/lockdep.h>
#include <linux/tick.h>
#include <linux/irq.h>
#include <linux/smpboot.h>

#include <trace/events/power.h>
#define CREATE_TRACE_POINTS
#include <trace/events/cpuhp.h>

#include "smpboot.h"

/**
 * cpuhp_cpu_state - Per cpu hotplug state storage
 * @state:	The current cpu state
 * @target:	The target state
 * @thread:	Pointer to the hotplug thread
 * @should_run:	Thread should execute
 * @rollback:	Perform a rollback
 * @cb_stat:	The state for a single callback (install/uninstall)
 * @cb:		Single callback function (install/uninstall)
 * @result:	Result of the operation
 * @done:	Signal completion to the issuer of the task
 */
struct cpuhp_cpu_state {
	enum cpuhp_state	state;
	enum cpuhp_state	target;
#ifdef CONFIG_SMP
	struct task_struct	*thread;
	bool			should_run;
	bool			rollback;
	enum cpuhp_state	cb_state;
	int			(*cb)(unsigned int cpu);
	int			result;
	struct completion	done;
#endif
};

static DEFINE_PER_CPU(struct cpuhp_cpu_state, cpuhp_state);

/**
 * cpuhp_step - Hotplug state machine step
 * @name:	Name of the step
 * @startup:	Startup function of the step
 * @teardown:	Teardown function of the step
 * @skip_onerr:	Do not invoke the functions on error rollback
 *		Will go away once the notifiers	are gone
 * @cant_stop:	Bringup/teardown can't be stopped at this step
 */
struct cpuhp_step {
	const char	*name;
	int		(*startup)(unsigned int cpu);
	int		(*teardown)(unsigned int cpu);
	bool		skip_onerr;
	bool		cant_stop;
};

static DEFINE_MUTEX(cpuhp_state_mutex);
static struct cpuhp_step cpuhp_bp_states[];
static struct cpuhp_step cpuhp_ap_states[];

/**
 * cpuhp_invoke_callback _ Invoke the callbacks for a given state
 * @cpu:	The cpu for which the callback should be invoked
 * @step:	The step in the state machine
 * @cb:		The callback function to invoke
 *
 * Called from cpu hotplug and from the state register machinery
 */
static int cpuhp_invoke_callback(unsigned int cpu, enum cpuhp_state step,
				 int (*cb)(unsigned int))
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	int ret = 0;

	if (cb) {
		trace_cpuhp_enter(cpu, st->target, step, cb);
		ret = cb(cpu);
		trace_cpuhp_exit(cpu, st->state, step, ret);
	}
	return ret;
}

#ifdef CONFIG_SMP
/* Serializes the updates to cpu_online_mask, cpu_present_mask */
static DEFINE_MUTEX(cpu_add_remove_lock);
bool cpuhp_tasks_frozen;
EXPORT_SYMBOL_GPL(cpuhp_tasks_frozen);

/*
 * The following two APIs (cpu_maps_update_begin/done) must be used when
 * attempting to serialize the updates to cpu_online_mask & cpu_present_mask.
 * The APIs cpu_notifier_register_begin/done() must be used to protect CPU
 * hotplug callback (un)registration performed using __register_cpu_notifier()
 * or __unregister_cpu_notifier().
 */
void cpu_maps_update_begin(void)
{
	mutex_lock(&cpu_add_remove_lock);
}
EXPORT_SYMBOL(cpu_notifier_register_begin);

void cpu_maps_update_done(void)
{
	mutex_unlock(&cpu_add_remove_lock);
}
EXPORT_SYMBOL(cpu_notifier_register_done);

static RAW_NOTIFIER_HEAD(cpu_chain);

/* If set, cpu_up and cpu_down will return -EBUSY and do nothing.
 * Should always be manipulated under cpu_add_remove_lock
 */
static int cpu_hotplug_disabled;

#ifdef CONFIG_HOTPLUG_CPU

static struct {
	struct task_struct *active_writer;
	/* wait queue to wake up the active_writer */
	wait_queue_head_t wq;
	/* verifies that no writer will get active while readers are active */
	struct mutex lock;
	/*
	 * Also blocks the new readers during
	 * an ongoing cpu hotplug operation.
	 */
	atomic_t refcount;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map;
#endif
} cpu_hotplug = {
	.active_writer = NULL,
	.lock = __MUTEX_INITIALIZER(cpu_hotplug.lock),
	.wq = __WAIT_QUEUE_HEAD_INITIALIZER(cpu_hotplug.wq),
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	.dep_map = {.name = "cpu_hotplug.lock" },
#endif
};

/* Lockdep annotations for get/put_online_cpus() and cpu_hotplug_begin/end() */
#define cpuhp_lock_acquire_read() lock_map_acquire_read(&cpu_hotplug.dep_map)
#define cpuhp_lock_acquire_tryread() \
				  lock_map_acquire_tryread(&cpu_hotplug.dep_map)
#define cpuhp_lock_acquire()      lock_map_acquire(&cpu_hotplug.dep_map)
#define cpuhp_lock_release()      lock_map_release(&cpu_hotplug.dep_map)

/**
 * hotplug_pcp	- per cpu hotplug descriptor
 * @unplug:	set when pin_current_cpu() needs to sync tasks
 * @sync_tsk:	the task that waits for tasks to finish pinned sections
 * @refcount:	counter of tasks in pinned sections
 * @grab_lock:	set when the tasks entering pinned sections should wait
 * @synced:	notifier for @sync_tsk to tell cpu_down it's finished
 * @mutex:	the mutex to make tasks wait (used when @grab_lock is true)
 * @mutex_init:	zero if the mutex hasn't been initialized yet.
 *
 * Although @unplug and @sync_tsk may point to the same task, the @unplug
 * is used as a flag and still exists after @sync_tsk has exited and
 * @sync_tsk set to NULL.
 */
struct hotplug_pcp {
	struct task_struct *unplug;
	struct task_struct *sync_tsk;
	int refcount;
	int grab_lock;
	struct completion synced;
	struct completion unplug_wait;
#ifdef CONFIG_PREEMPT_RT_FULL
	/*
	 * Note, on PREEMPT_RT, the hotplug lock must save the state of
	 * the task, otherwise the mutex will cause the task to fail
	 * to sleep when required. (Because it's called from migrate_disable())
	 *
	 * The spinlock_t on PREEMPT_RT is a mutex that saves the task's
	 * state.
	 */
	spinlock_t lock;
#else
	struct mutex mutex;
#endif
	int mutex_init;
};

#ifdef CONFIG_PREEMPT_RT_FULL
# define hotplug_lock(hp) rt_spin_lock__no_mg(&(hp)->lock)
# define hotplug_unlock(hp) rt_spin_unlock__no_mg(&(hp)->lock)
#else
# define hotplug_lock(hp) mutex_lock(&(hp)->mutex)
# define hotplug_unlock(hp) mutex_unlock(&(hp)->mutex)
#endif

static DEFINE_PER_CPU(struct hotplug_pcp, hotplug_pcp);

/**
 * pin_current_cpu - Prevent the current cpu from being unplugged
 *
 * Lightweight version of get_online_cpus() to prevent cpu from being
 * unplugged when code runs in a migration disabled region.
 *
 * Must be called with preemption disabled (preempt_count = 1)!
 */
void pin_current_cpu(void)
{
	struct hotplug_pcp *hp;
	int force = 0;

retry:
	hp = this_cpu_ptr(&hotplug_pcp);

	if (!hp->unplug || hp->refcount || force || preempt_count() > 1 ||
	    hp->unplug == current) {
		hp->refcount++;
		return;
	}
	if (hp->grab_lock) {
		preempt_enable();
		hotplug_lock(hp);
		hotplug_unlock(hp);
	} else {
		preempt_enable();
		/*
		 * Try to push this task off of this CPU.
		 */
		if (!migrate_me()) {
			preempt_disable();
			hp = this_cpu_ptr(&hotplug_pcp);
			if (!hp->grab_lock) {
				/*
				 * Just let it continue it's already pinned
				 * or about to sleep.
				 */
				force = 1;
				goto retry;
			}
			preempt_enable();
		}
	}
	preempt_disable();
	goto retry;
}

/**
 * unpin_current_cpu - Allow unplug of current cpu
 *
 * Must be called with preemption or interrupts disabled!
 */
void unpin_current_cpu(void)
{
	struct hotplug_pcp *hp = this_cpu_ptr(&hotplug_pcp);

	WARN_ON(hp->refcount <= 0);

	/* This is safe. sync_unplug_thread is pinned to this cpu */
	if (!--hp->refcount && hp->unplug && hp->unplug != current)
		wake_up_process(hp->unplug);
}

static void wait_for_pinned_cpus(struct hotplug_pcp *hp)
{
	set_current_state(TASK_UNINTERRUPTIBLE);
	while (hp->refcount) {
		schedule_preempt_disabled();
		set_current_state(TASK_UNINTERRUPTIBLE);
	}
}

static int sync_unplug_thread(void *data)
{
	struct hotplug_pcp *hp = data;

	wait_for_completion(&hp->unplug_wait);
	preempt_disable();
	hp->unplug = current;
	wait_for_pinned_cpus(hp);

	/*
	 * This thread will synchronize the cpu_down() with threads
	 * that have pinned the CPU. When the pinned CPU count reaches
	 * zero, we inform the cpu_down code to continue to the next step.
	 */
	set_current_state(TASK_UNINTERRUPTIBLE);
	preempt_enable();
	complete(&hp->synced);

	/*
	 * If all succeeds, the next step will need tasks to wait till
	 * the CPU is offline before continuing. To do this, the grab_lock
	 * is set and tasks going into pin_current_cpu() will block on the
	 * mutex. But we still need to wait for those that are already in
	 * pinned CPU sections. If the cpu_down() failed, the kthread_should_stop()
	 * will kick this thread out.
	 */
	while (!hp->grab_lock && !kthread_should_stop()) {
		schedule();
		set_current_state(TASK_UNINTERRUPTIBLE);
	}

	/* Make sure grab_lock is seen before we see a stale completion */
	smp_mb();

	/*
	 * Now just before cpu_down() enters stop machine, we need to make
	 * sure all tasks that are in pinned CPU sections are out, and new
	 * tasks will now grab the lock, keeping them from entering pinned
	 * CPU sections.
	 */
	if (!kthread_should_stop()) {
		preempt_disable();
		wait_for_pinned_cpus(hp);
		preempt_enable();
		complete(&hp->synced);
	}

	set_current_state(TASK_UNINTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_UNINTERRUPTIBLE);
	}
	set_current_state(TASK_RUNNING);

	/*
	 * Force this thread off this CPU as it's going down and
	 * we don't want any more work on this CPU.
	 */
	current->flags &= ~PF_NO_SETAFFINITY;
	set_cpus_allowed_ptr(current, cpu_present_mask);
	migrate_me();
	return 0;
}

static void __cpu_unplug_sync(struct hotplug_pcp *hp)
{
	wake_up_process(hp->sync_tsk);
	wait_for_completion(&hp->synced);
}

static void __cpu_unplug_wait(unsigned int cpu)
{
	struct hotplug_pcp *hp = &per_cpu(hotplug_pcp, cpu);

	complete(&hp->unplug_wait);
	wait_for_completion(&hp->synced);
}

/*
 * Start the sync_unplug_thread on the target cpu and wait for it to
 * complete.
 */
static int cpu_unplug_begin(unsigned int cpu)
{
	struct hotplug_pcp *hp = &per_cpu(hotplug_pcp, cpu);
	int err;

	/* Protected by cpu_hotplug.lock */
	if (!hp->mutex_init) {
#ifdef CONFIG_PREEMPT_RT_FULL
		spin_lock_init(&hp->lock);
#else
		mutex_init(&hp->mutex);
#endif
		hp->mutex_init = 1;
	}

	/* Inform the scheduler to migrate tasks off this CPU */
	tell_sched_cpu_down_begin(cpu);

	init_completion(&hp->synced);
	init_completion(&hp->unplug_wait);

	hp->sync_tsk = kthread_create(sync_unplug_thread, hp, "sync_unplug/%d", cpu);
	if (IS_ERR(hp->sync_tsk)) {
		err = PTR_ERR(hp->sync_tsk);
		hp->sync_tsk = NULL;
		return err;
	}
	kthread_bind(hp->sync_tsk, cpu);

	/*
	 * Wait for tasks to get out of the pinned sections,
	 * it's still OK if new tasks enter. Some CPU notifiers will
	 * wait for tasks that are going to enter these sections and
	 * we must not have them block.
	 */
	wake_up_process(hp->sync_tsk);
	return 0;
}

static void cpu_unplug_sync(unsigned int cpu)
{
	struct hotplug_pcp *hp = &per_cpu(hotplug_pcp, cpu);

	init_completion(&hp->synced);
	/* The completion needs to be initialzied before setting grab_lock */
	smp_wmb();

	/* Grab the mutex before setting grab_lock */
	hotplug_lock(hp);
	hp->grab_lock = 1;

	/*
	 * The CPU notifiers have been completed.
	 * Wait for tasks to get out of pinned CPU sections and have new
	 * tasks block until the CPU is completely down.
	 */
	__cpu_unplug_sync(hp);

	/* All done with the sync thread */
	kthread_stop(hp->sync_tsk);
	hp->sync_tsk = NULL;
}

static void cpu_unplug_done(unsigned int cpu)
{
	struct hotplug_pcp *hp = &per_cpu(hotplug_pcp, cpu);

	hp->unplug = NULL;
	/* Let all tasks know cpu unplug is finished before cleaning up */
	smp_wmb();

	if (hp->sync_tsk)
		kthread_stop(hp->sync_tsk);

	if (hp->grab_lock) {
		hotplug_unlock(hp);
		/* protected by cpu_hotplug.lock */
		hp->grab_lock = 0;
	}
	tell_sched_cpu_down_done(cpu);
}

void get_online_cpus(void)
{
	might_sleep();
	if (cpu_hotplug.active_writer == current)
		return;
	cpuhp_lock_acquire_read();
	mutex_lock(&cpu_hotplug.lock);
	atomic_inc(&cpu_hotplug.refcount);
	mutex_unlock(&cpu_hotplug.lock);
}
EXPORT_SYMBOL_GPL(get_online_cpus);

void put_online_cpus(void)
{
	int refcount;

	if (cpu_hotplug.active_writer == current)
		return;

	refcount = atomic_dec_return(&cpu_hotplug.refcount);
	if (WARN_ON(refcount < 0)) /* try to fix things up */
		atomic_inc(&cpu_hotplug.refcount);

	if (refcount <= 0 && waitqueue_active(&cpu_hotplug.wq))
		wake_up(&cpu_hotplug.wq);

	cpuhp_lock_release();

}
EXPORT_SYMBOL_GPL(put_online_cpus);

/*
 * This ensures that the hotplug operation can begin only when the
 * refcount goes to zero.
 *
 * Note that during a cpu-hotplug operation, the new readers, if any,
 * will be blocked by the cpu_hotplug.lock
 *
 * Since cpu_hotplug_begin() is always called after invoking
 * cpu_maps_update_begin(), we can be sure that only one writer is active.
 *
 * Note that theoretically, there is a possibility of a livelock:
 * - Refcount goes to zero, last reader wakes up the sleeping
 *   writer.
 * - Last reader unlocks the cpu_hotplug.lock.
 * - A new reader arrives at this moment, bumps up the refcount.
 * - The writer acquires the cpu_hotplug.lock finds the refcount
 *   non zero and goes to sleep again.
 *
 * However, this is very difficult to achieve in practice since
 * get_online_cpus() not an api which is called all that often.
 *
 */
void cpu_hotplug_begin(void)
{
	DEFINE_WAIT(wait);

	cpu_hotplug.active_writer = current;
	cpuhp_lock_acquire();

	for (;;) {
		mutex_lock(&cpu_hotplug.lock);
		prepare_to_wait(&cpu_hotplug.wq, &wait, TASK_UNINTERRUPTIBLE);
		if (likely(!atomic_read(&cpu_hotplug.refcount)))
				break;
		mutex_unlock(&cpu_hotplug.lock);
		schedule();
	}
	finish_wait(&cpu_hotplug.wq, &wait);
}

void cpu_hotplug_done(void)
{
	cpu_hotplug.active_writer = NULL;
	mutex_unlock(&cpu_hotplug.lock);
	cpuhp_lock_release();
}

/*
 * Wait for currently running CPU hotplug operations to complete (if any) and
 * disable future CPU hotplug (from sysfs). The 'cpu_add_remove_lock' protects
 * the 'cpu_hotplug_disabled' flag. The same lock is also acquired by the
 * hotplug path before performing hotplug operations. So acquiring that lock
 * guarantees mutual exclusion from any currently running hotplug operations.
 */
void cpu_hotplug_disable(void)
{
	cpu_maps_update_begin();
	cpu_hotplug_disabled++;
	cpu_maps_update_done();
}
EXPORT_SYMBOL_GPL(cpu_hotplug_disable);

void cpu_hotplug_enable(void)
{
	cpu_maps_update_begin();
	WARN_ON(--cpu_hotplug_disabled < 0);
	cpu_maps_update_done();
}
EXPORT_SYMBOL_GPL(cpu_hotplug_enable);
#endif	/* CONFIG_HOTPLUG_CPU */

/* Need to know about CPUs going up/down? */
int register_cpu_notifier(struct notifier_block *nb)
{
	int ret;
	cpu_maps_update_begin();
	ret = raw_notifier_chain_register(&cpu_chain, nb);
	cpu_maps_update_done();
	return ret;
}

int __register_cpu_notifier(struct notifier_block *nb)
{
	return raw_notifier_chain_register(&cpu_chain, nb);
}

static int __cpu_notify(unsigned long val, unsigned int cpu, int nr_to_call,
			int *nr_calls)
{
	unsigned long mod = cpuhp_tasks_frozen ? CPU_TASKS_FROZEN : 0;
	void *hcpu = (void *)(long)cpu;

	int ret;

	ret = __raw_notifier_call_chain(&cpu_chain, val | mod, hcpu, nr_to_call,
					nr_calls);

	return notifier_to_errno(ret);
}

static int cpu_notify(unsigned long val, unsigned int cpu)
{
	return __cpu_notify(val, cpu, -1, NULL);
}

static void cpu_notify_nofail(unsigned long val, unsigned int cpu)
{
	BUG_ON(cpu_notify(val, cpu));
}

/* Notifier wrappers for transitioning to state machine */
static int notify_prepare(unsigned int cpu)
{
	int nr_calls = 0;
	int ret;

	ret = __cpu_notify(CPU_UP_PREPARE, cpu, -1, &nr_calls);
	if (ret) {
		nr_calls--;
		printk(KERN_WARNING "%s: attempt to bring up CPU %u failed\n",
				__func__, cpu);
		__cpu_notify(CPU_UP_CANCELED, cpu, nr_calls, NULL);
	}
	return ret;
}

static int notify_online(unsigned int cpu)
{
	cpu_notify(CPU_ONLINE, cpu);
	return 0;
}

static int notify_starting(unsigned int cpu)
{
	cpu_notify(CPU_STARTING, cpu);
	return 0;
}

static int bringup_wait_for_ap(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);

	wait_for_completion(&st->done);
	return st->result;
}

static int bringup_cpu(unsigned int cpu)
{
	struct task_struct *idle = idle_thread_get(cpu);
	int ret;

	/* Arch-specific enabling code. */
	ret = __cpu_up(cpu, idle);
	if (ret) {
		cpu_notify(CPU_UP_CANCELED, cpu);
		return ret;
	}
	ret = bringup_wait_for_ap(cpu);
	BUG_ON(!cpu_online(cpu));
	return ret;
}

/*
 * Hotplug state machine related functions
 */
static void undo_cpu_down(unsigned int cpu, struct cpuhp_cpu_state *st,
			  struct cpuhp_step *steps)
{
	for (st->state++; st->state < st->target; st->state++) {
		struct cpuhp_step *step = steps + st->state;

		if (!step->skip_onerr)
			cpuhp_invoke_callback(cpu, st->state, step->startup);
	}
}

static int cpuhp_down_callbacks(unsigned int cpu, struct cpuhp_cpu_state *st,
				struct cpuhp_step *steps, enum cpuhp_state target)
{
	enum cpuhp_state prev_state = st->state;
	int ret = 0;

	for (; st->state > target; st->state--) {
		struct cpuhp_step *step = steps + st->state;

		ret = cpuhp_invoke_callback(cpu, st->state, step->teardown);
		if (ret) {
			st->target = prev_state;
			undo_cpu_down(cpu, st, steps);
			break;
		}
	}
	return ret;
}

static void undo_cpu_up(unsigned int cpu, struct cpuhp_cpu_state *st,
			struct cpuhp_step *steps)
{
	for (st->state--; st->state > st->target; st->state--) {
		struct cpuhp_step *step = steps + st->state;

		if (!step->skip_onerr)
			cpuhp_invoke_callback(cpu, st->state, step->teardown);
	}
}

static int cpuhp_up_callbacks(unsigned int cpu, struct cpuhp_cpu_state *st,
			      struct cpuhp_step *steps, enum cpuhp_state target)
{
	enum cpuhp_state prev_state = st->state;
	int ret = 0;

	while (st->state < target) {
		struct cpuhp_step *step;

		st->state++;
		step = steps + st->state;
		ret = cpuhp_invoke_callback(cpu, st->state, step->startup);
		if (ret) {
			st->target = prev_state;
			undo_cpu_up(cpu, st, steps);
			break;
		}
	}
	return ret;
}

/*
 * The cpu hotplug threads manage the bringup and teardown of the cpus
 */
static void cpuhp_create(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);

	init_completion(&st->done);
}

static int cpuhp_should_run(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = this_cpu_ptr(&cpuhp_state);

	return st->should_run;
}

/* Execute the teardown callbacks. Used to be CPU_DOWN_PREPARE */
static int cpuhp_ap_offline(unsigned int cpu, struct cpuhp_cpu_state *st)
{
	enum cpuhp_state target = max((int)st->target, CPUHP_TEARDOWN_CPU);

	return cpuhp_down_callbacks(cpu, st, cpuhp_ap_states, target);
}

/* Execute the online startup callbacks. Used to be CPU_ONLINE */
static int cpuhp_ap_online(unsigned int cpu, struct cpuhp_cpu_state *st)
{
	return cpuhp_up_callbacks(cpu, st, cpuhp_ap_states, st->target);
}

/*
 * Execute teardown/startup callbacks on the plugged cpu. Also used to invoke
 * callbacks when a state gets [un]installed at runtime.
 */
static void cpuhp_thread_fun(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = this_cpu_ptr(&cpuhp_state);
	int ret = 0;

	/*
	 * Paired with the mb() in cpuhp_kick_ap_work and
	 * cpuhp_invoke_ap_callback, so the work set is consistent visible.
	 */
	smp_mb();
	if (!st->should_run)
		return;

	st->should_run = false;

	/* Single callback invocation for [un]install ? */
	if (st->cb) {
		if (st->cb_state < CPUHP_AP_ONLINE) {
			local_irq_disable();
			ret = cpuhp_invoke_callback(cpu, st->cb_state, st->cb);
			local_irq_enable();
		} else {
			ret = cpuhp_invoke_callback(cpu, st->cb_state, st->cb);
		}
	} else if (st->rollback) {
		BUG_ON(st->state < CPUHP_AP_ONLINE_IDLE);

		undo_cpu_down(cpu, st, cpuhp_ap_states);
		/*
		 * This is a momentary workaround to keep the notifier users
		 * happy. Will go away once we got rid of the notifiers.
		 */
		cpu_notify_nofail(CPU_DOWN_FAILED, cpu);
		st->rollback = false;
	} else {
		/* Cannot happen .... */
		BUG_ON(st->state < CPUHP_AP_ONLINE_IDLE);

		/* Regular hotplug work */
		if (st->state < st->target)
			ret = cpuhp_ap_online(cpu, st);
		else if (st->state > st->target)
			ret = cpuhp_ap_offline(cpu, st);
	}
	st->result = ret;
	complete(&st->done);
}

/* Invoke a single callback on a remote cpu */
static int cpuhp_invoke_ap_callback(int cpu, enum cpuhp_state state,
				    int (*cb)(unsigned int))
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);

	if (!cpu_online(cpu))
		return 0;

	/*
	 * If we are up and running, use the hotplug thread. For early calls
	 * we invoke the thread function directly.
	 */
	if (!st->thread)
		return cpuhp_invoke_callback(cpu, state, cb);

	st->cb_state = state;
	st->cb = cb;
	/*
	 * Make sure the above stores are visible before should_run becomes
	 * true. Paired with the mb() above in cpuhp_thread_fun()
	 */
	smp_mb();
	st->should_run = true;
	wake_up_process(st->thread);
	wait_for_completion(&st->done);
	return st->result;
}

/* Regular hotplug invocation of the AP hotplug thread */
static void __cpuhp_kick_ap_work(struct cpuhp_cpu_state *st)
{
	st->result = 0;
	st->cb = NULL;
	/*
	 * Make sure the above stores are visible before should_run becomes
	 * true. Paired with the mb() above in cpuhp_thread_fun()
	 */
	smp_mb();
	st->should_run = true;
	wake_up_process(st->thread);
}

static int cpuhp_kick_ap_work(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	enum cpuhp_state state = st->state;

	trace_cpuhp_enter(cpu, st->target, state, cpuhp_kick_ap_work);
	__cpuhp_kick_ap_work(st);
	wait_for_completion(&st->done);
	trace_cpuhp_exit(cpu, st->state, state, st->result);
	return st->result;
}

static struct smp_hotplug_thread cpuhp_threads = {
	.store			= &cpuhp_state.thread,
	.create			= &cpuhp_create,
	.thread_should_run	= cpuhp_should_run,
	.thread_fn		= cpuhp_thread_fun,
	.thread_comm		= "cpuhp/%u",
	.selfparking		= true,
};

void __init cpuhp_threads_init(void)
{
	BUG_ON(smpboot_register_percpu_thread(&cpuhp_threads));
	kthread_unpark(this_cpu_read(cpuhp_state.thread));
}

EXPORT_SYMBOL(register_cpu_notifier);
EXPORT_SYMBOL(__register_cpu_notifier);
void unregister_cpu_notifier(struct notifier_block *nb)
{
	cpu_maps_update_begin();
	raw_notifier_chain_unregister(&cpu_chain, nb);
	cpu_maps_update_done();
}
EXPORT_SYMBOL(unregister_cpu_notifier);

void __unregister_cpu_notifier(struct notifier_block *nb)
{
	raw_notifier_chain_unregister(&cpu_chain, nb);
}
EXPORT_SYMBOL(__unregister_cpu_notifier);

#ifdef CONFIG_HOTPLUG_CPU
/**
 * clear_tasks_mm_cpumask - Safely clear tasks' mm_cpumask for a CPU
 * @cpu: a CPU id
 *
 * This function walks all processes, finds a valid mm struct for each one and
 * then clears a corresponding bit in mm's cpumask.  While this all sounds
 * trivial, there are various non-obvious corner cases, which this function
 * tries to solve in a safe manner.
 *
 * Also note that the function uses a somewhat relaxed locking scheme, so it may
 * be called only for an already offlined CPU.
 */
void clear_tasks_mm_cpumask(int cpu)
{
	struct task_struct *p;
	struct task_struct *t;

	/*
	 * This function is called after the cpu is taken down and marked
	 * offline, so its not like new tasks will ever get this cpu set in
	 * their mm mask. -- Peter Zijlstra
	 * Thus, we may use rcu_read_lock() here, instead of grabbing
	 * full-fledged tasklist_lock.
	 */
	WARN_ON(cpu_online(cpu));
	rcu_read_lock();
	do_each_thread(p, t) {
		if (likely(t->mm)) {
			cpumask_clear_cpu(cpu, mm_cpumask(t->mm));
			break;
		}
	} while_each_thread(p, t);
	rcu_read_unlock();
}

static inline void check_for_tasks(int dead_cpu)
{
	struct task_struct *g, *p;

	read_lock(&tasklist_lock);
	for_each_process_thread(g, p) {
		if (!p->on_rq)
			continue;
		/*
		 * We do the check with unlocked task_rq(p)->lock.
		 * Order the reading to do not warn about a task,
		 * which was running on this cpu in the past, and
		 * it's just been woken on another cpu.
		 */
		rmb();
		if (task_cpu(p) != dead_cpu)
			continue;

		pr_warn("Task %s (pid=%d) is on cpu %d (state=%ld, flags=%x)\n",
			p->comm, task_pid_nr(p), dead_cpu, p->state, p->flags);
	}
	read_unlock(&tasklist_lock);
}

static int notify_down_prepare(unsigned int cpu)
{
	int err, nr_calls = 0;

	err = __cpu_notify(CPU_DOWN_PREPARE, cpu, -1, &nr_calls);
	if (err) {
		nr_calls--;
		__cpu_notify(CPU_DOWN_FAILED, cpu, nr_calls, NULL);
		pr_warn("%s: attempt to take down CPU %u failed\n",
				__func__, cpu);
	}
	return err;
}

static int notify_dying(unsigned int cpu)
{
	cpu_notify(CPU_DYING, cpu);
	return 0;
}

/* Take this CPU down. */
static int take_cpu_down(void *_param)
{
	struct cpuhp_cpu_state *st = this_cpu_ptr(&cpuhp_state);
	enum cpuhp_state target = max((int)st->target, CPUHP_AP_OFFLINE);
	int err, cpu = smp_processor_id();

	/* Ensure this CPU doesn't handle any more interrupts. */
	err = __cpu_disable();
	if (err < 0)
		return err;

	/* Invoke the former CPU_DYING callbacks */
	for (; st->state > target; st->state--) {
		struct cpuhp_step *step = cpuhp_ap_states + st->state;

		cpuhp_invoke_callback(cpu, st->state, step->teardown);
	}
	/* Give up timekeeping duties */
	tick_handover_do_timer();
	/* Park the stopper thread */
	stop_machine_park(cpu);
	return 0;
}

static int takedown_cpu(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	int err;

	__cpu_unplug_wait(cpu);
	/* Park the smpboot threads */
	kthread_park(per_cpu_ptr(&cpuhp_state, cpu)->thread);
	smpboot_park_threads(cpu);

	/* Notifiers are done. Don't let any more tasks pin this CPU. */
	cpu_unplug_sync(cpu);

	/*
	 * Prevent irq alloc/free while the dying cpu reorganizes the
	 * interrupt affinities.
	 */
	irq_lock_sparse();

	/*
	 * So now all preempt/rcu users must observe !cpu_active().
	 */
	err = stop_machine(take_cpu_down, NULL, cpumask_of(cpu));
	if (err) {
		/* CPU refused to die */
		irq_unlock_sparse();
		/* Unpark the hotplug thread so we can rollback there */
		kthread_unpark(per_cpu_ptr(&cpuhp_state, cpu)->thread);
		return err;
	}
	BUG_ON(cpu_online(cpu));

	/*
	 * The migration_call() CPU_DYING callback will have removed all
	 * runnable tasks from the cpu, there's only the idle task left now
	 * that the migration thread is done doing the stop_machine thing.
	 *
	 * Wait for the stop thread to go away.
	 */
	wait_for_completion(&st->done);
	BUG_ON(st->state != CPUHP_AP_IDLE_DEAD);

	/* Interrupts are moved away from the dying cpu, reenable alloc/free */
	irq_unlock_sparse();

	hotplug_cpu__broadcast_tick_pull(cpu);
	/* This actually kills the CPU. */
	__cpu_die(cpu);

	tick_cleanup_dead_cpu(cpu);
	return 0;
}

static int notify_dead(unsigned int cpu)
{
	cpu_notify_nofail(CPU_DEAD, cpu);
	check_for_tasks(cpu);
	return 0;
}

static void cpuhp_complete_idle_dead(void *arg)
{
	struct cpuhp_cpu_state *st = arg;

	complete(&st->done);
}

void cpuhp_report_idle_dead(void)
{
	struct cpuhp_cpu_state *st = this_cpu_ptr(&cpuhp_state);

	BUG_ON(st->state != CPUHP_AP_OFFLINE);
	rcu_report_dead(smp_processor_id());
	st->state = CPUHP_AP_IDLE_DEAD;
	/*
	 * We cannot call complete after rcu_report_dead() so we delegate it
	 * to an online cpu.
	 */
	smp_call_function_single(cpumask_first(cpu_online_mask),
				 cpuhp_complete_idle_dead, st, 0);
}

#else
#define notify_down_prepare	NULL
#define takedown_cpu		NULL
#define notify_dead		NULL
#define notify_dying		NULL
#endif

#ifdef CONFIG_HOTPLUG_CPU

/* Requires cpu_add_remove_lock to be held */
static int __ref _cpu_down(unsigned int cpu, int tasks_frozen,
			   enum cpuhp_state target)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	int prev_state, ret = 0;
	bool hasdied = false;
	int mycpu;
	cpumask_var_t cpumask;
	cpumask_var_t cpumask_org;

	if (num_online_cpus() == 1)
		return -EBUSY;

	if (!cpu_present(cpu))
		return -EINVAL;

	/* Move the downtaker off the unplug cpu */
	if (!alloc_cpumask_var(&cpumask, GFP_KERNEL))
		return -ENOMEM;
	if (!alloc_cpumask_var(&cpumask_org, GFP_KERNEL))  {
		free_cpumask_var(cpumask);
		return -ENOMEM;
	}

	cpumask_copy(cpumask_org, tsk_cpus_allowed(current));
	cpumask_andnot(cpumask, cpu_online_mask, cpumask_of(cpu));
	set_cpus_allowed_ptr(current, cpumask);
	free_cpumask_var(cpumask);
	migrate_disable();
	mycpu = smp_processor_id();
	if (mycpu == cpu) {
		printk(KERN_ERR "Yuck! Still on unplug CPU\n!");
		migrate_enable();
		ret = -EBUSY;
		goto restore_cpus;
	}

	migrate_enable();
	cpu_hotplug_begin();
	ret = cpu_unplug_begin(cpu);
	if (ret) {
		printk("cpu_unplug_begin(%d) failed\n", cpu);
		goto out_cancel;
	}

	cpuhp_tasks_frozen = tasks_frozen;

	prev_state = st->state;
	st->target = target;
	/*
	 * If the current CPU state is in the range of the AP hotplug thread,
	 * then we need to kick the thread.
	 */
	if (st->state > CPUHP_TEARDOWN_CPU) {
		ret = cpuhp_kick_ap_work(cpu);
		/*
		 * The AP side has done the error rollback already. Just
		 * return the error code..
		 */
		if (ret)
			goto out;

		/*
		 * We might have stopped still in the range of the AP hotplug
		 * thread. Nothing to do anymore.
		 */
		if (st->state > CPUHP_TEARDOWN_CPU)
			goto out;
	}
	/*
	 * The AP brought itself down to CPUHP_TEARDOWN_CPU. So we need
	 * to do the further cleanups.
	 */
	ret = cpuhp_down_callbacks(cpu, st, cpuhp_bp_states, target);
	if (ret && st->state > CPUHP_TEARDOWN_CPU && st->state < prev_state) {
		st->target = prev_state;
		st->rollback = true;
		cpuhp_kick_ap_work(cpu);
	}

	hasdied = prev_state != st->state && st->state == CPUHP_OFFLINE;
out:
	cpu_unplug_done(cpu);
out_cancel:
	cpu_hotplug_done();
	/* This post dead nonsense must die */
	if (!ret && hasdied)
		cpu_notify_nofail(CPU_POST_DEAD, cpu);
restore_cpus:
	set_cpus_allowed_ptr(current, cpumask_org);
	free_cpumask_var(cpumask_org);
	return ret;
}

static int do_cpu_down(unsigned int cpu, enum cpuhp_state target)
{
	int err;

	cpu_maps_update_begin();

	if (cpu_hotplug_disabled) {
		err = -EBUSY;
		goto out;
	}

	err = _cpu_down(cpu, 0, target);

out:
	cpu_maps_update_done();
	return err;
}
int cpu_down(unsigned int cpu)
{
	return do_cpu_down(cpu, CPUHP_OFFLINE);
}
EXPORT_SYMBOL(cpu_down);
#endif /*CONFIG_HOTPLUG_CPU*/

/**
 * notify_cpu_starting(cpu) - call the CPU_STARTING notifiers
 * @cpu: cpu that just started
 *
 * This function calls the cpu_chain notifiers with CPU_STARTING.
 * It must be called by the arch code on the new cpu, before the new cpu
 * enables interrupts and before the "boot" cpu returns from __cpu_up().
 */
void notify_cpu_starting(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	enum cpuhp_state target = min((int)st->target, CPUHP_AP_ONLINE);

	while (st->state < target) {
		struct cpuhp_step *step;

		st->state++;
		step = cpuhp_ap_states + st->state;
		cpuhp_invoke_callback(cpu, st->state, step->startup);
	}
}

/*
 * Called from the idle task. We need to set active here, so we can kick off
 * the stopper thread and unpark the smpboot threads. If the target state is
 * beyond CPUHP_AP_ONLINE_IDLE we kick cpuhp thread and let it bring up the
 * cpu further.
 */
void cpuhp_online_idle(enum cpuhp_state state)
{
	struct cpuhp_cpu_state *st = this_cpu_ptr(&cpuhp_state);
	unsigned int cpu = smp_processor_id();

	/* Happens for the boot cpu */
	if (state != CPUHP_AP_ONLINE_IDLE)
		return;

	st->state = CPUHP_AP_ONLINE_IDLE;

	/* Unpark the stopper thread and the hotplug thread of this cpu */
	stop_machine_unpark(cpu);
	kthread_unpark(st->thread);

	/* Should we go further up ? */
	if (st->target > CPUHP_AP_ONLINE_IDLE)
		__cpuhp_kick_ap_work(st);
	else
		complete(&st->done);
}

/* Requires cpu_add_remove_lock to be held */
static int _cpu_up(unsigned int cpu, int tasks_frozen, enum cpuhp_state target)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	struct task_struct *idle;
	int ret = 0;

	cpu_hotplug_begin();

	if (!cpu_present(cpu)) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * The caller of do_cpu_up might have raced with another
	 * caller. Ignore it for now.
	 */
	if (st->state >= target)
		goto out;

	if (st->state == CPUHP_OFFLINE) {
		/* Let it fail before we try to bring the cpu up */
		idle = idle_thread_get(cpu);
		if (IS_ERR(idle)) {
			ret = PTR_ERR(idle);
			goto out;
		}
	}

	cpuhp_tasks_frozen = tasks_frozen;

	st->target = target;
	/*
	 * If the current CPU state is in the range of the AP hotplug thread,
	 * then we need to kick the thread once more.
	 */
	if (st->state > CPUHP_BRINGUP_CPU) {
		ret = cpuhp_kick_ap_work(cpu);
		/*
		 * The AP side has done the error rollback already. Just
		 * return the error code..
		 */
		if (ret)
			goto out;
	}

	/*
	 * Try to reach the target state. We max out on the BP at
	 * CPUHP_BRINGUP_CPU. After that the AP hotplug thread is
	 * responsible for bringing it up to the target state.
	 */
	target = min((int)target, CPUHP_BRINGUP_CPU);
	ret = cpuhp_up_callbacks(cpu, st, cpuhp_bp_states, target);
out:
	cpu_hotplug_done();
	return ret;
}

static int do_cpu_up(unsigned int cpu, enum cpuhp_state target)
{
	int err = 0;

	if (!cpu_possible(cpu)) {
		pr_err("can't online cpu %d because it is not configured as may-hotadd at boot time\n",
		       cpu);
#if defined(CONFIG_IA64)
		pr_err("please check additional_cpus= boot parameter\n");
#endif
		return -EINVAL;
	}

	err = try_online_node(cpu_to_node(cpu));
	if (err)
		return err;

	cpu_maps_update_begin();

	if (cpu_hotplug_disabled) {
		err = -EBUSY;
		goto out;
	}

	err = _cpu_up(cpu, 0, target);
out:
	cpu_maps_update_done();
	return err;
}

int cpu_up(unsigned int cpu)
{
	return do_cpu_up(cpu, CPUHP_ONLINE);
}
EXPORT_SYMBOL_GPL(cpu_up);

#ifdef CONFIG_PM_SLEEP_SMP
static cpumask_var_t frozen_cpus;

int disable_nonboot_cpus(void)
{
	int cpu, first_cpu, error = 0;

	cpu_maps_update_begin();
	first_cpu = cpumask_first(cpu_online_mask);
	/*
	 * We take down all of the non-boot CPUs in one shot to avoid races
	 * with the userspace trying to use the CPU hotplug at the same time
	 */
	cpumask_clear(frozen_cpus);

	pr_info("Disabling non-boot CPUs ...\n");
	for_each_online_cpu(cpu) {
		if (cpu == first_cpu)
			continue;
		trace_suspend_resume(TPS("CPU_OFF"), cpu, true);
		error = _cpu_down(cpu, 1, CPUHP_OFFLINE);
		trace_suspend_resume(TPS("CPU_OFF"), cpu, false);
		if (!error)
			cpumask_set_cpu(cpu, frozen_cpus);
		else {
			pr_err("Error taking CPU%d down: %d\n", cpu, error);
			break;
		}
	}

	if (!error)
		BUG_ON(num_online_cpus() > 1);
	else
		pr_err("Non-boot CPUs are not disabled\n");

	/*
	 * Make sure the CPUs won't be enabled by someone else. We need to do
	 * this even in case of failure as all disable_nonboot_cpus() users are
	 * supposed to do enable_nonboot_cpus() on the failure path.
	 */
	cpu_hotplug_disabled++;

	cpu_maps_update_done();
	return error;
}

void __weak arch_enable_nonboot_cpus_begin(void)
{
}

void __weak arch_enable_nonboot_cpus_end(void)
{
}

void enable_nonboot_cpus(void)
{
	int cpu, error;

	/* Allow everyone to use the CPU hotplug again */
	cpu_maps_update_begin();
	WARN_ON(--cpu_hotplug_disabled < 0);
	if (cpumask_empty(frozen_cpus))
		goto out;

	pr_info("Enabling non-boot CPUs ...\n");

	arch_enable_nonboot_cpus_begin();

	for_each_cpu(cpu, frozen_cpus) {
		trace_suspend_resume(TPS("CPU_ON"), cpu, true);
		error = _cpu_up(cpu, 1, CPUHP_ONLINE);
		trace_suspend_resume(TPS("CPU_ON"), cpu, false);
		if (!error) {
			pr_info("CPU%d is up\n", cpu);
			continue;
		}
		pr_warn("Error taking CPU%d up: %d\n", cpu, error);
	}

	arch_enable_nonboot_cpus_end();

	cpumask_clear(frozen_cpus);
out:
	cpu_maps_update_done();
}

static int __init alloc_frozen_cpus(void)
{
	if (!alloc_cpumask_var(&frozen_cpus, GFP_KERNEL|__GFP_ZERO))
		return -ENOMEM;
	return 0;
}
core_initcall(alloc_frozen_cpus);

/*
 * When callbacks for CPU hotplug notifications are being executed, we must
 * ensure that the state of the system with respect to the tasks being frozen
 * or not, as reported by the notification, remains unchanged *throughout the
 * duration* of the execution of the callbacks.
 * Hence we need to prevent the freezer from racing with regular CPU hotplug.
 *
 * This synchronization is implemented by mutually excluding regular CPU
 * hotplug and Suspend/Hibernate call paths by hooking onto the Suspend/
 * Hibernate notifications.
 */
static int
cpu_hotplug_pm_callback(struct notifier_block *nb,
			unsigned long action, void *ptr)
{
	switch (action) {

	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		cpu_hotplug_disable();
		break;

	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		cpu_hotplug_enable();
		break;

	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}


static int __init cpu_hotplug_pm_sync_init(void)
{
	/*
	 * cpu_hotplug_pm_callback has higher priority than x86
	 * bsp_pm_callback which depends on cpu_hotplug_pm_callback
	 * to disable cpu hotplug to avoid cpu hotplug race.
	 */
	pm_notifier(cpu_hotplug_pm_callback, 0);
	return 0;
}
core_initcall(cpu_hotplug_pm_sync_init);

#endif /* CONFIG_PM_SLEEP_SMP */

#endif /* CONFIG_SMP */

/* Boot processor state steps */
static struct cpuhp_step cpuhp_bp_states[] = {
	[CPUHP_OFFLINE] = {
		.name			= "offline",
		.startup		= NULL,
		.teardown		= NULL,
	},
#ifdef CONFIG_SMP
	[CPUHP_CREATE_THREADS]= {
		.name			= "threads:create",
		.startup		= smpboot_create_threads,
		.teardown		= NULL,
		.cant_stop		= true,
	},
	[CPUHP_PERF_PREPARE] = {
		.name = "perf prepare",
		.startup = perf_event_init_cpu,
		.teardown = perf_event_exit_cpu,
	},
	[CPUHP_WORKQUEUE_PREP] = {
		.name = "workqueue prepare",
		.startup = workqueue_prepare_cpu,
		.teardown = NULL,
	},
	[CPUHP_HRTIMERS_PREPARE] = {
		.name = "hrtimers prepare",
		.startup = hrtimers_prepare_cpu,
		.teardown = hrtimers_dead_cpu,
	},
	[CPUHP_SMPCFD_PREPARE] = {
		.name = "SMPCFD prepare",
		.startup = smpcfd_prepare_cpu,
		.teardown = smpcfd_dead_cpu,
	},
	[CPUHP_RCUTREE_PREP] = {
		.name = "RCU-tree prepare",
		.startup = rcutree_prepare_cpu,
		.teardown = rcutree_dead_cpu,
	},
	/*
	 * Preparatory and dead notifiers. Will be replaced once the notifiers
	 * are converted to states.
	 */
	[CPUHP_NOTIFY_PREPARE] = {
		.name			= "notify:prepare",
		.startup		= notify_prepare,
		.teardown		= notify_dead,
		.skip_onerr		= true,
		.cant_stop		= true,
	},
	/*
	 * On the tear-down path, timers_dead_cpu() must be invoked
	 * before blk_mq_queue_reinit_notify() from notify_dead(),
	 * otherwise a RCU stall occurs.
	 */
	[CPUHP_TIMERS_DEAD] = {
		.name = "timers dead",
		.startup = NULL,
		.teardown = timers_dead_cpu,
	},
	/* Kicks the plugged cpu into life */
	[CPUHP_BRINGUP_CPU] = {
		.name			= "cpu:bringup",
		.startup		= bringup_cpu,
		.teardown		= NULL,
		.cant_stop		= true,
	},
	[CPUHP_AP_SMPCFD_DYING] = {
		.startup = NULL,
		.teardown = smpcfd_dying_cpu,
	},
	/*
	 * Handled on controll processor until the plugged processor manages
	 * this itself.
	 */
	[CPUHP_TEARDOWN_CPU] = {
		.name			= "cpu:teardown",
		.startup		= NULL,
		.teardown		= takedown_cpu,
		.cant_stop		= true,
	},
#else
	[CPUHP_BRINGUP_CPU] = { },
#endif
};

/* Application processor state steps */
static struct cpuhp_step cpuhp_ap_states[] = {
#ifdef CONFIG_SMP
	/* Final state before CPU kills itself */
	[CPUHP_AP_IDLE_DEAD] = {
		.name			= "idle:dead",
	},
	/*
	 * Last state before CPU enters the idle loop to die. Transient state
	 * for synchronization.
	 */
	[CPUHP_AP_OFFLINE] = {
		.name			= "ap:offline",
		.cant_stop		= true,
	},
	/* First state is scheduler control. Interrupts are disabled */
	[CPUHP_AP_SCHED_STARTING] = {
		.name			= "sched:starting",
		.startup		= sched_cpu_starting,
		.teardown		= sched_cpu_dying,
	},
	[CPUHP_AP_RCUTREE_DYING] = {
		.startup = NULL,
		.teardown = rcutree_dying_cpu,
	},
	/*
	 * Low level startup/teardown notifiers. Run with interrupts
	 * disabled. Will be removed once the notifiers are converted to
	 * states.
	 */
	[CPUHP_AP_NOTIFY_STARTING] = {
		.name			= "notify:starting",
		.startup		= notify_starting,
		.teardown		= notify_dying,
		.skip_onerr		= true,
		.cant_stop		= true,
	},
	/* Entry state on starting. Interrupts enabled from here on. Transient
	 * state for synchronsization */
	[CPUHP_AP_ONLINE] = {
		.name			= "ap:online",
	},
	/* Handle smpboot threads park/unpark */
	[CPUHP_AP_SMPBOOT_THREADS] = {
		.name			= "smpboot:threads",
		.startup		= smpboot_unpark_threads,
		.teardown		= NULL,
	},
	[CPUHP_AP_PERF_ONLINE] = {
		.name = "perf online",
		.startup = perf_event_init_cpu,
		.teardown = perf_event_exit_cpu,
	},
	[CPUHP_AP_WORKQUEUE_ONLINE] = {
		.name = "workqueue online",
		.startup = workqueue_online_cpu,
		.teardown = workqueue_offline_cpu,
	},
	[CPUHP_AP_RCUTREE_ONLINE] = {
		.name = "RCU-tree online",
		.startup = rcutree_online_cpu,
		.teardown = rcutree_offline_cpu,
	},

	/*
	 * Online/down_prepare notifiers. Will be removed once the notifiers
	 * are converted to states.
	 */
	[CPUHP_AP_NOTIFY_ONLINE] = {
		.name			= "notify:online",
		.startup		= notify_online,
		.teardown		= notify_down_prepare,
		.skip_onerr		= true,
	},
#endif
	/*
	 * The dynamically registered state space is here
	 */

#ifdef CONFIG_SMP
	/* Last state is scheduler control setting the cpu active */
	[CPUHP_AP_ACTIVE] = {
		.name			= "sched:active",
		.startup		= sched_cpu_activate,
		.teardown		= sched_cpu_deactivate,
	},
#endif

	/* CPU is fully up and running. */
	[CPUHP_ONLINE] = {
		.name			= "online",
		.startup		= NULL,
		.teardown		= NULL,
	},
};

/* Sanity check for callbacks */
static int cpuhp_cb_check(enum cpuhp_state state)
{
	if (state <= CPUHP_OFFLINE || state >= CPUHP_ONLINE)
		return -EINVAL;
	return 0;
}

static bool cpuhp_is_ap_state(enum cpuhp_state state)
{
	/*
	 * The extra check for CPUHP_TEARDOWN_CPU is only for documentation
	 * purposes as that state is handled explicitely in cpu_down.
	 */
	return state > CPUHP_BRINGUP_CPU && state != CPUHP_TEARDOWN_CPU;
}

static struct cpuhp_step *cpuhp_get_step(enum cpuhp_state state)
{
	struct cpuhp_step *sp;

	sp = cpuhp_is_ap_state(state) ? cpuhp_ap_states : cpuhp_bp_states;
	return sp + state;
}

static void cpuhp_store_callbacks(enum cpuhp_state state,
				  const char *name,
				  int (*startup)(unsigned int cpu),
				  int (*teardown)(unsigned int cpu))
{
	/* (Un)Install the callbacks for further cpu hotplug operations */
	struct cpuhp_step *sp;

	mutex_lock(&cpuhp_state_mutex);
	sp = cpuhp_get_step(state);
	sp->startup = startup;
	sp->teardown = teardown;
	sp->name = name;
	mutex_unlock(&cpuhp_state_mutex);
}

static void *cpuhp_get_teardown_cb(enum cpuhp_state state)
{
	return cpuhp_get_step(state)->teardown;
}

/*
 * Call the startup/teardown function for a step either on the AP or
 * on the current CPU.
 */
static int cpuhp_issue_call(int cpu, enum cpuhp_state state,
			    int (*cb)(unsigned int), bool bringup)
{
	int ret;

	if (!cb)
		return 0;
	/*
	 * The non AP bound callbacks can fail on bringup. On teardown
	 * e.g. module removal we crash for now.
	 */
#ifdef CONFIG_SMP
	if (cpuhp_is_ap_state(state))
		ret = cpuhp_invoke_ap_callback(cpu, state, cb);
	else
		ret = cpuhp_invoke_callback(cpu, state, cb);
#else
	ret = cpuhp_invoke_callback(cpu, state, cb);
#endif
	BUG_ON(ret && !bringup);
	return ret;
}

/*
 * Called from __cpuhp_setup_state on a recoverable failure.
 *
 * Note: The teardown callbacks for rollback are not allowed to fail!
 */
static void cpuhp_rollback_install(int failedcpu, enum cpuhp_state state,
				   int (*teardown)(unsigned int cpu))
{
	int cpu;

	if (!teardown)
		return;

	/* Roll back the already executed steps on the other cpus */
	for_each_present_cpu(cpu) {
		struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
		int cpustate = st->state;

		if (cpu >= failedcpu)
			break;

		/* Did we invoke the startup call on that cpu ? */
		if (cpustate >= state)
			cpuhp_issue_call(cpu, state, teardown, false);
	}
}

/*
 * Returns a free for dynamic slot assignment of the Online state. The states
 * are protected by the cpuhp_slot_states mutex and an empty slot is identified
 * by having no name assigned.
 */
static int cpuhp_reserve_state(enum cpuhp_state state)
{
	enum cpuhp_state i;

	mutex_lock(&cpuhp_state_mutex);
	for (i = CPUHP_AP_ONLINE_DYN; i <= CPUHP_AP_ONLINE_DYN_END; i++) {
		if (cpuhp_ap_states[i].name)
			continue;

		cpuhp_ap_states[i].name = "Reserved";
		mutex_unlock(&cpuhp_state_mutex);
		return i;
	}
	mutex_unlock(&cpuhp_state_mutex);
	WARN(1, "No more dynamic states available for CPU hotplug\n");
	return -ENOSPC;
}

/**
 * __cpuhp_setup_state - Setup the callbacks for an hotplug machine state
 * @state:	The state to setup
 * @invoke:	If true, the startup function is invoked for cpus where
 *		cpu state >= @state
 * @startup:	startup callback function
 * @teardown:	teardown callback function
 *
 * Returns 0 if successful, otherwise a proper error code
 */
int __cpuhp_setup_state(enum cpuhp_state state,
			const char *name, bool invoke,
			int (*startup)(unsigned int cpu),
			int (*teardown)(unsigned int cpu))
{
	int cpu, ret = 0;
	int dyn_state = 0;

	if (cpuhp_cb_check(state) || !name)
		return -EINVAL;

	get_online_cpus();

	/* currently assignments for the ONLINE state are possible */
	if (state == CPUHP_AP_ONLINE_DYN) {
		dyn_state = 1;
		ret = cpuhp_reserve_state(state);
		if (ret < 0)
			goto out;
		state = ret;
	}

	cpuhp_store_callbacks(state, name, startup, teardown);

	if (!invoke || !startup)
		goto out;

	/*
	 * Try to call the startup callback for each present cpu
	 * depending on the hotplug state of the cpu.
	 */
	for_each_present_cpu(cpu) {
		struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
		int cpustate = st->state;

		if (cpustate < state)
			continue;

		ret = cpuhp_issue_call(cpu, state, startup, true);
		if (ret) {
			cpuhp_rollback_install(cpu, state, teardown);
			cpuhp_store_callbacks(state, NULL, NULL, NULL);
			goto out;
		}
	}
out:
	put_online_cpus();
	if (!ret && dyn_state)
		return state;
	return ret;
}
EXPORT_SYMBOL(__cpuhp_setup_state);

/**
 * __cpuhp_remove_state - Remove the callbacks for an hotplug machine state
 * @state:	The state to remove
 * @invoke:	If true, the teardown function is invoked for cpus where
 *		cpu state >= @state
 *
 * The teardown callback is currently not allowed to fail. Think
 * about module removal!
 */
void __cpuhp_remove_state(enum cpuhp_state state, bool invoke)
{
	int (*teardown)(unsigned int cpu) = cpuhp_get_teardown_cb(state);
	int cpu;

	BUG_ON(cpuhp_cb_check(state));

	get_online_cpus();

	if (!invoke || !teardown)
		goto remove;

	/*
	 * Call the teardown callback for each present cpu depending
	 * on the hotplug state of the cpu. This function is not
	 * allowed to fail currently!
	 */
	for_each_present_cpu(cpu) {
		struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
		int cpustate = st->state;

		if (cpustate >= state)
			cpuhp_issue_call(cpu, state, teardown, false);
	}
remove:
	cpuhp_store_callbacks(state, NULL, NULL, NULL);
	put_online_cpus();
}
EXPORT_SYMBOL(__cpuhp_remove_state);

#if defined(CONFIG_SYSFS) && defined(CONFIG_HOTPLUG_CPU)
static ssize_t show_cpuhp_state(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, dev->id);

	return sprintf(buf, "%d\n", st->state);
}
static DEVICE_ATTR(state, 0444, show_cpuhp_state, NULL);

static ssize_t write_cpuhp_target(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, dev->id);
	struct cpuhp_step *sp;
	int target, ret;

	ret = kstrtoint(buf, 10, &target);
	if (ret)
		return ret;

#ifdef CONFIG_CPU_HOTPLUG_STATE_CONTROL
	if (target < CPUHP_OFFLINE || target > CPUHP_ONLINE)
		return -EINVAL;
#else
	if (target != CPUHP_OFFLINE && target != CPUHP_ONLINE)
		return -EINVAL;
#endif

	ret = lock_device_hotplug_sysfs();
	if (ret)
		return ret;

	mutex_lock(&cpuhp_state_mutex);
	sp = cpuhp_get_step(target);
	ret = !sp->name || sp->cant_stop ? -EINVAL : 0;
	mutex_unlock(&cpuhp_state_mutex);
	if (ret)
		return ret;

	if (st->state < target)
		ret = do_cpu_up(dev->id, target);
	else
		ret = do_cpu_down(dev->id, target);

	unlock_device_hotplug();
	return ret ? ret : count;
}

static ssize_t show_cpuhp_target(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, dev->id);

	return sprintf(buf, "%d\n", st->target);
}
static DEVICE_ATTR(target, 0644, show_cpuhp_target, write_cpuhp_target);

static struct attribute *cpuhp_cpu_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_target.attr,
	NULL
};

static struct attribute_group cpuhp_cpu_attr_group = {
	.attrs = cpuhp_cpu_attrs,
	.name = "hotplug",
	NULL
};

static ssize_t show_cpuhp_states(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	ssize_t cur, res = 0;
	int i;

	mutex_lock(&cpuhp_state_mutex);
	for (i = CPUHP_OFFLINE; i <= CPUHP_ONLINE; i++) {
		struct cpuhp_step *sp = cpuhp_get_step(i);

		if (sp->name) {
			cur = sprintf(buf, "%3d: %s\n", i, sp->name);
			buf += cur;
			res += cur;
		}
	}
	mutex_unlock(&cpuhp_state_mutex);
	return res;
}
static DEVICE_ATTR(states, 0444, show_cpuhp_states, NULL);

static struct attribute *cpuhp_cpu_root_attrs[] = {
	&dev_attr_states.attr,
	NULL
};

static struct attribute_group cpuhp_cpu_root_attr_group = {
	.attrs = cpuhp_cpu_root_attrs,
	.name = "hotplug",
	NULL
};

static int __init cpuhp_sysfs_init(void)
{
	int cpu, ret;

	ret = sysfs_create_group(&cpu_subsys.dev_root->kobj,
				 &cpuhp_cpu_root_attr_group);
	if (ret)
		return ret;

	for_each_possible_cpu(cpu) {
		struct device *dev = get_cpu_device(cpu);

		if (!dev)
			continue;
		ret = sysfs_create_group(&dev->kobj, &cpuhp_cpu_attr_group);
		if (ret)
			return ret;
	}
	return 0;
}
device_initcall(cpuhp_sysfs_init);
#endif

/*
 * cpu_bit_bitmap[] is a special, "compressed" data structure that
 * represents all NR_CPUS bits binary values of 1<<nr.
 *
 * It is used by cpumask_of() to get a constant address to a CPU
 * mask value that has a single bit set only.
 */

/* cpu_bit_bitmap[0] is empty - so we can back into it */
#define MASK_DECLARE_1(x)	[x+1][0] = (1UL << (x))
#define MASK_DECLARE_2(x)	MASK_DECLARE_1(x), MASK_DECLARE_1(x+1)
#define MASK_DECLARE_4(x)	MASK_DECLARE_2(x), MASK_DECLARE_2(x+2)
#define MASK_DECLARE_8(x)	MASK_DECLARE_4(x), MASK_DECLARE_4(x+4)

const unsigned long cpu_bit_bitmap[BITS_PER_LONG+1][BITS_TO_LONGS(NR_CPUS)] = {

	MASK_DECLARE_8(0),	MASK_DECLARE_8(8),
	MASK_DECLARE_8(16),	MASK_DECLARE_8(24),
#if BITS_PER_LONG > 32
	MASK_DECLARE_8(32),	MASK_DECLARE_8(40),
	MASK_DECLARE_8(48),	MASK_DECLARE_8(56),
#endif
};
EXPORT_SYMBOL_GPL(cpu_bit_bitmap);

const DECLARE_BITMAP(cpu_all_bits, NR_CPUS) = CPU_BITS_ALL;
EXPORT_SYMBOL(cpu_all_bits);

#ifdef CONFIG_INIT_ALL_POSSIBLE
struct cpumask __cpu_possible_mask __read_mostly
	= {CPU_BITS_ALL};
#else
struct cpumask __cpu_possible_mask __read_mostly;
#endif
EXPORT_SYMBOL(__cpu_possible_mask);

struct cpumask __cpu_online_mask __read_mostly;
EXPORT_SYMBOL(__cpu_online_mask);

struct cpumask __cpu_present_mask __read_mostly;
EXPORT_SYMBOL(__cpu_present_mask);

struct cpumask __cpu_active_mask __read_mostly;
EXPORT_SYMBOL(__cpu_active_mask);

void init_cpu_present(const struct cpumask *src)
{
	cpumask_copy(&__cpu_present_mask, src);
}

void init_cpu_possible(const struct cpumask *src)
{
	cpumask_copy(&__cpu_possible_mask, src);
}

void init_cpu_online(const struct cpumask *src)
{
	cpumask_copy(&__cpu_online_mask, src);
}

/*
 * Activate the first processor.
 */
void __init boot_cpu_init(void)
{
	int cpu = smp_processor_id();

	/* Mark the boot cpu "present", "online" etc for SMP and UP case */
	set_cpu_online(cpu, true);
	set_cpu_active(cpu, true);
	set_cpu_present(cpu, true);
	set_cpu_possible(cpu, true);
}

/*
 * Must be called _AFTER_ setting up the per_cpu areas
 */
void __init boot_cpu_state_init(void)
{
	per_cpu_ptr(&cpuhp_state, smp_processor_id())->state = CPUHP_ONLINE;
}
