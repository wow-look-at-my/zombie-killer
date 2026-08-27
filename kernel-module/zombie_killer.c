// SPDX-License-Identifier: GPL-2.0
/*
 * zombie_killer.c — remove zombie processes by walking the kernel's
 * process list.
 *
 * A zombie is a task_struct in the EXIT_ZOMBIE state that is still
 * doubly-linked into:
 *   - the global process list anchored at init_task (init_task.tasks,
 *     walked by for_each_process(); /proc, ps, htop and top all iterate
 *     exactly this list), and
 *   - its parent's children list (task->sibling),
 * and it is still findable in the pid hash.
 *
 * Signals cannot touch a zombie (it has no code and no handler); it is
 * removed only when the kernel release_task()s it — normally from a
 * parent's wait4()/waitid(), or via the autoreap path in exit_notify().
 * This module reaps them the way the kernel does, from the inside:
 *
 *   Pass 1 (under RCU): collect every EXIT_ZOMBIE task by walking
 *   for_each_process(), taking a reference on each candidate so the
 *   task_struct cannot vanish while we work on it.
 *
 *   Pass 2 (under RCU — tasklist_lock is kernel-internal and not
 *   exported to modules, so we cannot take it), for each victim:
 *     - set exit_state = EXIT_DEAD. This is the load-bearing step: every
 *       wait path and every reparent path skips EXIT_DEAD tasks
 *       (wait_consider_task(): "if (unlikely(exit_state == EXIT_DEAD))
 *       return 0;", reparent_leader(): "if (unlikely(p->exit_state ==
 *       EXIT_DEAD)) return;"), so nobody can ever again try to
 *       release_task() it — which would double-unhash and corrupt the
 *       lists. We atomically "own" the reap, exactly like wait_task_zombie()
 *       does with its cmpxchg, just without the tasklist lock.
 *     - list_del_rcu(&p->tasks): unlink from the global list — this is
 *       what makes the task vanish from /proc, ps, htop, top, pidof.
 *     - the parent's children list (sibling) and thread_node are left
 *       linked: their stale pointers are harmless, because EXIT_DEAD
 *       makes every wait and reparent path skip the task.
 *
 * The task_struct itself is intentionally left allocated: a zombie holds
 * no resources, and freeing it fully would mean replicating release_task()
 * (not exported). The residual one-page-per-zombie leak is the accepted
 * "doesn't matter if something isn't fully cleaned up" tradeoff. It also
 * means the pid number stays allocated and nr_threads is not decremented —
 * cosmetic, and irrelevant to the goal.
 *
 * Compile on the target machine (per-kernel, by design):
 *     cd kernel-module && make          # uses headers for the running kernel
 * Load — module init runs one sweep:
 *     sudo insmod zombie_killer.ko
 * Re-run any time without reloading:
 *     sudo sh -c 'echo 1 > /sys/module/zombie_killer/parameters/run'
 * Unload:
 *     sudo rmmod zombie_killer
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>   /* for_each_process()                  */
#include <linux/sched/task.h>     /* EXIT_ZOMBIE / EXIT_DEAD             */
#include <linux/init_task.h>      /* init_task                           */
#include <linux/pid_namespace.h>  /* task_active_pid_ns()                */
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Walk the kernel process list (init_task.tasks) and remove zombie processes");
MODULE_AUTHOR("zombie-killer");

#define MAX_ZOMBIES 65536

static struct task_struct *zombies[MAX_ZOMBIES];
static int nzombies;

/* Is this task a zombie we are allowed to reap? */
static bool zk_is_zombie(struct task_struct *p)
{
	if (p == &init_task)
		return false;
	if (p->exit_state != EXIT_ZOMBIE)
		return false;
	if (p->flags & PF_KTHREAD)
		return false;                /* kernel threads are kthreadd's job   */
	if (p->ptrace)
		return false;                /* a tracer owns the reap; skip        */
	if (!thread_group_empty(p))
		return false;                /* leader with live subthreads         */
	if (is_global_init(p))
		return false;                /* never touch PID 1                   */
	if (task_active_pid_ns(p)->child_reaper == p)
		return false;                /* ... or any pid-namespace init       */
	return true;
}

/*
 * Unlink one zombie from the global process list — the step that makes
 * it vanish from /proc, ps, htop and top.
 *
 * tasklist_lock is a kernel-internal symbol, not exported to modules,
 * so we cannot take the write lock the way wait_task_zombie() does.
 * Instead:
 *   - pass 1 already took a reference (get_task_struct), so the
 *     task_struct cannot be freed underneath us;
 *   - we set EXIT_DEAD first, so no wait/reparent path can touch the
 *     task (or its lists) again after we unlink it;
 *   - readers of the process list (for_each_process(), /proc) walk it
 *     under RCU, so list_del_rcu() keeps concurrent traversals safe.
 *
 * Returns the victim's parent pid for the log line (best-effort read;
 * the parent pointer cannot go NULL on us because we hold a reference).
 */
static int zk_unlink(struct task_struct *p)
{
	int ppid;

	rcu_read_lock();
	ppid = rcu_dereference(p->real_parent)->pid;
	rcu_read_unlock();

	p->exit_state = EXIT_DEAD;

	rcu_read_lock();
	list_del_rcu(&p->tasks);
	rcu_read_unlock();

	return ppid;
}

static int zk_sweep(void)
{
	struct task_struct *p;
	int i, reaped = 0;

	nzombies = 0;

	/* Pass 1: collect candidates via for_each_process() on the
	 * init_task.tasks doubly-linked list. */
	rcu_read_lock();
	for_each_process(p) {
		if (nzombies >= MAX_ZOMBIES)
			break;
		if (!zk_is_zombie(p))
			continue;
		get_task_struct(p);
		zombies[nzombies++] = p;
	}
	rcu_read_unlock();

	/* Pass 2: unlink everything we collected. */
	for (i = 0; i < nzombies; i++) {
		int ppid = zk_unlink(zombies[i]);
		pr_info("zombie-killer: removed zombie %d (\"%s\"), was child of %d\n",
			zombies[i]->pid, zombies[i]->comm, ppid);
		put_task_struct(zombies[i]);
		reaped++;
	}

	pr_info("zombie-killer: sweep done — %d zombie(s) removed from the process list\n",
		reaped);
	return reaped;
}

/* Re-run on demand: echo 1 > /sys/module/zombie_killer/parameters/run */
static int run;
static int zk_run_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (!ret && run)
		zk_sweep();
	return ret;
}
static const struct kernel_param_ops zk_run_ops = {
	.set = zk_run_set,
	.get = param_get_int,
};
module_param_cb(run, &zk_run_ops, &run, 0600);
MODULE_PARM_DESC(run,
		 "write nonzero to re-run the sweep: echo 1 > /sys/module/zombie_killer/parameters/run");

static int __init zk_init(void)
{
	pr_info("zombie-killer: loaded; walking the global process list (init_task.tasks)\n");
	zk_sweep();
	return 0;
}

static void __exit zk_exit(void)
{
	pr_info("zombie-killer: unloaded\n");
}

module_init(zk_init);
module_exit(zk_exit);