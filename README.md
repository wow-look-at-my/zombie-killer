# zombie-killer — gozer

A userspace program that **forcibly and permanently removes zombie processes
from the Linux process list** (from `ps`, `htop`, `top`, `/proc`, `pidof`,
everything — without modifying any of them, and without a kernel module).

## Why zombies can't be "killed"

A zombie is a process that has already exited; it has no code, no signal
handler, no resources. Sending it SIGKILL does nothing. The *only* thing that
removes a zombie from the process list is the kernel function
`release_task()` (which unhashes the task: `detach_pid()` + `list_del()` from
the task list), and it is normally called only by a `wait4()`/`waitid()` from
the zombie's parent — or by the kernel's own *autoreap* path.

So the problem is: **make the kernel release_task() the zombies**, using only
the parent/child machinery the kernel already has.

## The mechanism (kernel-sourced)

All of this is in mainline Linux, `kernel/exit.c` and `kernel/signal.c`:

1. **Autoreap on SIG_IGN** — `do_notify_parent()` (`kernel/signal.c`):

   ```c
   if (!tsk->ptrace && sig == SIGCHLD &&
       (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN ||
        (psig->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDWAIT))) {
       autoreap = true;
       if (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN)
           sig = 0;
   ```

   A parent that ignores SIGCHLD causes its dead children to be
   auto-reaped: `exit_notify()` puts them on its `dead` list with
   `exit_state = EXIT_DEAD` and `release_task()`s them. The zombie never
   exists.

2. **Zombie children die with their parent** — `reparent_leader()`
   (`kernel/exit.c`), called from `forget_original_parent()` while a process
   exits, for every child that is *already* `EXIT_ZOMBIE`:

   ```c
   if (!p->ptrace &&
       p->exit_state == EXIT_ZOMBIE && thread_group_empty(p)) {
       if (do_notify_parent(p, p->exit_signal)) {
           p->exit_state = EXIT_DEAD;
           list_add(&p->ptrace_entry, dead);
       }
   }
   ```

   …and `exit_notify()` then `release_task()`s everything on that `dead`
   list. **When a parent dies, its zombie children are reaped by the dying
   parent itself — provided the *new* parent ignores SIGCHLD.**

3. **Subreaper** — `prctl(PR_SET_CHILD_SUBREAPER)`: orphaned children are
   reparented to us instead of init, so *we* decide what happens to them.

### What gozer does

1. Becomes a subreaper, so children of dying processes come to *it*.
2. Sets SIGCHLD to `SIG_IGN`, so *every* child that dies while reparented to
   it is auto-reaped by the kernel (rule 1) — permanently, no `wait()`
   needed.
3. Every 250 ms scans `/proc` for `state == 'Z'`. For each zombie whose
   parent is still alive, it **SIGKILLs the parent** (and its process group,
   so surviving siblings are reparented to gozer and auto-reaped too). Each
   dying parent reaps its already-zombie children (rule 2), so the zombie
   climbs the parent chain and is auto-reaped the moment it lands on gozer —
   or on init, which reaps its own children anyway.
4. As a belt-and-braces sweep, calls `wait4(pid, …, WNOHANG)` on any zombie
   that is directly its child — exactly what a normal parent does.

That's it. The only thing "destroyed" is the zombie's *parent*. Once
`release_task()` runs, the task is unhashed and can never reappear — not in
`ps`, not in `htop`, not in `/proc`.

### Why not ptrace?

`ptrace_attach()` explicitly refuses tasks that have exited:

```c
if (unlikely(task->exit_state))
    return -EPERM;
```

so there is no ptrace-based way to touch an existing zombie. (And a zombie
can't be coerced into exiting again anyway.)

## Build & run

```sh
make            # builds ./gozer
sudo ./gozer    # run as root so it may kill any parent
```

`gozer` runs until `/proc` shows zero zombies, then exits 0 (exit 1 if it
couldn't finish in ~30 s). It detaches into its own session and can be
stopped with SIGTERM/SIGINT.

## Test

```sh
make test
```

Builds `test/makezombies` (a C program that forks N children which `_exit(0)`
immediately and are never reaped — classic zombies), spawns 20 of them, shows
them in `ps`, runs gozer, and asserts that zero zombies remain.

## Caveats

- **PID 1's own children**: if a zombie is a direct child of PID 1, gozer
  does not kill PID 1; such zombies are reaped by init itself (systemd
  reaps; a plain `init` that ignores SIGCHLD auto-reaps). The verification
  loop still reports them.
- **Unkillable parents**: a parent in uninterruptible sleep (`D`) that never
  exits can delay completion; the sweep keeps trying and gives up after 120
  passes with exit code 1.
- This intentionally kills the parents of zombies. On the single test
  machine that is the point; do not run it on a production box with
  processes you care about.
- Kernel version: relies on behavior present since Linux ~2.6.9 (subreaper
  since 3.4); verified against mainline `kernel/exit.c` and `kernel/signal.c`
  (v6.6). No per-kernel compilation needed — this is pure userspace syscalls.
