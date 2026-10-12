// gozer — forcibly destroy zombie processes on Linux.
//
// A zombie cannot be killed with a signal: it has no code to run, no signal
// handler, no resources to free. The only thing that removes a zombie from
// the process list (from /proc, ps, htop, top, ...) is the kernel function
// release_task(), which is normally called only from a wait4()/waitid() by
// the zombie's parent. This is or by the kernel's own "autoreap" path. This
// program drives exactly those kernel mechanisms, using only syscalls. No
// kernel module, no /proc surgery, no ptrace (ptrace_attach() refuses tasks
// with exit_state set: `if (unlikely(task->exit_state)) return -EPERM;`).
package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

// kernel_sigaction mirrors struct kernel_sigaction on amd64 (and most 64-bit
// Linux arches): handler, flags, restorer, mask.
type kernel_sigaction struct {
	handler  uintptr
	flags    uintptr
	restorer uintptr
	mask     uint64
}

// PR_SET_CHILD_SUBREAPER (linux/prctl.h) — not in the stdlib syscall pkg.
const PR_SET_CHILD_SUBREAPER = 36

func becomeSubreaper() error {
	_, _, errno := syscall.Syscall6(syscall.SYS_PRCTL,
		PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0, 0)
	if errno != 0 {
		return errno
	}
	return nil
}

// ignoreSigchld sets SIGCHLD to SIG_IGN via rt_sigaction.
func ignoreSigchld() error {
	act := &kernel_sigaction{handler: 1} // (sighandler_t)SIG_IGN
	_, _, errno := syscall.Syscall6(syscall.SYS_RT_SIGACTION,
		uintptr(syscall.SIGCHLD),
		uintptr(unsafe.Pointer(act)),
		0, // oldact
		8, // sizeof(kernel_sigset_t)
		0, 0)
	if errno != 0 {
		return errno
	}
	return nil
}

const passDelay = 250 * time.Millisecond
const maxPasses = 120

// --- /proc helpers -------------------------------------------------------

// procStat returns (state, ppid) for a pid. The comm field can contain
// spaces and parens, so the state char is found by scanning backwards from
// the last ')'.
func procStat(pid int) (byte, int, bool) {
	data, err := os.ReadFile(fmt.Sprintf("/proc/%d/stat", pid))
	if err != nil {
		return 0, 0, false
	}
	s := string(data)
	closeParen := strings.LastIndexByte(s, ')')
	if closeParen < 0 || closeParen+3 > len(s) {
		return 0, 0, false
	}
	fields := strings.Fields(s[closeParen+2:])
	if len(fields) < 2 {
		return 0, 0, false
	}
	ppid, err := strconv.Atoi(fields[1])
	if err != nil {
		return 0, 0, false
	}
	return fields[0][0], ppid, true
}

// allPids lists every numeric /proc entry.
func allPids() []int {
	ents, err := os.ReadDir("/proc")
	if err != nil {
		return nil
	}
	var out []int
	for _, e := range ents {
		n := e.Name()
		if n == "" || n[0] < '0' || n[0] > '9' {
			continue
		}
		if pid, err := strconv.Atoi(n); err == nil && pid > 0 {
			out = append(out, pid)
		}
	}
	return out
}

// zombies returns every zombie as (pid, ppid).
func zombies() [][2]int {
	var out [][2]int
	for _, pid := range allPids() {
		st, ppid, ok := procStat(pid)
		if ok && st == 'Z' {
			out = append(out, [2]int{pid, ppid})
		}
	}
	return out
}

// killTree delivers a signal to pid and its whole process group, so
// siblings also die and are reparented to us (and auto-reaped).
func killTree(pid int, sig syscall.Signal) {
	_ = syscall.Kill(-pid, sig) // process group
	_ = syscall.Kill(pid, sig)  // exact pid (non-leader threads, etc.)
}

// reapChild reaps a zombie that is directly our child, using wait4() —
// the ordinary parent mechanism.
func reapChild(pid int) {
	var ws syscall.WaitStatus
	_, _ = syscall.Wait4(pid, &ws, syscall.WNOHANG, nil)
}

// --- main ----------------------------------------------------------------

func main() {
	me := os.Getpid()
	fmt.Printf("gozer: zombie reaper starting (pid %d)\n", me)

	// 1. Subreaper: orphans come to us instead of init.
	if err := becomeSubreaper(); err != nil {
		fmt.Fprintf(os.Stderr, "gozer: prctl(PR_SET_CHILD_SUBREAPER) failed: %v\n", err)
	}

	// 2. Ignore SIGCHLD: kernel auto-reaps every child of ours that dies.
	if err := ignoreSigchld(); err != nil {
		fmt.Fprintf(os.Stderr, "gozer: rt_sigaction(SIGCHLD, SIG_IGN) failed: %v\n", err)
	}

	// Behave well: don't die if the terminal goes away.
	_, _ = syscall.Setsid()

	// 3. Kill-the-parent loop.
	for pass := 1; pass <= maxPasses; pass++ {
		zs := zombies()
		if len(zs) == 0 {
			fmt.Printf("gozer: pass %d: no zombies — done.\n", pass)
			return
		}
		fmt.Printf("gozer: pass %d: %d zombie(s)\n", pass, len(zs))
		for _, z := range zs {
			fmt.Printf("    zombie %d (parent %d)\n", z[0], z[1])
			if z[1] != me && z[1] != 1 {
				// Kill the parent: the dying parent reaps its zombie children (kernel reparent_leader/exit_notify), and any that outlive it are reparented to us.
				killTree(z[1], syscall.SIGKILL)
			}
		}

		time.Sleep(passDelay)

		// 4. Belt-and-braces: wait4() anything zombie that is now directly
		// our child (this is what a plain parent does).
		for _, z := range zombies() {
			if z[1] == me {
				reapChild(z[0])
			}
		}

		time.Sleep(passDelay)
	}

	fmt.Fprintln(os.Stderr, "gozer: still zombies after", maxPasses, "passes")
	os.Exit(1)
}
