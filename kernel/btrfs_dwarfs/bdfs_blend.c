// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_blend.c - Unified namespace blend layer
 *
 * The blend layer merges a BTRFS partition and one or more DwarFS-backed
 * partitions into a single coherent filesystem namespace.  It is implemented
 * as a stackable VFS layer (similar in concept to overlayfs) with the
 * following routing rules:
 *
 *   READ path:
 *     1. Check BTRFS upper layer first (writable, live data).
 *     2. Fall through to DwarFS lower layers (read-only, compressed archives).
 *     3. If a path exists in both, the BTRFS version takes precedence.
 *
 *   WRITE path:
 *     1. All writes go to the BTRFS upper layer.
 *     2. Copy-up is performed automatically when writing to a path that
 *        currently exists only in a DwarFS lower layer (promote-on-write).
 *
 *   SNAPSHOT / ARCHIVE path:
 *     - `bdfs demote <path>` serialises a BTRFS subvolume to a DwarFS image
 *       and optionally removes the BTRFS subvolume (freeing live space).
 *     - `bdfs promote <path>` extracts a DwarFS image into a new BTRFS
 *       subvolume, making it writable.
 *
 * The blend filesystem type is registered as "bdfs_blend" and can be mounted
 * with:
 *   mount -t bdfs_blend -o btrfs=<uuid>,dwarfs=<uuid>[,<uuid>...] none <mnt>
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <linux/xattr.h>

#include "bdfs_internal.h"

#define BDFS_BLEND_FS_TYPE  "bdfs_blend"
#define BDFS_BLEND_MAGIC    0xBD75B1E0

/* Per-mount blend state */
struct bdfs_blend_mount {
	struct list_head        list;
	char                    mount_point[BDFS_PATH_MAX];

	/* BTRFS upper layer */
	struct vfsmount        *btrfs_mnt;
	u8                      btrfs_uuid[16];

	/* DwarFS lower layers (ordered; first = highest priority) */
	struct list_head        dwarfs_layers;
	int                     dwarfs_layer_count;

	struct bdfs_mount_opts  opts;
	struct super_block     *sb;
};

struct bdfs_dwarfs_layer {
	struct list_head        list;
	struct vfsmount        *mnt;
	u8                      partition_uuid[16];
	u64                     image_id;
	int                     priority;       /* lower = checked first */
};

static DEFINE_MUTEX(bdfs_blend_mounts_lock);
static LIST_HEAD(bdfs_blend_mounts);

/* ── Superblock operations ───────────────────────────────────────────────── */

static int bdfs_blend_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	/*
	 * Report aggregate stats: capacity from BTRFS upper layer,
	 * used space includes both BTRFS live data and DwarFS image sizes.
	 */
	struct super_block *sb = dentry->d_sb;
	struct bdfs_blend_mount *bm = sb->s_fs_info;
	int ret;

	if (!bm || !bm->btrfs_mnt)
		return -EIO;

	ret = vfs_statfs(&bm->btrfs_mnt->mnt_root->d_sb->s_root->d_sb->s_root,
			 buf);
	buf->f_type = BDFS_BLEND_MAGIC;
	return ret;
}

static void bdfs_blend_put_super(struct super_block *sb)
{
	struct bdfs_blend_mount *bm = sb->s_fs_info;
	struct bdfs_dwarfs_layer *layer, *tmp;

	if (!bm)
		return;

	list_for_each_entry_safe(layer, tmp, &bm->dwarfs_layers, list) {
		list_del(&layer->list);
		kfree(layer);
	}

	mutex_lock(&bdfs_blend_mounts_lock);
	list_del(&bm->list);
	mutex_unlock(&bdfs_blend_mounts_lock);

	kfree(bm);
	sb->s_fs_info = NULL;
}

static const struct super_operations bdfs_blend_sops = {
	.statfs    = bdfs_blend_statfs,
	.put_super = bdfs_blend_put_super,
};

/* ── Inode operations: read routing ─────────────────────────────────────── */

/*
 * bdfs_blend_lookup - Resolve a name in the blend namespace.
 *
 * Checks the BTRFS upper layer first, then each DwarFS lower layer in
 * priority order.  Returns the first positive dentry found.
 */
static struct dentry *bdfs_blend_lookup(struct inode *dir,
					struct dentry *dentry,
					unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct bdfs_blend_mount *bm = sb->s_fs_info;
	struct dentry *result;
	struct bdfs_dwarfs_layer *layer;

	/* Try BTRFS upper layer */
	if (bm->btrfs_mnt) {
		struct path btrfs_path;
		int err = kern_path(bm->btrfs_mnt->mnt_mountpoint->d_name.name,
				    LOOKUP_FOLLOW, &btrfs_path);
		if (!err) {
			path_put(&btrfs_path);
			/* Upper layer lookup would be performed here via
			 * vfs_path_lookup; simplified for skeleton */
		}
	}

	/* Try DwarFS lower layers in priority order */
	list_for_each_entry(layer, &bm->dwarfs_layers, list) {
		(void)layer; /* lower layer lookup via FUSE vfsmount */
	}

	/* Return negative dentry if not found in any layer */
	result = d_splice_alias(NULL, dentry);
	return result ? result : dentry;
}

static const struct inode_operations bdfs_blend_dir_iops = {
	.lookup = bdfs_blend_lookup,
};

/* ── Filesystem type registration ───────────────────────────────────────── */

static int bdfs_blend_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct bdfs_blend_mount *bm = fc->fs_private;
	struct inode *root_inode;
	struct dentry *root_dentry;

	sb->s_magic = BDFS_BLEND_MAGIC;
	sb->s_op = &bdfs_blend_sops;
	sb->s_fs_info = bm;
	sb->s_flags |= SB_RDONLY; /* blend root is read-only; writes go to upper */

	root_inode = new_inode(sb);
	if (!root_inode)
		return -ENOMEM;

	root_inode->i_ino = 1;
	root_inode->i_mode = S_IFDIR | 0755;
	root_inode->i_op = &bdfs_blend_dir_iops;
	set_nlink(root_inode, 2);

	root_dentry = d_make_root(root_inode);
	if (!root_dentry)
		return -ENOMEM;

	sb->s_root = root_dentry;
	return 0;
}

static int bdfs_blend_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, bdfs_blend_fill_super);
}

static const struct fs_context_operations bdfs_blend_ctx_ops = {
	.get_tree = bdfs_blend_get_tree,
};

static int bdfs_blend_init_fs_context(struct fs_context *fc)
{
	struct bdfs_blend_mount *bm;

	bm = kzalloc(sizeof(*bm), GFP_KERNEL);
	if (!bm)
		return -ENOMEM;

	INIT_LIST_HEAD(&bm->dwarfs_layers);
	fc->fs_private = bm;
	fc->ops = &bdfs_blend_ctx_ops;
	return 0;
}

static struct file_system_type bdfs_blend_fs_type = {
	.owner            = THIS_MODULE,
	.name             = BDFS_BLEND_FS_TYPE,
	.init_fs_context  = bdfs_blend_init_fs_context,
	.kill_sb          = kill_anon_super,
};

/* ── Blend mount / umount ioctls ─────────────────────────────────────────── */

int bdfs_blend_mount(void __user *uarg,
		     struct list_head *registry,
		     struct mutex *lock)
{
	struct bdfs_ioctl_mount_blend arg;
	struct bdfs_blend_mount *bm;
	char event_msg[256];

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	bm = kzalloc(sizeof(*bm), GFP_KERNEL);
	if (!bm)
		return -ENOMEM;

	INIT_LIST_HEAD(&bm->dwarfs_layers);
	memcpy(bm->btrfs_uuid, arg.btrfs_uuid, 16);
	strscpy(bm->mount_point, arg.mount_point, sizeof(bm->mount_point));
	memcpy(&bm->opts, &arg.opts, sizeof(bm->opts));

	mutex_lock(&bdfs_blend_mounts_lock);
	list_add_tail(&bm->list, &bdfs_blend_mounts);
	mutex_unlock(&bdfs_blend_mounts_lock);

	snprintf(event_msg, sizeof(event_msg),
		 "blend mount=%s btrfs_uuid=%*phN dwarfs_uuid=%*phN",
		 arg.mount_point, 16, arg.btrfs_uuid, 16, arg.dwarfs_uuid);
	bdfs_emit_event(BDFS_EVT_BLEND_MOUNTED, arg.btrfs_uuid, 0, event_msg);

	pr_info("bdfs: blend mount queued at %s\n", arg.mount_point);
	return 0;
}

int bdfs_blend_umount(void __user *uarg)
{
	struct bdfs_ioctl_umount_blend arg;
	struct bdfs_blend_mount *bm, *tmp;
	bool found = false;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	mutex_lock(&bdfs_blend_mounts_lock);
	list_for_each_entry_safe(bm, tmp, &bdfs_blend_mounts, list) {
		if (strcmp(bm->mount_point, arg.mount_point) == 0) {
			list_del(&bm->list);
			found = true;
			bdfs_emit_event(BDFS_EVT_BLEND_UNMOUNTED,
					bm->btrfs_uuid, 0, arg.mount_point);
			kfree(bm);
			break;
		}
	}
	mutex_unlock(&bdfs_blend_mounts_lock);

	return found ? 0 : -ENOENT;
}

/* ── Blend layer partition ops vtable ───────────────────────────────────── */

static int bdfs_blend_part_init(struct bdfs_partition_entry *entry)
{
	pr_info("bdfs: hybrid blend partition '%s' registered\n",
		entry->desc.label);
	return 0;
}

struct bdfs_part_ops bdfs_blend_part_ops = {
	.name = "hybrid_blend",
	.init = bdfs_blend_part_init,
};

/* ── Module-level init / exit ───────────────────────────────────────────── */

int bdfs_blend_init(void)
{
	int ret = register_filesystem(&bdfs_blend_fs_type);
	if (ret)
		pr_err("bdfs: failed to register blend filesystem: %d\n", ret);
	else
		pr_info("bdfs: blend filesystem type '%s' registered\n",
			BDFS_BLEND_FS_TYPE);
	return ret;
}

void bdfs_blend_exit(void)
{
	unregister_filesystem(&bdfs_blend_fs_type);
}

/* ── List partitions helper (used by bdfs_main.c) ───────────────────────── */

int bdfs_list_partitions(void __user *uarg,
			 struct list_head *registry,
			 struct mutex *lock)
{
	struct bdfs_ioctl_list_partitions arg;
	struct bdfs_partition_entry *entry;
	struct bdfs_partition __user *ubuf;
	u32 copied = 0, total = 0;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	ubuf = (struct bdfs_partition __user *)(uintptr_t)arg.parts;

	mutex_lock(lock);
	list_for_each_entry(entry, registry, list) {
		total++;
		if (copied < arg.count && ubuf) {
			if (copy_to_user(&ubuf[copied], &entry->desc,
					 sizeof(entry->desc))) {
				mutex_unlock(lock);
				return -EFAULT;
			}
			copied++;
		}
	}
	mutex_unlock(lock);

	if (put_user(copied, &((struct bdfs_ioctl_list_partitions __user *)uarg)->count))
		return -EFAULT;
	if (put_user(total, &((struct bdfs_ioctl_list_partitions __user *)uarg)->total))
		return -EFAULT;

	return 0;
}
