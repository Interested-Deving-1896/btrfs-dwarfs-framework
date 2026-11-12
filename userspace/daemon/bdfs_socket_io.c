// SPDX-License-Identifier: GPL-2.0-or-later
/* Bounded, newline-framed Unix-domain socket I/O helpers. */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

#include "bdfs_socket_io.h"

#define BDFS_SOCK_IO_TIMEOUT_MS 5000

/* Wait for socket readiness against one absolute monotonic deadline. */
int bdfs_socket_wait(int fd, short events,
			    const struct timespec *deadline)
{
	struct pollfd pfd = {.fd = fd, .events = events};
	for (;;) {
		struct timespec now;
		int timeout_ms;
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return -errno;
		int64_t remaining =
			(int64_t)(deadline->tv_sec - now.tv_sec) * 1000 +
			(deadline->tv_nsec - now.tv_nsec) / 1000000;
		if (remaining <= 0)
			return -ETIMEDOUT;
		timeout_ms = remaining > INT_MAX ? INT_MAX : (int)remaining;
		int rc = poll(&pfd, 1, timeout_ms);
		if (rc > 0) {
			if (pfd.revents & (POLLERR | POLLNVAL))
				return -EIO;
			if (pfd.revents & (events | POLLHUP))
				return 0;
			continue;
		}
		if (rc == 0)
			return -ETIMEDOUT;
		if (errno != EINTR)
			return -errno;
	}
}

/* Read exactly one bounded newline-framed request before the deadline. */
int bdfs_socket_read_request(int fd, char *buf, size_t capacity)
{
	struct timespec deadline;
	size_t used = 0;
	if (!buf || capacity < 2)
		return -EINVAL;
	if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
		return -errno;
	deadline.tv_sec += BDFS_SOCK_IO_TIMEOUT_MS / 1000;
	deadline.tv_nsec += (BDFS_SOCK_IO_TIMEOUT_MS % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	while (used < capacity - 1) {
		int rc = bdfs_socket_wait(fd, POLLIN, &deadline);
		if (rc < 0)
			return rc;
		char ch;
		ssize_t n = recv(fd, &ch, 1, 0);
		if (n == 1) {
			if (ch == '\n') {
				buf[used] = '\0';
				return used ? (int)used : -EINVAL;
			}
			if (ch == '\0')
				return -EINVAL;
			buf[used++] = ch;
			continue;
		}
		if (n == 0)
			return -ECONNRESET;
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			return -errno;
	}
	return -EMSGSIZE;
}

/* Send the complete response, handling short writes and nonblocking sockets. */
int bdfs_socket_write_response(int fd, const char *buf, size_t length)
{
	struct timespec deadline;
	size_t sent = 0;
	if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
		return -errno;
	deadline.tv_sec += BDFS_SOCK_IO_TIMEOUT_MS / 1000;
	deadline.tv_nsec += (BDFS_SOCK_IO_TIMEOUT_MS % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	while (sent < length) {
		int rc = bdfs_socket_wait(fd, POLLOUT, &deadline);
		if (rc < 0)
			return rc;
		ssize_t n = send(fd, buf + sent, length - sent, MSG_NOSIGNAL);
		if (n > 0) {
			sent += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EINTR || errno == EAGAIN ||
			      errno == EWOULDBLOCK))
			continue;
		return n == 0 ? -EPIPE : -errno;
	}
	return 0;
}

