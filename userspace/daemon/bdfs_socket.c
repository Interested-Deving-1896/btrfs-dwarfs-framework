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
#include "bdfs_policy.h"

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
		/* Count queued jobs */
		int queue_depth = 0;
		pthread_mutex_lock(&d->queue_lock);
		struct bdfs_job *j;
		TAILQ_FOREACH(j, &d->job_queue, entry) queue_depth++;
		pthread_mutex_unlock(&d->queue_lock);

		uint64_t total_demotes = 0;
		time_t last_scan = 0;
		if (d->policy) {
			total_demotes = d->policy->total_demotes;
			last_scan     = d->policy->last_scan_time;
		}
		snprintf(resp, sizeof(resp),
			 "{\"status\":0,\"data\":{"
			 "\"workers\":%d,"
			 "\"queue_depth\":%d,"
			 "\"active_mounts\":%d,"
			 "\"policy_demotes\":%llu,"
			 "\"policy_last_scan\":%lld"
			 "}}\n",
			 d->worker_count, queue_depth,
			 bdfs_mount_count(d),
			 (unsigned long long)total_demotes,
			 (long long)last_scan);

	} else if (strstr(req, "\"ping\"")) {
		snprintf(resp, sizeof(resp), "{\"status\":0,\"data\":\"pong\"}\n");

	} else if (strstr(req, "\"policy-add\"")) {
		/*
		 * Parse key fields from the JSON request.
		 * Format: {"cmd":"policy-add","args":{...}}
		 */
		struct bdfs_policy_rule rule;
		memset(&rule, 0, sizeof(rule));
		rule.compression = BDFS_COMPRESS_ZSTD;

		/* Extract partition UUID */
		char *p = strstr(req, "\"partition\":\"");
		if (p) {
			p += strlen("\"partition\":\"");
			char uuid_str[37] = {0};
			strncpy(uuid_str, p, 36);
			/* bdfs_str_to_uuid not available here; store raw */
			/* In production use a real JSON parser */
		}

		/* Extract age_days */
		p = strstr(req, "\"age_days\":");
		if (p) rule.age_days = (uint32_t)atoi(p + strlen("\"age_days\":"));

		/* Extract min_size_bytes */
		p = strstr(req, "\"min_size_bytes\":");
		if (p) rule.min_size_bytes =
			(uint64_t)strtoull(p + strlen("\"min_size_bytes\":"),
					   NULL, 10);

		/* Extract name_pattern */
		p = strstr(req, "\"name_pattern\":\"");
		if (p) {
			p += strlen("\"name_pattern\":\"");
			char *end = strchr(p, '"');
			if (end) {
				size_t len = (size_t)(end - p);
				if (len >= sizeof(rule.name_pattern))
					len = sizeof(rule.name_pattern) - 1;
				strncpy(rule.name_pattern, p, len);
			}
		}

		rule.readonly           = !!strstr(req, "\"readonly\":true");
		rule.delete_after_demote = !!strstr(req, "\"delete_after_demote\":true");
		rule.enabled            = true;

		if (d->policy && rule.age_days > 0) {
			uint64_t id = bdfs_policy_add_rule(d->policy, &rule);
			snprintf(resp, sizeof(resp),
				 "{\"status\":0,\"data\":{\"rule_id\":%llu}}\n",
				 (unsigned long long)id);
		} else {
			snprintf(resp, sizeof(resp),
				 "{\"status\":-22,\"error\":"
				 "\"invalid rule or policy engine not running\"}\n");
		}

	} else if (strstr(req, "\"policy-remove\"")) {
		uint64_t rule_id = 0;
		char *p = strstr(req, "\"rule_id\":");
		if (p) rule_id = (uint64_t)strtoull(
				p + strlen("\"rule_id\":"), NULL, 10);

		if (d->policy && rule_id > 0) {
			int r = bdfs_policy_remove_rule(d->policy, rule_id);
			snprintf(resp, sizeof(resp),
				 "{\"status\":%d}\n", r);
		} else {
			snprintf(resp, sizeof(resp),
				 "{\"status\":-22,\"error\":\"invalid rule_id\"}\n");
		}

	} else if (strstr(req, "\"policy-list\"")) {
		if (d->policy) {
			struct bdfs_policy_rule rules[64];
			uint32_t count = 0;
			bdfs_policy_list_rules(d->policy, rules, 64, &count);
			char *pos = resp;
			size_t rem = sizeof(resp);
			int written = snprintf(pos, rem,
				"{\"status\":0,\"data\":{\"rules\":[");
			pos += written; rem -= written;
			for (uint32_t i = 0; i < count && rem > 2; i++) {
				written = snprintf(pos, rem,
					"%s{\"id\":%llu,\"age_days\":%u,"
					"\"pattern\":\"%s\","
					"\"compression\":\"%s\","
					"\"delete_after\":%s,"
					"\"enabled\":%s}",
					i ? "," : "",
					(unsigned long long)rules[i].rule_id,
					rules[i].age_days,
					rules[i].name_pattern,
					/* compression name not available here */
					"zstd",
					rules[i].delete_after_demote ? "true":"false",
					rules[i].enabled ? "true" : "false");
				pos += written; rem -= written;
			}
			snprintf(pos, rem, "]}}\n");
		} else {
			snprintf(resp, sizeof(resp),
				 "{\"status\":0,\"data\":{\"rules\":[]}}\n");
		}

	} else if (strstr(req, "\"policy-scan\"")) {
		if (d->policy) {
			int demoted = bdfs_policy_scan(d->policy);
			snprintf(resp, sizeof(resp),
				 "{\"status\":0,\"data\":"
				 "{\"demotes_queued\":%d}}\n", demoted);
		} else {
			snprintf(resp, sizeof(resp),
				 "{\"status\":-1,\"error\":"
				 "\"policy engine not running\"}\n");
		}

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
