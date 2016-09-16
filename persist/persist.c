/*
 * Linux-specific persistent program.
 *
 * persist should do a few things:
 * 1. a fork should watch the binary. if it's removed, restore it and if
 *    the parent is killed, restart it.
 * 2. occasionally send messages to syslog
 * 3. the fork should rename itself.
 */

/* Feature macros. */
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/*
 * BIN_NAME is the name this is built under, and WATCHER_NAME is the name
 * the watcher process will take up.
 */
#define BIN_NAME	"persist"
#define WATCHER_NAME	"bash"

#ifndef MAX_PATH
#define MAX_PATH	4096
#endif


static pid_t	 pid = 0;
static char	*name = NULL;
static char	 exe[PATH_MAX];
static ssize_t	 exelen = 0;


/*
 * init prepopulates the original exe pathname.
 *
 * before we start forking around (keeping in mind the child process
 * will attempt to rename itself), we need to know where the original
 * program is. this probably should be done before the file is deleted,
 * otherwise the " (deleted)" part of the name needs to be removed,
 * which is doable but extra work.
 *
 * this exe name is kept around so we know where to write the restored
 * file to in the event the original is deleted. this works by using
 * the Linux procfs system: /proc/$$/exe is a special link; it can be
 * read like a normal file (even if the target is deleted), and if
 * readlink(2) is called on it, it returns the original path to the
 * program (with " (deleted)" appended if the original was unlinked).
 */
static void
init(void)
{
	char	*p = NULL;

	memset(exe, 0, MAX_PATH+1);
	asprintf(&p, "/proc/%u/exe", pid);
	exelen = readlink(p, exe, MAX_PATH);
	free(p);

	if (-1 == exelen) {
		err(EXIT_FAILURE, "couldn't look up original file (%u, %s)", pid, p);
	}
}


/*
 * check_bin makes sure the original exe (as named by the exe value) is
 * present. if not, the current program (/proc/getpid()/exe) is copied
 * to the path named by exe using the sendfile(2) syscall.
 */
static void
check_bin(void)
{
	struct stat	 st;
	int		 failed = 0;
	char		*p = NULL;
	off_t		 origlen;
	int		 src = 0, dst = 0;

	if (0 == stat(exe, &st)) {
		/*
		 * The original is in place, and we don't need to do anything.
		 */
		goto fin;
	}

	asprintf(&p, "/proc/%u/exe", getpid());

	/*
	 * Now, figure out how big the exe actually is.
	 */
	if (-1 == stat(p, &st)) {
		failed = 1;
		goto fin;
	}

	origlen = st.st_size;

	/*
	 * Open file descriptors for the source and destination files;
         * these are used by sendfile(2).
	 */
	if (-1 == (dst = open(exe, O_CREAT|O_WRONLY, 0755))) {
		failed = 1;
		goto fin;
	}

	if (-1 == (src = open(p, O_RDONLY))) {
		failed = 1;
		goto fin;
	}

	if (-1 == sendfile(dst, src, 0, (size_t)origlen)) {
		failed = 1;
		goto fin;
	}

fin:
	free(p);

	if (dst > 0) {
		close(dst);
	}

	if (src > 0) {
		close(src);
	}

	if (failed) {
		err(EXIT_FAILURE, "failed to restore");
	}
}


/*
 * check_run checks to see whether the parent process is running. before
 * forking, the pid (or ppid) is stored in a static var. by stat(2)'ing
 * /proc/pid, we can tell if the parent is running or not.
 *
 * if it's not running, we execv into a new parent process.
 *
 * N.B. this will fail if the path named by exe isn't present, so this
 * should be called right after check_bin for maximum success.
 */
static void
check_run(void)
{
	struct stat	 st;
	char		*nargv[2] = {BIN_NAME, NULL};
	char		*p = NULL;

	asprintf(&p, "/proc/%u", pid);
	if (0 == stat(p, &st)) {
		/* Process is still running, so there's nothing to do. */
		free(p);
		return;
	}

	/* Process isn't running, so restart it. */
	free(p);
	execv(exe, nargv);
}


static void
reset_comm(void)
{
	FILE	*comm = NULL;
	char	*p = NULL;

	asprintf(&p, "/proc/%u/comm", pid);
	if (NULL != (comm = fopen(p, "w"))) {
		fwrite(WATCHER_NAME, strlen(WATCHER_NAME), 1, comm);
		fclose(comm);
	} else {
		warn("failed to rewrite commandline");
	}

	free(p);
}

static void
watch(void)
{
	while (1) {
		sleep(60);
		check_bin();
		check_run();
	}
}


static void
spam(void)
{
	while (1) {
		syslog(LOG_INFO, "hey! you!");
		sleep(60);
	}
}


int
main(int argc, char *argv[])
{
	char	*nargv[2] = {WATCHER_NAME, NULL};

	if (0 == strcmp(argv[0], WATCHER_NAME)) {
		pid = getppid();
		reset_comm();
	} else {
		daemon(1, 1);
		pid = getpid();
	}

	init();

	/*
	 * Make sure to spam every console using LOG_EMERG.
	 */
	openlog("persist", LOG_CONS|LOG_NDELAY, LOG_DAEMON);

	if (pid == getpid()) {
		switch (fork()) {
		case -1:
			/* Generate a core dump. */
			abort();
		case 0:
			execvp(exe, nargv);
		default:
			spam();
		}
	} else {
		watch();
	}
}

