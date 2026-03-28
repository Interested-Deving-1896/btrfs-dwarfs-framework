/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bdfs_internal.h - Internal kernel-side declarations for the BDFS framework
 */

#ifndef _BDFS_INTERNAL_H
#define _BDFS_INTERNAL_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kref.h>
#include "../../include/uapi/bdfs_ioctl.h"

/* Forward declaration */
struct bdfs_partition_entry;

/*
 * Backend operations vtable.  Each partition type (DwarFS-backed,
 * BTRFS-backed, hybrid blend) implements this interface.
 */
struct bdfs_part_ops {
	const char *name;

	/* Called when a partition is registered */
	int  (*init)(struct bdfs_partition_entry *entry);

	/* Called when a partition is unregistered */
	void (*destroy)(struct bdfs_partition_entry *entry);

	/* Called to mount the partition's filesystem */
	int  (*mount)(struct bdfs_partition_entry *entry,
		      const char *mount_point,
		      struct bdfs_mount_opts *opts);

	/* Called to unmount */
	int  (*umount)(struct bdfs_partition_entry *entry,
		       const char *mount_point, u32 flags);

	/* Partition-specific ioctl passthrough */
	long (*ioctl)(struct bdfs_partition_entry *entry,
		      unsigned int cmd, unsigned long arg);
};

/* Declared in bdfs_dwarfs_part.c */
extern struct bdfs_part_ops bdfs_dwarfs_part_ops;

/* Declared in bdfs_btrfs_part.c */
extern struct bdfs_part_ops bdfs_btrfs_part_ops;

/* Declared in bdfs_blend.c */
extern struct bdfs_part_ops bdfs_blend_part_ops;

/* ── DwarFS-backed partition operations (bdfs_dwarfs_part.c) ─────────── */

int bdfs_dwarfs_export(void __user *uarg,
		       struct list_head *registry,
		       struct mutex *lock);

int bdfs_dwarfs_mount_image(void __user *uarg,
			    struct list_head *registry,
			    struct mutex *lock);

int bdfs_dwarfs_umount_image(void __user *uarg,
			     struct list_head *registry,
			     struct mutex *lock);

int bdfs_dwarfs_list_images(void __user *uarg,
			    struct list_head *registry,
			    struct mutex *lock);

/* ── BTRFS-backed partition operations (bdfs_btrfs_part.c) ──────────── */

int bdfs_btrfs_import(void __user *uarg,
		      struct list_head *registry,
		      struct mutex *lock);

int bdfs_btrfs_store_image(void __user *uarg,
			   struct list_head *registry,
			   struct mutex *lock);

int bdfs_btrfs_snapshot_container(void __user *uarg,
				   struct list_head *registry,
				   struct mutex *lock);

int bdfs_btrfs_list_subvols(void __user *uarg,
			    struct list_head *registry,
			    struct mutex *lock);

/* ── Blend layer (bdfs_blend.c) ─────────────────────────────────────── */

int  bdfs_blend_init(void);
void bdfs_blend_exit(void);

int bdfs_blend_mount(void __user *uarg,
		     struct list_head *registry,
		     struct mutex *lock);

int bdfs_blend_umount(void __user *uarg);

/* ── Partition list helpers (bdfs_main.c) ────────────────────────────── */

int bdfs_list_partitions(void __user *uarg,
			 struct list_head *registry,
			 struct mutex *lock);

/* ── Event emission (bdfs_main.c) ────────────────────────────────────── */

void bdfs_emit_event(enum bdfs_event_type type, const u8 uuid[16],
		     u64 object_id, const char *message);

#endif /* _BDFS_INTERNAL_H */
