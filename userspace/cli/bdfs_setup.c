// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_setup.c - bdfs setup subcommand group
 *
 * bdfs setup fstab [OPTIONS]
 *
 *   Generate /etc/fstab entries for a BTRFS filesystem by introspecting
 *   its live subvolume layout.  Delegates to tools/setup/bdfs-genfstab.sh,
 *   which is installed at $(BDFS_LIBEXECDIR)/bdfs-genfstab.sh.
 *
 *   All flags are forwarded verbatim to the script; see bdfs-genfstab.sh
 *   --help for the full option reference.
 *
 * bdfs setup check
 *
 *   Verify that all runtime dependencies are present and report their
 *   versions.  Useful for diagnosing missing tools before first use.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bdfs.h"

/* Installed location of bdfs-genfstab.sh.
 * Overridden at build time via -DBDFS_LIBEXECDIR=<path>. */
#ifndef BDFS_LIBEXECDIR
#define BDFS_LIBEXECDIR "/usr/lib/bdfs"
#endif

#define GENFSTAB_SCRIPT BDFS_LIBEXECDIR "/bdfs-genfstab.sh"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/*
 * Find the genfstab script: prefer the installed path, fall back to the
 * source tree location so the CLI works from a build directory.
 */
static const char *find_genfstab(void)
{
	static char path[PATH_MAX];

	if (access(GENFSTAB_SCRIPT, X_OK) == 0)
		return GENFSTAB_SCRIPT;

	/* Try relative to the directory containing the bdfs binary */
	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n > 0) {
		self[n] = '\0';
		char *slash = strrchr(self, '/');
		if (slash) {
			*slash = '\0';
			snprintf(path, sizeof(path),
				 "%s/../../tools/setup/bdfs-genfstab.sh", self);
			if (access(path, X_OK) == 0)
				return path;
		}
	}

	return NULL;
}

static int run_script(const char *script, char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0) {
		bdfs_err("fork: %s", strerror(errno));
		return 1;
	}
	if (pid == 0) {
		execv(script, argv);
		fprintf(stderr, "bdfs: exec %s: %s\n", script, strerror(errno));
		_exit(127);
	}

	int status;
	if (waitpid(pid, &status, 0) < 0) {
		bdfs_err("waitpid: %s", strerror(errno));
		return 1;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return 1;
}

/* ── bdfs setup fstab ────────────────────────────────────────────────────── */

/*
 * Forward all arguments after "fstab" directly to bdfs-genfstab.sh.
 * The script handles its own --help, so we only intercept -h/--help
 * when it appears as the sole argument to print a brief wrapper note.
 */
static int cmd_setup_fstab(struct bdfs_cli *cli, int argc, char *argv[])
{
	(void)cli;

	const char *script = find_genfstab();
	if (!script) {
		bdfs_err("bdfs-genfstab.sh not found at %s", GENFSTAB_SCRIPT);
		bdfs_err("Install btrfs-dwarfs-framework or set BDFS_LIBEXECDIR");
		return 1;
	}

	/* Build argv for the script: script_path [user args...] NULL */
	char **sargv = calloc((size_t)(argc + 2), sizeof(char *));
	if (!sargv) {
		bdfs_err("out of memory");
		return 1;
	}

	sargv[0] = (char *)script;
	for (int i = 0; i < argc; i++)
		sargv[i + 1] = argv[i];
	sargv[argc + 1] = NULL;

	int ret = run_script(script, sargv);
	free(sargv);
	return ret;
}

/* ── bdfs setup check ────────────────────────────────────────────────────── */

struct tool_check {
	const char *name;
	const char *version_flag;
	bool        required;
};

static const struct tool_check TOOLS[] = {
	{ "btrfs",          "--version", true  },
	{ "mkdwarfs",       "--version", true  },
	{ "dwarfs",         "--version", true  },
	{ "dwarfsextract",  "--version", true  },
	{ "dwarfsck",       "--version", true  },
	{ "fuse-overlayfs", "--version", false },
	{ "fusermount",     "--version", false },
	{ "findmnt",        "--version", true  },
	{ "lsblk",          "--version", true  },
};

/*
 * Capture the first line of a tool's version output without invoking a shell.
 * execvp performs PATH lookup while keeping the executable name and arguments
 * as distinct argv elements. Output is bounded and the child is always reaped.
 */
static int capture_tool_version(const char *tool, const char *flag,
			       char *output, size_t output_size)
{
	int pipefd[2];
	pid_t pid;
	int status = 0;
	size_t used = 0;
	char buffer[256];

	if (!output || output_size == 0 || pipe(pipefd) < 0)
		return -1;
	output[0] = '\0';

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
		    dup2(pipefd[1], STDERR_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);
		execlp(tool, tool, flag, (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);
	for (;;) {
		ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
		if (n == 0)
			break;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(pipefd[0]);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
				;
			return -1;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (buffer[i] == '\n')
				goto output_complete;
			if (used + 1 < output_size)
				output[used++] = buffer[i];
		}
	}
output_complete:
	output[used] = '\0';
	close(pipefd[0]);
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int cmd_setup_check(struct bdfs_cli *cli, int argc, char *argv[])
{
	(void)cli; (void)argc; (void)argv;

	int missing_required = 0;

	printf("%-20s %-10s %s\n", "Tool", "Status", "Version");
	printf("%-20s %-10s %s\n", "----", "------", "-------");

	for (size_t i = 0; i < sizeof(TOOLS) / sizeof(TOOLS[0]); i++) {
		const struct tool_check *t = &TOOLS[i];
		char version[128] = "";

		int rc = capture_tool_version(t->name, t->version_flag,
					      version, sizeof(version));
		if (rc == 127 || rc < 0) {
			printf("%-20s %-10s %s\n",
			       t->name,
			       t->required ? "MISSING*" : "absent",
			       t->required ? "(required)" : "(optional)");
			if (t->required)
				missing_required++;
			continue;
		}

		printf("%-20s %-10s %s\n", t->name, "ok",
		       version[0] ? version : "(version unavailable)");
	}

	if (missing_required) {
		printf("\n%d required tool(s) missing. "
		       "Install them via your package manager.\n",
		       missing_required);
		return 1;
	}

	printf("\nAll required tools present.\n");
	return 0;
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

int cmd_setup(struct bdfs_cli *cli, int argc, char *argv[])
{
	if (argc < 1) {
		fprintf(stderr,
			"Usage: bdfs setup <subcommand> [options]\n"
			"\n"
			"Subcommands:\n"
			"  fstab   Generate /etc/fstab entries for a BTRFS filesystem\n"
			"  check   Verify runtime dependencies\n"
			"\n"
			"Run 'bdfs setup <subcommand> --help' for details.\n");
		return 1;
	}

	const char *sub = argv[0];
	int sub_argc = argc - 1;
	char **sub_argv = argv + 1;

	if (strcmp(sub, "fstab") == 0)
		return cmd_setup_fstab(cli, sub_argc, sub_argv);
	if (strcmp(sub, "check") == 0)
		return cmd_setup_check(cli, sub_argc, sub_argv);

	bdfs_err("unknown setup subcommand: %s", sub);
	return 1;
}
