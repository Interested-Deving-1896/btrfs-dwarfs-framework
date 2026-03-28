// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_socket.c - Unix domain socket server for the bdfs CLI
 *
 * The daemon exposes a simple request/response protocol over a Unix socket.
 * The bdfs CLI tool connects, sends a JSON-encoded command, and receives a
 * JSON-encoded response.  This avoids requiring the CLI to open /dev/bdfs_ctl
 * directly (which requires root) while still allowing privileged operations
 * to be delegated through the daemon.
 *
 * Protocol:
 *   Request:  { "cmd": "<command>", "args": { ... } }\n
 *   Response: { "status": 0, "data": { ... } }\n
 *             { "status": -<errno>, "error": "<message>" }\n
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#include "bdfs_daemon.h"

#define BDFS_SOCK_BACKLOG   8
#define BDFS_SOCK_BUFSIZE   65536

int bdfs_socket_init(struct bdfs_daemon *d)
{
	struct sockaddr_un addr;
	int fd;
	char *dir_end;
	char dir[256];

	/* Ensure the socket directory exists */
	strncpy(dir, d->cfg.socket_path, sizeof(dir) - 1);
	dir_end = strrchr(dir, '/');
	if (dir_end) {
		*dir_end = '\0';
		if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
			syslog(LOG_ERR, "bdfs: socket dir %s: %m", dir);
			return -errno;
		}
	}

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		syslog(LOG_ERR, "bdfs: unix socket: %m");
		return -errno;
	}

	unlink(d->cfg.socket_path);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, d->cfg.socket_path, sizeof(addr.sun_path) - 1);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		syslog(LOG_ERR, "bdfs: socket bind %s: %m", d->cfg.socket_path);
		close(fd);
		return -errno;
	}

	chmod(d->cfg.socket_path, 0660);

	if (listen(fd, BDFS_SOCK_BACKLOG) < 0) {
		syslog(LOG_ERR, "bdfs: socket listen: %m");
		close(fd);
		return -errno;
	}

	d->sock_fd = fd;
	syslog(LOG_INFO, "bdfs: CLI socket at %s", d->cfg.socket_path);
	return 0;
}

/*
 * Handle a single CLI connection.  Reads one newline-terminated JSON request,
 * dispatches it, and writes a JSON response.
 */
static void bdfs_handle_client(struct bdfs_daemon *d, int client_fd)
{
	char req[BDFS_SOCK_BUFSIZE];
	char resp[BDFS_SOCK_BUFSIZE];
	ssize_t n;
	int status = 0;

	n = recv(client_fd, req, sizeof(req) - 1, 0);
	if (n <= 0)
		goto out;
	req[n] = '\0';

	/*
	 * Minimal command dispatch.  A production implementation would use
	 * a proper JSON parser (e.g. cJSON or jsmn).  Here we do simple
	 * string matching for the command field.
	 */
	if (strstr(req, "\"list-partitions\"")) {
		snprintf(resp, sizeof(resp),
			 "{\"status\":0,\"data\":{\"note\":\"use ioctl\"}}\n");
	} else if (strstr(req, "\"status\"")) {
		snprintf(resp, sizeof(resp),
			 "{\"status\":0,\"data\":{"
			 "\"workers\":%d,"
			 "\"queue_depth\":0"
			 "}}\n",
			 d->worker_count);
	} else if (strstr(req, "\"ping\"")) {
		snprintf(resp, sizeof(resp), "{\"status\":0,\"data\":\"pong\"}\n");
	} else {
		status = -ENOSYS;
		snprintf(resp, sizeof(resp),
			 "{\"status\":%d,\"error\":\"unknown command\"}\n",
			 status);
	}

	send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);

out:
	close(client_fd);
}

void bdfs_socket_loop(struct bdfs_daemon *d)
{
	int client_fd;

	client_fd = accept4(d->sock_fd, NULL, NULL,
			    SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (client_fd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			syslog(LOG_ERR, "bdfs: accept: %m");
		return;
	}

	bdfs_handle_client(d, client_fd);
}
