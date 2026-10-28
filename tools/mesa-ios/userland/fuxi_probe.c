/*
 * fuxi_probe — the part of the EAGL bridge that says, visibly, that the library
 * was loaded into the process it was injected into.
 *
 * The kernel's exec_inject sets DYLD_INSERT_LIBRARIES for backboardd (proven by a
 * panic build: "FUXI INJECT: backboardd gets /redwood/lib/libredwood-gl.dylib").
 * Whether dyld then *honours* that for a platform binary is the open question, and
 * the only channel back is the screen. So: the first time this library is loaded,
 * reboot the machine once (a marker file keeps it from looping). A reboot right
 * after "exec backboardd" means dyld loaded us; no reboot means it did not.
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* the iOS SDK has no <sys/reboot.h>; libSystem still exports reboot(2) */
extern int reboot(int howto);

#define MARKER "/redwood/bridge-loaded"
#define LOG    "/redwood/bridge-loaded.log"

__attribute__((constructor)) static void
fuxi_probe_loaded(void)
{
	FILE *log = fopen(LOG, "a");

	if (log != NULL) {
		fprintf(log, "fuxi_probe: library loaded (pid %d)\n", (int)getpid());
		fclose(log);
	}

	/* one reboot per root image, so a working load cannot turn into a loop */
	if (access(MARKER, F_OK) == 0) {
		return;
	}
	int fd = open(MARKER, O_CREAT | O_WRONLY, 0644);
	if (fd >= 0) {
		close(fd);
	}

	sleep(3);
	sync();
	reboot(0);
}
