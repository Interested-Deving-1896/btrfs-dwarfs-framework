// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_netlink.c - Netlink event listener
 *
 * Receives bdfs_event messages from the kernel module and translates them
 * into bdfs_job entries dispatched to the worker pool.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#include "bdfs_daemon.h"

#define BDFS_NETLINK_PROTO  31
#define BDFS_NL_GROUP       1
#define BDFS_NL_BUFSIZE     8192

int bdfs_netlink_init(struct bdfs_daemon *d)
{
	struct sockaddr_nl addr;
	int fd;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
		    d->cfg.netlink_proto);
	if (fd < 0) {
		syslog(LOG_ERR, "bdfs: netlink socket: %m");
		return -errno;
	}

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid    = 0;
	addr.nl_groups = BDFS_NL_GROUP;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		syslog(LOG_ERR, "bdfs: netlink bind: %m");
		close(fd);
		return -errno;
	}

	d->nl_fd = fd;
	syslog(LOG_INFO, "bdfs: netlink listener ready (proto=%d group=%d)",
	       d->cfg.netlink_proto, BDFS_NL_GROUP);
	return 0;
}

/*
 * Parse a kernel event message and dispatch the appropriate job.
 * The event message field carries a space-separated key=value string
 * with the parameters needed to construct the job.
 */
static void bdfs_handle_event(struct bdfs_daemon *d,
			      const struct bdfs_event *evt)
{
	struct bdfs_job *job = NULL;

	switch (evt->type) {
	case BDFS_EVT_SNAPSHOT_EXPORTED: {
		/*
		 * Kernel has registered an export intent.  Parse the message
		 * to extract subvol_id, image path, and compression.
		 *
		 * Message format:
		 *   "export subvol=<id> image=<name> compression=<n>"
		 */
		uint64_t subvol_id = 0;
		char image_name[BDFS_NAME_MAX + 1] = {0};
		uint32_t compression = BDFS_COMPRESS_ZSTD;

		sscanf(evt->message,
		       "export subvol=%llu image=%255s compression=%u",
		       &subvol_id, image_name, &compression);

		job = bdfs_job_alloc(BDFS_JOB_EXPORT_TO_DWARFS);
		if (!job) break;

		memcpy(job->partition_uuid, evt->partition_uuid, 16);
		job->object_id = evt->object_id;
		job->export_to_dwarfs.subvol_id = subvol_id;
		job->export_to_dwarfs.compression = compression;
		strncpy(job->export_to_dwarfs.image_name, image_name,
			sizeof(job->export_to_dwarfs.image_name) - 1);
		break;
	}

	case BDFS_EVT_IMAGE_MOUNTED: {
		/*
		 * Message format:
		 *   "mount image_id=<id> path=<path> mount=<mnt> cache_mb=<n>"
		 */
		uint64_t image_id = 0;
		char path[BDFS_PATH_MAX] = {0};
		char mount[BDFS_PATH_MAX] = {0};
		uint32_t cache_mb = 256;

		sscanf(evt->message,
		       "mount image_id=%llu path=%4095s mount=%4095s cache_mb=%u",
		       &image_id, path, mount, &cache_mb);

		job = bdfs_job_alloc(BDFS_JOB_MOUNT_DWARFS);
		if (!job) break;

		memcpy(job->partition_uuid, evt->partition_uuid, 16);
		job->object_id = image_id;
		strncpy(job->mount_dwarfs.image_path, path,
			sizeof(job->mount_dwarfs.image_path) - 1);
		strncpy(job->mount_dwarfs.mount_point, mount,
			sizeof(job->mount_dwarfs.mount_point) - 1);
		job->mount_dwarfs.cache_size_mb = cache_mb;
		break;
	}

	case BDFS_EVT_IMAGE_UNMOUNTED: {
		job = bdfs_job_alloc(BDFS_JOB_UMOUNT_DWARFS);
		if (!job) break;
		/* mount_point was stored in the image descriptor; the kernel
		 * passes it in the message field for unmount events */
		strncpy(job->umount_dwarfs.mount_point, evt->message,
			sizeof(job->umount_dwarfs.mount_point) - 1);
		break;
	}

	case BDFS_EVT_IMAGE_IMPORTED: {
		/*
		 * Message format (store):
		 *   "store src=<path> dest=<path> flags=0x<n>"
		 * Message format (import):
		 *   "import image_id=<id> subvol=<name> btrfs=<mnt> flags=0x<n>"
		 */
		if (strncmp(evt->message, "store", 5) == 0) {
			char src[BDFS_PATH_MAX] = {0};
			char dest[BDFS_PATH_MAX] = {0};
			uint32_t flags = 0;

			sscanf(evt->message,
			       "store src=%4095s dest=%4095s flags=0x%x",
			       src, dest, &flags);

			job = bdfs_job_alloc(BDFS_JOB_STORE_IMAGE);
			if (!job) break;

			strncpy(job->store_image.source_path, src,
				sizeof(job->store_image.source_path) - 1);
			strncpy(job->store_image.dest_path, dest,
				sizeof(job->store_image.dest_path) - 1);
			job->store_image.flags = flags;
		} else {
			uint64_t image_id = 0;
			char subvol[BDFS_NAME_MAX + 1] = {0};
			char btrfs_mnt[BDFS_PATH_MAX] = {0};
			uint32_t flags = 0;

			sscanf(evt->message,
			       "import image_id=%llu subvol=%255s btrfs=%4095s flags=0x%x",
			       &image_id, subvol, btrfs_mnt, &flags);

			job = bdfs_job_alloc(BDFS_JOB_IMPORT_FROM_DWARFS);
			if (!job) break;

			memcpy(job->partition_uuid, evt->partition_uuid, 16);
			job->object_id = image_id;
			strncpy(job->import_from_dwarfs.subvol_name, subvol,
				sizeof(job->import_from_dwarfs.subvol_name) - 1);
			strncpy(job->import_from_dwarfs.btrfs_mount, btrfs_mnt,
				sizeof(job->import_from_dwarfs.btrfs_mount) - 1);
			job->import_from_dwarfs.flags = flags;
		}
		break;
	}

	case BDFS_EVT_SNAPSHOT_CREATED: {
		/*
		 * Message format:
		 *   "snapshot image_id=<id> snap=<name> readonly=<0|1>"
		 */
		uint64_t image_id = 0;
		char snap_name[BDFS_NAME_MAX + 1] = {0};
		int readonly = 0;

		sscanf(evt->message,
		       "snapshot image_id=%llu snap=%255s readonly=%d",
		       &image_id, snap_name, &readonly);

		job = bdfs_job_alloc(BDFS_JOB_SNAPSHOT_CONTAINER);
		if (!job) break;

		memcpy(job->partition_uuid, evt->partition_uuid, 16);
		job->object_id = image_id;
		job->snapshot_container.flags =
			readonly ? BDFS_SNAP_READONLY : 0;
		strncpy(job->snapshot_container.snapshot_path, snap_name,
			sizeof(job->snapshot_container.snapshot_path) - 1);
		break;
	}

	case BDFS_EVT_BLEND_MOUNTED: {
		job = bdfs_job_alloc(BDFS_JOB_MOUNT_BLEND);
		if (!job) break;
		/* blend mount details are in the message */
		break;
	}

	case BDFS_EVT_PARTITION_ADDED:
	case BDFS_EVT_PARTITION_REMOVED:
		syslog(LOG_INFO, "bdfs: partition event type=%u", evt->type);
		return;

	case BDFS_EVT_ERROR:
		syslog(LOG_ERR, "bdfs: kernel error event: %s", evt->message);
		return;

	default:
		syslog(LOG_WARNING, "bdfs: unknown event type %u", evt->type);
		return;
	}

	if (job)
		bdfs_daemon_enqueue(d, job);
}

void bdfs_netlink_loop(struct bdfs_daemon *d)
{
	char buf[BDFS_NL_BUFSIZE];
	struct sockaddr_nl src_addr;
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr msg = {
		.msg_name    = &src_addr,
		.msg_namelen = sizeof(src_addr),
		.msg_iov     = &iov,
		.msg_iovlen  = 1,
	};
	ssize_t len;

	len = recvmsg(d->nl_fd, &msg, MSG_DONTWAIT);
	if (len < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			syslog(LOG_ERR, "bdfs: netlink recvmsg: %m");
		return;
	}

	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	while (NLMSG_OK(nlh, (unsigned int)len)) {
		if (nlh->nlmsg_type == NLMSG_DONE)
			break;
		if (nlh->nlmsg_type != NLMSG_ERROR) {
			struct bdfs_event *evt = NLMSG_DATA(nlh);
			bdfs_handle_event(d, evt);
		}
		nlh = NLMSG_NEXT(nlh, len);
	}
}
