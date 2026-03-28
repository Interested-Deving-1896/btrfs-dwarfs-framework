// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_daemon.c - BTRFS+DwarFS userspace daemon
 *
 * Main daemon entry point.  Initialises the control device connection,
 * netlink listener, Unix socket server, and worker thread pool, then
 * enters the event loop.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>

#include "bdfs_daemon.h"
#include "bdfs_policy.h"

#define BDFS_DEFAULT_CTL_DEVICE  "/dev/bdfs_ctl"
#define BDFS_DEFAULT_SOCKET_PATH "/run/bdfs/daemon.sock"
#define BDFS_DEFAULT_STATE_DIR   "/var/lib/bdfs"
#define BDFS_DEFAULT_WORKERS     4
#define BDFS_NETLINK_PROTO       31

static volatile sig_atomic_t g_shutdown = 0;

static void signal_handler(int sig)
{
	(void)sig;
	g_shutdown = 1;
}

/* ── Worker thread pool ─────────────────────────────────────────────────── */

static void *worker_thread(void *arg)
{
	struct bdfs_daemon *d = arg;

	while (1) {
		struct bdfs_job *job = NULL;
		int result;

		pthread_mutex_lock(&d->queue_lock);
		while (TAILQ_EMPTY(&d->job_queue) && !d->shutdown)
			pthread_cond_wait(&d->queue_cond, &d->queue_lock);

		if (d->shutdown && TAILQ_EMPTY(&d->job_queue)) {
			pthread_mutex_unlock(&d->queue_lock);
			break;
		}

		job = TAILQ_FIRST(&d->job_queue);
		TAILQ_REMOVE(&d->job_queue, job, entry);
		pthread_mutex_unlock(&d->queue_lock);

		/* Dispatch to the appropriate handler */
		switch (job->type) {
		case BDFS_JOB_EXPORT_TO_DWARFS:
			result = bdfs_job_export_to_dwarfs(d, job);
			break;
		case BDFS_JOB_IMPORT_FROM_DWARFS:
			result = bdfs_job_import_from_dwarfs(d, job);
			break;
		case BDFS_JOB_MOUNT_DWARFS:
			result = bdfs_job_mount_dwarfs(d, job);
			break;
		case BDFS_JOB_UMOUNT_DWARFS:
			result = bdfs_job_umount_dwarfs(d, job);
			break;
		case BDFS_JOB_STORE_IMAGE:
			result = bdfs_job_store_image(d, job);
			break;
		case BDFS_JOB_SNAPSHOT_CONTAINER:
			result = bdfs_job_snapshot_container(d, job);
			break;
		case BDFS_JOB_MOUNT_BLEND:
			result = bdfs_job_mount_blend(d, job);
			break;
		case BDFS_JOB_UMOUNT_BLEND:
			result = bdfs_job_umount_blend(d, job);
			break;
		case BDFS_JOB_PROMOTE_COPYUP:
			result = bdfs_job_promote_copyup(d, job);
			break;
		default:
			syslog(LOG_WARNING, "bdfs: unknown job type %d", job->type);
			result = -EINVAL;
			break;
		}

		if (job->on_complete)
			job->on_complete(job, result);

		bdfs_job_free(job);
	}

	return NULL;
}

/* ── Daemon initialisation ──────────────────────────────────────────────── */

int bdfs_daemon_init(struct bdfs_daemon *d, struct bdfs_daemon_config *cfg)
{
	int i, ret;

	memset(d, 0, sizeof(*d));
	memcpy(&d->cfg, cfg, sizeof(*cfg));

	/* Apply defaults */
	if (!d->cfg.ctl_device[0])
		strncpy(d->cfg.ctl_device, BDFS_DEFAULT_CTL_DEVICE,
			sizeof(d->cfg.ctl_device) - 1);
	if (!d->cfg.socket_path[0])
		strncpy(d->cfg.socket_path, BDFS_DEFAULT_SOCKET_PATH,
			sizeof(d->cfg.socket_path) - 1);
	if (!d->cfg.state_dir[0])
		strncpy(d->cfg.state_dir, BDFS_DEFAULT_STATE_DIR,
			sizeof(d->cfg.state_dir) - 1);
	if (!d->cfg.worker_threads)
		d->cfg.worker_threads = BDFS_DEFAULT_WORKERS;
	if (!d->cfg.netlink_proto)
		d->cfg.netlink_proto = BDFS_NETLINK_PROTO;

	/* Locate required binaries */
	if (!d->cfg.mkdwarfs_bin[0])
		strncpy(d->cfg.mkdwarfs_bin, "mkdwarfs",
			sizeof(d->cfg.mkdwarfs_bin) - 1);
	if (!d->cfg.dwarfs_bin[0])
		strncpy(d->cfg.dwarfs_bin, "dwarfs",
			sizeof(d->cfg.dwarfs_bin) - 1);
	if (!d->cfg.dwarfsextract_bin[0])
		strncpy(d->cfg.dwarfsextract_bin, "dwarfsextract",
			sizeof(d->cfg.dwarfsextract_bin) - 1);
	if (!d->cfg.dwarfsck_bin[0])
		strncpy(d->cfg.dwarfsck_bin, "dwarfsck",
			sizeof(d->cfg.dwarfsck_bin) - 1);
	if (!d->cfg.btrfs_bin[0])
		strncpy(d->cfg.btrfs_bin, "btrfs",
			sizeof(d->cfg.btrfs_bin) - 1);

	/* Create state directory */
	if (mkdir(d->cfg.state_dir, 0700) < 0 && errno != EEXIST) {
		syslog(LOG_ERR, "bdfs: cannot create state dir %s: %m",
		       d->cfg.state_dir);
		return -errno;
	}

	/* Open control device */
	d->ctl_fd = open(d->cfg.ctl_device, O_RDWR | O_CLOEXEC);
	if (d->ctl_fd < 0) {
		syslog(LOG_ERR, "bdfs: cannot open %s: %m", d->cfg.ctl_device);
		return -errno;
	}

	/* Initialise job queue and mount table */
	TAILQ_INIT(&d->job_queue);
	TAILQ_INIT(&d->mounts);
	pthread_mutex_init(&d->queue_lock, NULL);
	pthread_cond_init(&d->queue_cond, NULL);
	pthread_mutex_init(&d->mounts_lock, NULL);

	/* Spawn worker threads */
	d->worker_count = d->cfg.worker_threads;
	d->workers = calloc(d->worker_count, sizeof(pthread_t));
	if (!d->workers)
		return -ENOMEM;

	for (i = 0; i < d->worker_count; i++) {
		ret = pthread_create(&d->workers[i], NULL, worker_thread, d);
		if (ret) {
			syslog(LOG_ERR, "bdfs: pthread_create failed: %s",
			       strerror(ret));
			d->worker_count = i;
			return -ret;
		}
	}

	/* Initialise netlink listener */
	ret = bdfs_netlink_init(d);
	if (ret) {
		syslog(LOG_ERR, "bdfs: netlink init failed: %d", ret);
		return ret;
	}

	/* Initialise Unix socket server */
	ret = bdfs_socket_init(d);
	if (ret) {
		syslog(LOG_ERR, "bdfs: socket init failed: %d", ret);
		return ret;
	}

	/* Start the auto-demote policy engine */
	d->policy = calloc(1, sizeof(*d->policy));
	if (d->policy) {
		ret = bdfs_policy_init(d->policy, d);
		if (ret) {
			syslog(LOG_WARNING,
			       "bdfs: policy engine init failed: %d (continuing)",
			       ret);
			free(d->policy);
			d->policy = NULL;
		}
	}

	syslog(LOG_INFO, "bdfs: daemon initialised (%d workers)", d->worker_count);
	return 0;
}

int bdfs_daemon_run(struct bdfs_daemon *d)
{
	fd_set rfds;
	int maxfd;

	syslog(LOG_INFO, "bdfs: entering event loop");

	while (!g_shutdown) {
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

		FD_ZERO(&rfds);
		FD_SET(d->nl_fd, &rfds);
		FD_SET(d->sock_fd, &rfds);
		maxfd = (d->nl_fd > d->sock_fd ? d->nl_fd : d->sock_fd) + 1;

		int ret = select(maxfd, &rfds, NULL, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "bdfs: select error: %m");
			break;
		}

		if (FD_ISSET(d->nl_fd, &rfds))
			bdfs_netlink_loop(d);

		if (FD_ISSET(d->sock_fd, &rfds))
			bdfs_socket_loop(d);
	}

	syslog(LOG_INFO, "bdfs: shutting down");
	bdfs_daemon_shutdown(d);
	return 0;
}

void bdfs_daemon_shutdown(struct bdfs_daemon *d)
{
	int i;

	pthread_mutex_lock(&d->queue_lock);
	d->shutdown = true;
	pthread_cond_broadcast(&d->queue_cond);
	pthread_mutex_unlock(&d->queue_lock);

	for (i = 0; i < d->worker_count; i++)
		pthread_join(d->workers[i], NULL);

	free(d->workers);

	if (d->ctl_fd >= 0)
		close(d->ctl_fd);
	if (d->nl_fd >= 0)
		close(d->nl_fd);
	if (d->sock_fd >= 0) {
		close(d->sock_fd);
		unlink(d->cfg.socket_path);
	}

	if (d->policy) {
		bdfs_policy_shutdown(d->policy);
		free(d->policy);
		d->policy = NULL;
	}

	/* Free mount table entries (mounts were already unmounted by jobs) */
	pthread_mutex_lock(&d->mounts_lock);
	{
		struct bdfs_mount_entry *me, *tmp;
		TAILQ_FOREACH_SAFE(me, &d->mounts, entry, tmp) {
			TAILQ_REMOVE(&d->mounts, me, entry);
			free(me);
		}
	}
	pthread_mutex_unlock(&d->mounts_lock);

	pthread_mutex_destroy(&d->queue_lock);
	pthread_cond_destroy(&d->queue_cond);
	pthread_mutex_destroy(&d->mounts_lock);
}

/* ── Mount table helpers ────────────────────────────────────────────────── */

void bdfs_mount_track(struct bdfs_daemon *d, enum bdfs_mount_type type,
		      const uint8_t uuid[16], uint64_t image_id,
		      const char *mount_point)
{
	struct bdfs_mount_entry *me = calloc(1, sizeof(*me));
	if (!me)
		return;

	me->type     = type;
	me->image_id = image_id;
	if (uuid)
		memcpy(me->partition_uuid, uuid, 16);
	strncpy(me->mount_point, mount_point, sizeof(me->mount_point) - 1);

	pthread_mutex_lock(&d->mounts_lock);
	TAILQ_INSERT_TAIL(&d->mounts, me, entry);
	pthread_mutex_unlock(&d->mounts_lock);
}

void bdfs_mount_untrack(struct bdfs_daemon *d, const char *mount_point)
{
	struct bdfs_mount_entry *me, *tmp;

	pthread_mutex_lock(&d->mounts_lock);
	TAILQ_FOREACH_SAFE(me, &d->mounts, entry, tmp) {
		if (strcmp(me->mount_point, mount_point) == 0) {
			TAILQ_REMOVE(&d->mounts, me, entry);
			free(me);
			break;
		}
	}
	pthread_mutex_unlock(&d->mounts_lock);
}

int bdfs_mount_count(struct bdfs_daemon *d)
{
	struct bdfs_mount_entry *me;
	int n = 0;

	pthread_mutex_lock(&d->mounts_lock);
	TAILQ_FOREACH(me, &d->mounts, entry)
		n++;
	pthread_mutex_unlock(&d->mounts_lock);
	return n;
}

/* ── Job allocation ─────────────────────────────────────────────────────── */

struct bdfs_job *bdfs_job_alloc(enum bdfs_job_type type)
{
	struct bdfs_job *job = calloc(1, sizeof(*job));
	if (job)
		job->type = type;
	return job;
}

void bdfs_job_free(struct bdfs_job *job)
{
	free(job);
}

int bdfs_daemon_enqueue(struct bdfs_daemon *d, struct bdfs_job *job)
{
	pthread_mutex_lock(&d->queue_lock);
	TAILQ_INSERT_TAIL(&d->job_queue, job, entry);
	pthread_cond_signal(&d->queue_cond);
	pthread_mutex_unlock(&d->queue_lock);
	return 0;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	struct bdfs_daemon daemon;
	struct bdfs_daemon_config cfg;
	int opt, ret;

	memset(&cfg, 0, sizeof(cfg));
	cfg.daemonize = true;

	while ((opt = getopt(argc, argv, "fc:s:d:j:v")) != -1) {
		switch (opt) {
		case 'f':
			cfg.daemonize = false;
			break;
		case 'c':
			strncpy(cfg.ctl_device, optarg,
				sizeof(cfg.ctl_device) - 1);
			break;
		case 's':
			strncpy(cfg.socket_path, optarg,
				sizeof(cfg.socket_path) - 1);
			break;
		case 'd':
			strncpy(cfg.state_dir, optarg,
				sizeof(cfg.state_dir) - 1);
			break;
		case 'j':
			cfg.worker_threads = atoi(optarg);
			break;
		case 'v':
			cfg.verbose = true;
			break;
		default:
			fprintf(stderr,
				"Usage: %s [-f] [-c ctl_dev] [-s sock] "
				"[-d state_dir] [-j threads] [-v]\n",
				argv[0]);
			return 1;
		}
	}

	openlog("bdfs_daemon", LOG_PID | LOG_CONS, LOG_DAEMON);

	if (cfg.daemonize) {
		if (daemon(0, 0) < 0) {
			perror("daemon");
			return 1;
		}
	}

	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGPIPE, SIG_IGN);

	ret = bdfs_daemon_init(&daemon, &cfg);
	if (ret) {
		syslog(LOG_ERR, "bdfs: init failed: %d", ret);
		return 1;
	}

	ret = bdfs_daemon_run(&daemon);
	closelog();
	return ret ? 1 : 0;
}
