// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_jobs.c - Job handler implementations
 *
 * Each handler runs in a worker thread and performs the actual filesystem
 * operations by calling the exec helpers and btrfs ioctls.
 *
 * Export pipeline (BTRFS subvolume → DwarFS image):
 *
 *   1. Create a read-only BTRFS snapshot of the source subvolume.
 *   2. Run `btrfs send` on the snapshot into a temporary directory via
 *      `btrfs receive` to get a clean extracted tree.
 *   3. Run `mkdwarfs` on the extracted tree to produce the .dwarfs image.
 *   4. Move the image to the backing store path.
 *   5. Delete the temporary snapshot and extracted tree.
 *   6. Notify the kernel module of completion via ioctl.
 *
 * Import pipeline (DwarFS image → BTRFS subvolume):
 *
 *   1. Create a new BTRFS subvolume at the target path.
 *   2. Run `dwarfsextract` into the subvolume directory.
 *   3. Optionally make the subvolume read-only.
 *   4. Notify the kernel module of completion.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/time.h>

#include "bdfs_daemon.h"

/* ── Export: BTRFS subvolume → DwarFS image ─────────────────────────────── */

int bdfs_job_export_to_dwarfs(struct bdfs_daemon *d, struct bdfs_job *job)
{
	const struct bdfs_job *j = job;
	char snap_path[BDFS_PATH_MAX];
	char extract_dir[BDFS_PATH_MAX];
	char tmp_image[BDFS_PATH_MAX];
	int ret;

	/*
	 * Step 1: Create a read-only snapshot of the source subvolume.
	 * This ensures the DwarFS image captures a consistent point-in-time
	 * view even if the source subvolume is being written to.
	 */
	snprintf(snap_path, sizeof(snap_path), "%s/.bdfs_snap_%" PRIu64 "",
		 j->export_to_dwarfs.btrfs_mount,
		 j->export_to_dwarfs.subvol_id);

	ret = bdfs_exec_btrfs_snapshot(d,
		/* source */ j->export_to_dwarfs.btrfs_mount,
		/* dest   */ snap_path,
		/* ro     */ true);
	if (ret) {
		syslog(LOG_ERR, "bdfs: export: snapshot failed: %d", ret);
		return ret;
	}

	/*
	 * Step 2: Extract the snapshot tree to a temporary directory.
	 * We use btrfs send | btrfs receive to get a clean POSIX tree
	 * that mkdwarfs can process without needing btrfs-specific ioctls.
	 */
	snprintf(extract_dir, sizeof(extract_dir), "%s/.bdfs_extract_%" PRIu64 "",
		 d->cfg.state_dir, j->export_to_dwarfs.subvol_id);

	if (mkdir(extract_dir, 0700) < 0 && errno != EEXIST) {
		syslog(LOG_ERR, "bdfs: export: mkdir %s: %m", extract_dir);
		bdfs_exec_btrfs_subvol_delete(d, snap_path);
		return -errno;
	}

	{
		int pipe_fd;
		pid_t send_pid;
		const char *parent = NULL;

		/*
		 * Incremental export: pass the parent snapshot path to
		 * btrfs-send via -p so only the delta is streamed.
		 * The parent_snap_path field is populated by the netlink
		 * event parser when BDFS_EXPORT_INCREMENTAL is set.
		 */
		if ((j->export_to_dwarfs.flags & BDFS_EXPORT_INCREMENTAL) &&
		    j->export_to_dwarfs.parent_snap_path[0])
			parent = j->export_to_dwarfs.parent_snap_path;

		send_pid = bdfs_exec_btrfs_send_incremental(d, snap_path,
							    parent, &pipe_fd);
		if (send_pid < 0) {
			syslog(LOG_ERR, "bdfs: export: btrfs send failed: %d",
			       send_pid);
			bdfs_exec_btrfs_subvol_delete(d, snap_path);
			return send_pid;
		}

		ret = bdfs_exec_btrfs_receive(d, extract_dir, pipe_fd);
		close(pipe_fd);

		if (ret) {
			syslog(LOG_ERR, "bdfs: export: btrfs receive failed: %d",
			       ret);
			bdfs_exec_btrfs_subvol_delete(d, snap_path);
			return ret;
		}
	}

	/*
	 * Step 3: Build the DwarFS image from the extracted tree.
	 * mkdwarfs uses similarity hashing and segmentation analysis to
	 * maximise compression across file boundaries.
	 */
	snprintf(tmp_image, sizeof(tmp_image), "%s/%s.dwarfs.tmp",
		 d->cfg.state_dir, j->export_to_dwarfs.image_name);

	ret = bdfs_exec_mkdwarfs(d,
		extract_dir,
		tmp_image,
		j->export_to_dwarfs.compression,
		j->export_to_dwarfs.block_size_bits,
		(int)j->export_to_dwarfs.worker_threads);
	if (ret) {
		syslog(LOG_ERR, "bdfs: export: mkdwarfs failed: %d", ret);
		goto cleanup;
	}

	/*
	 * Step 4: Atomically move the image to its final backing path.
	 * rename(2) is atomic on the same filesystem, so the backing store
	 * never sees a partial image.
	 */
	if (rename(tmp_image, j->export_to_dwarfs.image_path) < 0) {
		syslog(LOG_ERR, "bdfs: export: rename %s → %s: %m",
		       tmp_image, j->export_to_dwarfs.image_path);
		ret = -errno;
		goto cleanup;
	}

	syslog(LOG_INFO, "bdfs: export complete: subvol %" PRIu64 " → %s",
	       j->export_to_dwarfs.subvol_id,
	       j->export_to_dwarfs.image_path);
	ret = 0;

cleanup:
	/* Step 5: Clean up temporary snapshot and extracted tree */
	bdfs_exec_btrfs_subvol_delete(d, snap_path);
	/* Remove extracted tree (non-subvolume directory) */
	{
		char rm_cmd[BDFS_PATH_MAX + 8];
		snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", extract_dir);
		system(rm_cmd); /* best-effort cleanup */
	}
	if (ret && access(tmp_image, F_OK) == 0)
		unlink(tmp_image);

	return ret;
}

/* ── Import: DwarFS image → BTRFS subvolume ─────────────────────────────── */

int bdfs_job_import_from_dwarfs(struct bdfs_daemon *d, struct bdfs_job *job)
{
	const struct bdfs_job *j = job;
	char subvol_path[BDFS_PATH_MAX];
	int ret;

	snprintf(subvol_path, sizeof(subvol_path), "%s/%s",
		 j->import_from_dwarfs.btrfs_mount,
		 j->import_from_dwarfs.subvol_name);

	/*
	 * Step 1: Create a new BTRFS subvolume to receive the extracted data.
	 * Using a subvolume (rather than a plain directory) means the imported
	 * data can later be snapshotted, sent, or demoted back to DwarFS.
	 */
	ret = bdfs_exec_btrfs_subvol_create(d, subvol_path);
	if (ret) {
		syslog(LOG_ERR, "bdfs: import: subvol create %s failed: %d",
		       subvol_path, ret);
		return ret;
	}

	/*
	 * Step 2: Extract the DwarFS image into the subvolume.
	 * dwarfsextract preserves all POSIX metadata (permissions, timestamps,
	 * xattrs) stored in the DwarFS image.
	 */
	ret = bdfs_exec_dwarfsextract(d,
		j->import_from_dwarfs.image_path,
		subvol_path);
	if (ret) {
		syslog(LOG_ERR, "bdfs: import: dwarfsextract failed: %d", ret);
		bdfs_exec_btrfs_subvol_delete(d, subvol_path);
		return ret;
	}

	/*
	 * Step 3: Optionally make the subvolume read-only.
	 * A read-only subvolume can be used as a base for CoW snapshots
	 * without risk of accidental modification.
	 */
	if (j->import_from_dwarfs.flags & BDFS_IMPORT_READONLY) {
		const char *argv[] = {
			d->cfg.btrfs_bin, "property", "set",
			"-ts", subvol_path, "ro", "true", NULL
		};
		ret = bdfs_exec_wait(argv);
		if (ret) {
			syslog(LOG_WARNING,
			       "bdfs: import: failed to set subvol ro on %s: %d"
			       " (subvol created but not read-only)",
			       subvol_path, ret);
			/* Non-fatal: the data was imported successfully;
			 * the caller can set ro manually via btrfs-progs. */
			ret = 0;
		} else {
			syslog(LOG_INFO, "bdfs: import: subvol %s set read-only",
			       subvol_path);
		}
	}

	syslog(LOG_INFO, "bdfs: import complete: %s → subvol %s",
	       j->import_from_dwarfs.image_path, subvol_path);
	return 0;
}

/* ── Mount DwarFS image ─────────────────────────────────────────────────── */

int bdfs_job_mount_dwarfs(struct bdfs_daemon *d, struct bdfs_job *job)
{
	const struct bdfs_job *j = job;
	int ret;

	/* Ensure mount point exists */
	if (mkdir(j->mount_dwarfs.mount_point, 0755) < 0 && errno != EEXIST) {
		syslog(LOG_ERR, "bdfs: mount: mkdir %s: %m",
		       j->mount_dwarfs.mount_point);
		return -errno;
	}

	ret = bdfs_exec_dwarfs_mount(d,
		j->mount_dwarfs.image_path,
		j->mount_dwarfs.mount_point,
		j->mount_dwarfs.cache_size_mb);

	if (ret == 0) {
		syslog(LOG_INFO, "bdfs: mounted %s at %s",
		       j->mount_dwarfs.image_path,
		       j->mount_dwarfs.mount_point);
		bdfs_mount_track(d, BDFS_MNT_DWARFS,
				 j->partition_uuid, j->object_id,
				 j->mount_dwarfs.mount_point);
	}
	return ret;
}

/* ── Unmount DwarFS image ───────────────────────────────────────────────── */

int bdfs_job_umount_dwarfs(struct bdfs_daemon *d, struct bdfs_job *job)
{
	int ret = bdfs_exec_dwarfs_umount(d, job->umount_dwarfs.mount_point);
	if (ret == 0)
		bdfs_mount_untrack(d, job->umount_dwarfs.mount_point);
	return ret;
}

/* ── Store DwarFS image onto BTRFS partition ────────────────────────────── */

/*
 * bdfs_job_store_image - Copy a DwarFS image file to the BTRFS partition.
 *
 * Uses copy_file_range(2) for efficient in-kernel copy that benefits from
 * BTRFS CoW: if source and destination are on the same BTRFS filesystem,
 * the kernel can reflink the data blocks rather than copying them.
 */
int bdfs_job_store_image(struct bdfs_daemon *d, struct bdfs_job *job)
{
	const struct bdfs_job *j = job;
	int src_fd = -1, dst_fd = -1;
	struct stat st;
	off_t offset = 0;
	ssize_t copied;
	int ret = 0;

	(void)d;

	src_fd = open(j->store_image.source_path, O_RDONLY | O_CLOEXEC);
	if (src_fd < 0) {
		syslog(LOG_ERR, "bdfs: store: open %s: %m",
		       j->store_image.source_path);
		return -errno;
	}

	if (fstat(src_fd, &st) < 0) {
		ret = -errno;
		goto out;
	}

	dst_fd = open(j->store_image.dest_path,
		      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (dst_fd < 0) {
		syslog(LOG_ERR, "bdfs: store: create %s: %m",
		       j->store_image.dest_path);
		ret = -errno;
		goto out;
	}

	/*
	 * copy_file_range performs the copy entirely in kernel space.
	 * On BTRFS, if source and dest are on the same filesystem, this
	 * triggers a reflink (CoW extent sharing) rather than a data copy.
	 */
	while (offset < st.st_size) {
		copied = copy_file_range(src_fd, &offset,
					 dst_fd, NULL,
					 (size_t)(st.st_size - offset), 0);
		if (copied < 0) {
			if (errno == EXDEV || errno == EOPNOTSUPP) {
				/* Fall back to sendfile */
				off_t sf_off = offset;
				copied = sendfile(dst_fd, src_fd, &sf_off,
						  (size_t)(st.st_size - offset));
				if (copied < 0) {
					ret = -errno;
					goto out;
				}
				offset = sf_off;
			} else {
				ret = -errno;
				goto out;
			}
		}
	}

	syslog(LOG_INFO, "bdfs: stored %s → %s (%lld bytes)",
	       j->store_image.source_path, j->store_image.dest_path,
	       (long long)st.st_size);

out:
	if (src_fd >= 0) close(src_fd);
	if (dst_fd >= 0) {
		close(dst_fd);
		if (ret)
			unlink(j->store_image.dest_path);
	}
	return ret;
}

/* ── Snapshot BTRFS subvolume containing a DwarFS image ─────────────────── */

int bdfs_job_snapshot_container(struct bdfs_daemon *d, struct bdfs_job *job)
{
	const struct bdfs_job *j = job;
	bool readonly = !!(j->snapshot_container.flags & BDFS_SNAP_READONLY);

	return bdfs_exec_btrfs_snapshot(d,
		j->snapshot_container.subvol_path,
		j->snapshot_container.snapshot_path,
		readonly);
}

/* ── Unmount blend layer ────────────────────────────────────────────────── */

int bdfs_job_umount_blend(struct bdfs_daemon *d, struct bdfs_job *job)
{
	const struct bdfs_job *j = job;
	const char *mnt = j->mount_blend.blend_mount;
	int ret;

	ret = umount2(mnt, MNT_DETACH);
	if (ret < 0) {
		syslog(LOG_ERR, "bdfs: blend umount %s: %m", mnt);
		return -errno;
	}

	syslog(LOG_INFO, "bdfs: blend unmounted: %s", mnt);
	bdfs_mount_untrack(d, mnt);
	return 0;
}

/* ── Copy-up: promote DwarFS file to BTRFS upper layer ──────────────────── */

/*
 * bdfs_job_promote_copyup - Copy a file from a DwarFS lower layer to the
 * BTRFS upper layer, then signal the kernel that the copy-up is complete.
 *
 * Flow:
 *   1. Ensure the parent directory exists on the upper layer.
 *   2. Copy the file using copy_file_range (reflink if same FS, else copy).
 *   3. Preserve permissions and timestamps via stat + utimensat/chmod/chown.
 *   4. Call BDFS_IOC_COPYUP_COMPLETE so the kernel wakes blocked openers.
 */
int bdfs_job_promote_copyup(struct bdfs_daemon *d, struct bdfs_job *job)
{
	const struct bdfs_job *j = job;
	const char *src = j->promote_copyup.lower_path;
	const char *dst = j->promote_copyup.upper_path;
	struct stat st;
	int src_fd = -1, dst_fd = -1;
	off_t offset = 0;
	ssize_t copied;
	int ret = 0;
	char parent[BDFS_PATH_MAX];
	char *slash;

	/*
	 * Step 1: ensure the full parent directory tree exists on the upper
	 * layer.  A single mkdir(2) is not sufficient for nested paths like
	 * "/upper/usr/lib/" when intermediate components are missing.  Walk
	 * each component and create it if absent (mkdir -p semantics).
	 */
	snprintf(parent, sizeof(parent), "%s", dst);
	slash = strrchr(parent, '/');
	if (slash && slash != parent) {
		*slash = '\0';
		/* Recreate every component from the root down */
		for (char *p = parent + 1; *p; p++) {
			if (*p != '/')
				continue;
			*p = '\0';
			if (mkdir(parent, 0755) < 0 && errno != EEXIST) {
				syslog(LOG_ERR,
				       "bdfs: copyup: mkdir %s: %m", parent);
				return -errno;
			}
			*p = '/';
		}
		/* Create the final component */
		if (mkdir(parent, 0755) < 0 && errno != EEXIST) {
			syslog(LOG_ERR, "bdfs: copyup: mkdir %s: %m", parent);
			return -errno;
		}
	}

	/* Step 2: open source (DwarFS FUSE mount) */
	src_fd = open(src, O_RDONLY | O_CLOEXEC);
	if (src_fd < 0) {
		syslog(LOG_ERR, "bdfs: copyup: open src %s: %m", src);
		return -errno;
	}

	if (fstat(src_fd, &st) < 0) {
		ret = -errno;
		goto out;
	}

	/* Step 3: create destination on BTRFS upper layer */
	dst_fd = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
		      st.st_mode & 0777);
	if (dst_fd < 0 && errno == EEXIST) {
		/* Already copied by a concurrent thread — success */
		ret = 0;
		goto out;
	}
	if (dst_fd < 0) {
		syslog(LOG_ERR, "bdfs: copyup: create dst %s: %m", dst);
		ret = -errno;
		goto out;
	}

	/* Step 4: copy data */
	while (offset < st.st_size) {
		copied = copy_file_range(src_fd, &offset,
					 dst_fd, NULL,
					 (size_t)(st.st_size - offset), 0);
		if (copied < 0) {
			if (errno == EXDEV || errno == EOPNOTSUPP) {
				/* Fallback: read/write loop */
				char buf[65536];
				ssize_t n;
				while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
					if (write(dst_fd, buf, (size_t)n) != n) {
						ret = -errno;
						goto out;
					}
				}
				if (n < 0)
					ret = -errno;
				break;
			}
			ret = -errno;
			goto out;
		}
	}

	/* Step 5: preserve ownership and timestamps */
	if (fchown(dst_fd, st.st_uid, st.st_gid) < 0)
		syslog(LOG_WARNING, "bdfs: copyup: chown %s: %m", dst);

	{
		struct timespec times[2] = { st.st_atim, st.st_mtim };
		if (futimens(dst_fd, times) < 0)
			syslog(LOG_WARNING, "bdfs: copyup: utimens %s: %m", dst);
	}

	syslog(LOG_INFO, "bdfs: copyup complete: %s → %s", src, dst);

out:
	if (src_fd >= 0) close(src_fd);
	if (dst_fd >= 0) {
		close(dst_fd);
		if (ret)
			unlink(dst);
	}

	if (ret == 0) {
		/* Step 6: signal the kernel that copy-up is done */
		struct bdfs_ioctl_copyup_complete arg;
		memset(&arg, 0, sizeof(arg));
		memcpy(arg.btrfs_uuid, j->promote_copyup.btrfs_uuid, 16);
		arg.inode_no = j->promote_copyup.inode_no;
		snprintf(arg.upper_path, sizeof(arg.upper_path), "%s", dst);

		if (ioctl(d->ctl_fd, BDFS_IOC_COPYUP_COMPLETE, &arg) < 0) {
			syslog(LOG_ERR,
			       "bdfs: copyup: BDFS_IOC_COPYUP_COMPLETE failed: %m");
			/* Non-fatal: the file is copied; the kernel will
			 * time out and the opener will get -ETIMEDOUT. */
		}
	}

	return ret;
}

/* ── Mount blend layer ──────────────────────────────────────────────────── */

/*
 * bdfs_job_mount_blend - Mount the bdfs_blend filesystem.
 *
 * Constructs the mount options string encoding the BTRFS and DwarFS
 * partition UUIDs, then calls mount(2) with filesystem type "bdfs_blend".
 */
int bdfs_job_mount_blend(struct bdfs_daemon *d, struct bdfs_job *job)
{
	const struct bdfs_job *j = job;
	char opts[512];
	int ret;

	(void)d;

	/* Ensure blend mount point exists */
	if (mkdir(j->mount_blend.blend_mount, 0755) < 0 && errno != EEXIST) {
		syslog(LOG_ERR, "bdfs: blend mount: mkdir %s: %m",
		       j->mount_blend.blend_mount);
		return -errno;
	}

	snprintf(opts, sizeof(opts),
		 "btrfs=%s,dwarfs=%s",
		 j->mount_blend.btrfs_mount,
		 j->mount_blend.dwarfs_mount);

	ret = mount("none", j->mount_blend.blend_mount, "bdfs_blend", 0, opts);
	if (ret < 0) {
		syslog(LOG_ERR, "bdfs: blend mount %s failed: %m",
		       j->mount_blend.blend_mount);
		return -errno;
	}

	syslog(LOG_INFO, "bdfs: blend mounted at %s",
	       j->mount_blend.blend_mount);
	bdfs_mount_track(d, BDFS_MNT_BLEND,
			 j->partition_uuid, 0,
			 j->mount_blend.blend_mount);
	return 0;
}
