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
static off_t	 name_diff = 0;


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

	name_diff = (strlen(BIN_NAME) - strlen(WATCHER_NAME));
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


/* 6 chars for "Name:\t", 7 chars for "persist", 1 char for "\n". */
static const off_t	name_offs = 14;


/*
 * reset_comm is an attempt at rewriting the commandline. I'm not sure it works.
 *
 * ps(1) appears to use /proc/pid/cmdline (which is populated with argv) to
 * display processes. You can see this when running persist; one shows up as
 * "persist" and the other shows up as "bash" (e.g. BIN_NAME and WATCHER_NAME);
 * the nargv construct used in main and check_run appears to be working
 * correctly.
 *
 * however, pgrep(1) appears to use /proc/pid/status, which means pgrep will
 * show both processes as "persist" (and therefore, so will persist). We
 * try to rewrite this file, but it will likely fail. The kernel doesn't like
 * processes changing this data.
 */
static void
reset_comm(void)
{
	char		 buf[4096];
	struct stat	 st;
	off_t		 len, offs;
	FILE		*comm = NULL;
	char		*p = NULL;

	asprintf(&p, "/proc/%u/comm", pid);
	if (NULL != (comm = fopen(p, "w"))) {
		fwrite(WATCHER_NAME, strlen(WATCHER_NAME), 1, comm);
		fclose(comm);
		comm = NULL;
	} else {
		warn("failed to rewrite commandline");
	}

	free(p);
	p = NULL;

	asprintf(&p, "/proc/%u/status", getpid());
	if (-1 == stat(p, &st)) {
		warn("failed to rewrite status");
		goto fin;
	}

	memset(buf, 0, 4096);
	len = st.st_size;
	strcpy(buf, "Name:\t");
	offs += 6;

	strcpy(buf+offs, WATCHER_NAME);
	offs += strlen(WATCHER_NAME);

	buf[offs++] = 0xa;

	if (NULL == (comm = fopen(p, "r"))) {
		warn("failed to read status");
		goto fin;
	}

	fseek(comm, name_offs, SEEK_SET);
	if (-1 == fread(buf+offs, sizeof(char), len-name_offs, comm)) {
		warn("failed to read status");
		goto fin;
	}
	fclose(comm);
	comm = NULL;

	if (NULL == (comm = fopen(p, "w"))) {
		warn("failed to write status");
		goto fin;
	}

	if (-1 == fwrite(buf, sizeof(char), len-name_diff, comm)) {
		warn("failed to write status");
		goto fin;
	}

	fclose(comm);
	comm = NULL;
	printf("%s\n", buf);
fin:
	free(p);

	if (NULL != comm) {
		fclose(comm);
	}
}


/*
 * watch is a non-terminating loop that runs check_bin and check_run every
 * minute.
 */
static void
watch(void)
{
	while (1) {
		sleep(60);
		check_bin();
		check_run();
	}
}


/*
 * spam is a non-terminating loop that writes syslog messages every hour. For
 * maxmimum fun, it uses LOG_EMERG to spam on every console.
 */
static void
spam(void)
{
	while (1) {
		syslog(LOG_EMERG, "hey! you!");
		sleep(3600);
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

