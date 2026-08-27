// makezombies — spawn a bunch of zombie children and never reap them.
//
// Each child _exit()s immediately and becomes a zombie under this process,
// which then sleeps forever without ever calling wait(). Run it, then run
// gozer: gozer will SIGKILL this process, at which point every zombie is
// reparented to gozer (or init) and auto-reaped, because gozer ignores
// SIGCHLD (kernel autoreap) — or reaped via wait4().
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 10;

	printf("makezombies: spawning %d zombies, then sleeping forever (never wait()s)\n", n);
	fflush(stdout);
	for (int i = 0; i < n; i++) {
		pid_t p = fork();
		if (p == 0)
			_exit(0);          /* dies immediately -> zombie under us */
		if (p < 0) {
			perror("fork");
			exit(1);
		}
	}
	for (;;)
		pause();
}
