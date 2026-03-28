/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bdfs_daemon.h - BTRFS+DwarFS userspace daemon internal API
 *
 * The daemon bridges the kernel module and the userspace tools
 * (mkdwarfs, dwarfs, dwarfsextract, btrfs-progs).  It:
 *
 *   1. Listens on the bdfs netlink socket for kernel-emitted events.
 *   2. Executes the appropriate tool pipeline for each event.
 *   3. Reports completion back to the kernel via /dev/bdfs_ctl ioctls.
 *   4. Manages FUSE mount lifecycles for DwarFS images.
 *   5. Exposes a Unix domain socket for the bdfs CLI tool.
 */

#ifndef _BDFS_DAEMON_H
#define _BDFS_DAEMON_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/queue.h>

#include "../../include/uapi/bdfs_ioctl.h"

/* Daemon configuration */
struct bdfs_daemon_config {
	char    ctl_device[256];        /* default: /dev/bdfs_ctl */
	char    socket_path[256];       /* default: /run/bdfs/daemon.sock */
	char    state_dir[256];         /* default: /var/lib/bdfs */
	char    mkdwarfs_bin[256];
	char    dwarfs_bin[256];
	char    dwarfsextract_bin[256];
	char    dwarfsck_bin[256];
	char    btrfs_bin[256];
	int     worker_threads;
	int     netlink_proto;
	bool    daemonize;
	bool    verbose;
};

/* Job types dispatched to the worker pool */
enum bdfs_job_type {
	BDFS_JOB_EXPORT_TO_DWARFS,     /* btrfs-send | mkdwarfs */
	BDFS_JOB_IMPORT_FROM_DWARFS,   /* dwarfsextract | btrfs receive */
	BDFS_JOB_MOUNT_DWARFS,         /* dwarfs <image> <mnt> */
	BDFS_JOB_UMOUNT_DWARFS,        /* fusermount -u <mnt> */
	BDFS_JOB_STORE_IMAGE,          /* copy_file_range to btrfs */
	BDFS_JOB_SNAPSHOT_CONTAINER,   /* btrfs subvolume snapshot */
	BDFS_JOB_MOUNT_BLEND,          /* mount -t bdfs_blend */
	BDFS_JOB_UMOUNT_BLEND,         /* umount blend point */
};

/* A unit of work dispatched to the thread pool */
struct bdfs_job {
	TAILQ_ENTRY(bdfs_job)   entry;
	enum bdfs_job_type      type;
	uint8_t                 partition_uuid[16];
	uint64_t                object_id;          /* image_id or subvol_id */

	union {
		struct {
			char    btrfs_mount[BDFS_PATH_MAX];
			uint64_t subvol_id;
			char    image_path[BDFS_PATH_MAX];
			char    image_name[BDFS_NAME_MAX + 1];
			uint32_t compression;
			uint32_t block_size_bits;
			uint32_t worker_threads;
			uint32_t flags;
		} export_to_dwarfs;

		struct {
			char    image_path[BDFS_PATH_MAX];
			char    btrfs_mount[BDFS_PATH_MAX];
			char    subvol_name[BDFS_NAME_MAX + 1];
			uint32_t flags;
		} import_from_dwarfs;

		struct {
			char    image_path[BDFS_PATH_MAX];
			char    mount_point[BDFS_PATH_MAX];
			uint32_t cache_size_mb;
			uint32_t flags;
		} mount_dwarfs;

		struct {
			char    mount_point[BDFS_PATH_MAX];
			uint32_t flags;
		} umount_dwarfs;

		struct {
			char    source_path[BDFS_PATH_MAX];
			char    dest_path[BDFS_PATH_MAX];
			uint32_t flags;
		} store_image;

		struct {
			char    subvol_path[BDFS_PATH_MAX];
			char    snapshot_path[BDFS_PATH_MAX];
			uint32_t flags;
		} snapshot_container;

		struct {
			char    btrfs_mount[BDFS_PATH_MAX];
			char    dwarfs_mount[BDFS_PATH_MAX];
			char    blend_mount[BDFS_PATH_MAX];
			struct bdfs_mount_opts opts;
		} mount_blend;
	};

	/* Completion callback (called from worker thread) */
	void (*on_complete)(struct bdfs_job *job, int result);
	void *cb_data;
};

TAILQ_HEAD(bdfs_job_queue, bdfs_job);

/* Daemon global state */
struct bdfs_daemon {
	struct bdfs_daemon_config   cfg;
	int                         ctl_fd;         /* /dev/bdfs_ctl */
	int                         nl_fd;          /* netlink socket */
	int                         sock_fd;        /* unix domain socket */

	/* Worker thread pool */
	pthread_t                  *workers;
	int                         worker_count;
	struct bdfs_job_queue       job_queue;
	pthread_mutex_t             queue_lock;
	pthread_cond_t              queue_cond;
	bool                        shutdown;

	/* Active FUSE mounts tracked by the daemon */
	pthread_mutex_t             mounts_lock;
	struct bdfs_mount_table    *mounts;
};

/* ── Function declarations ─────────────────────────────────────────────── */

/* daemon lifecycle */
int  bdfs_daemon_init(struct bdfs_daemon *d, struct bdfs_daemon_config *cfg);
int  bdfs_daemon_run(struct bdfs_daemon *d);
void bdfs_daemon_shutdown(struct bdfs_daemon *d);

/* job dispatch */
int  bdfs_daemon_enqueue(struct bdfs_daemon *d, struct bdfs_job *job);
struct bdfs_job *bdfs_job_alloc(enum bdfs_job_type type);
void bdfs_job_free(struct bdfs_job *job);

/* job handlers (implemented in bdfs_jobs.c) */
int bdfs_job_export_to_dwarfs(struct bdfs_daemon *d, struct bdfs_job *job);
int bdfs_job_import_from_dwarfs(struct bdfs_daemon *d, struct bdfs_job *job);
int bdfs_job_mount_dwarfs(struct bdfs_daemon *d, struct bdfs_job *job);
int bdfs_job_umount_dwarfs(struct bdfs_daemon *d, struct bdfs_job *job);
int bdfs_job_store_image(struct bdfs_daemon *d, struct bdfs_job *job);
int bdfs_job_snapshot_container(struct bdfs_daemon *d, struct bdfs_job *job);
int bdfs_job_mount_blend(struct bdfs_daemon *d, struct bdfs_job *job);

/* netlink event listener (bdfs_netlink.c) */
int  bdfs_netlink_init(struct bdfs_daemon *d);
void bdfs_netlink_loop(struct bdfs_daemon *d);

/* unix socket server for CLI (bdfs_socket.c) */
int  bdfs_socket_init(struct bdfs_daemon *d);
void bdfs_socket_loop(struct bdfs_daemon *d);

/* tool execution helpers (bdfs_exec.c) */
int bdfs_exec_mkdwarfs(struct bdfs_daemon *d,
		       const char *input_dir,
		       const char *output_image,
		       uint32_t compression,
		       uint32_t block_size_bits,
		       int worker_threads);

int bdfs_exec_dwarfsextract(struct bdfs_daemon *d,
			    const char *image_path,
			    const char *output_dir);

int bdfs_exec_dwarfs_mount(struct bdfs_daemon *d,
			   const char *image_path,
			   const char *mount_point,
			   uint32_t cache_mb);

int bdfs_exec_dwarfs_umount(struct bdfs_daemon *d,
			    const char *mount_point);

int bdfs_exec_btrfs_send(struct bdfs_daemon *d,
			 const char *subvol_path,
			 int *pipe_read_fd_out);

int bdfs_exec_btrfs_receive(struct bdfs_daemon *d,
			    const char *dest_dir,
			    int pipe_write_fd);

int bdfs_exec_btrfs_snapshot(struct bdfs_daemon *d,
			     const char *source_subvol,
			     const char *dest_path,
			     bool readonly);

int bdfs_exec_btrfs_subvol_create(struct bdfs_daemon *d,
				  const char *path);

int bdfs_exec_btrfs_subvol_delete(struct bdfs_daemon *d,
				  const char *path);

#endif /* _BDFS_DAEMON_H */
