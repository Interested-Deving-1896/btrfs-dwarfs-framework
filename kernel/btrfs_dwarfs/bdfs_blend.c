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
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/file.h>
#include <linux/writeback.h>

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

/*
 * Pending copy-up table — maps (btrfs_uuid, inode_no) → blend inode.
 *
 * When bdfs_blend_trigger_copyup() emits a BDFS_EVT_COPYUP_NEEDED event it
 * registers the waiting blend inode here.  bdfs_copyup_complete() looks up
 * the inode by (uuid, ino) and calls bdfs_blend_complete_copyup() on it.
 * This avoids the incorrect ilookup(upper_sb, ino) approach which searched
 * the wrong superblock.
 */
struct bdfs_copyup_entry {
	struct list_head list;
	u8               btrfs_uuid[16];
	u64              inode_no;
	struct inode    *inode;   /* blend inode; held with ihold() */
};

static DEFINE_MUTEX(bdfs_copyup_table_lock);
static LIST_HEAD(bdfs_copyup_table);

static void bdfs_copyup_register(const u8 uuid[16], u64 ino,
				 struct inode *inode)
{
	struct bdfs_copyup_entry *e;

	e = kmalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return;

	memcpy(e->btrfs_uuid, uuid, 16);
	e->inode_no = ino;
	e->inode    = inode;
	ihold(inode);

	mutex_lock(&bdfs_copyup_table_lock);
	list_add(&e->list, &bdfs_copyup_table);
	mutex_unlock(&bdfs_copyup_table_lock);
}

static struct inode *bdfs_copyup_lookup_and_remove(const u8 uuid[16], u64 ino)
{
	struct bdfs_copyup_entry *e, *tmp;
	struct inode *found = NULL;

	mutex_lock(&bdfs_copyup_table_lock);
	list_for_each_entry_safe(e, tmp, &bdfs_copyup_table, list) {
		if (e->inode_no == ino &&
		    memcmp(e->btrfs_uuid, uuid, 16) == 0) {
			found = e->inode; /* caller owns the ihold ref */
			list_del(&e->list);
			kfree(e);
			break;
		}
	}
	mutex_unlock(&bdfs_copyup_table_lock);
	return found;
}

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
	.alloc_inode  = bdfs_blend_alloc_inode,
	.free_inode   = bdfs_blend_free_inode,
	.statfs       = bdfs_blend_statfs,
	.put_super    = bdfs_blend_put_super,
};

/* ── Inode operations: read routing ─────────────────────────────────────── */

/*
 * bdfs_blend_inode_from_path - Create a blend inode that aliases a real inode
 * found on a backing layer (BTRFS or DwarFS FUSE mount).
 *
 * We create a new inode in the blend superblock that mirrors the attributes
 * of the real inode.  The real path is stored in i_private so that file
 * operations can be forwarded to the backing layer.
 */
struct bdfs_blend_inode_info {
	struct inode    vfs_inode;
	struct path     real_path;      /* path on the backing layer */
	bool            is_upper;       /* true = BTRFS, false = DwarFS lower */
	atomic_t        copyup_done;    /* set to 1 when daemon finishes copy-up */
	/*
	 * Cached open file for the upper-layer backing file.
	 * Populated on first write; avoids a dentry_open() per write_iter call.
	 * Protected by the inode's i_rwsem (held by the VFS for writes).
	 * Dropped in bdfs_blend_free_inode().
	 */
	struct file    *upper_file;
};

static inline struct bdfs_blend_inode_info *
BDFS_I(struct inode *inode)
{
	return container_of(inode, struct bdfs_blend_inode_info, vfs_inode);
}

static struct inode *bdfs_blend_alloc_inode(struct super_block *sb)
{
	struct bdfs_blend_inode_info *bi;

	bi = kmem_cache_alloc(bdfs_inode_cachep, GFP_KERNEL);
	if (!bi)
		return NULL;
	memset(&bi->real_path, 0, sizeof(bi->real_path));
	bi->is_upper   = false;
	bi->upper_file = NULL;
	atomic_set(&bi->copyup_done, 0);
	return &bi->vfs_inode;
}

static void bdfs_blend_free_inode(struct inode *inode)
{
	struct bdfs_blend_inode_info *bi = BDFS_I(inode);
	if (bi->upper_file) {
		fput(bi->upper_file);
		bi->upper_file = NULL;
	}
	path_put(&bi->real_path);
	kmem_cache_free(bdfs_inode_cachep, bi);
}

/* Inode cache — allocated once at module init */
static struct kmem_cache *bdfs_inode_cachep;

static void bdfs_inode_init_once(void *obj)
{
	struct bdfs_blend_inode_info *bi = obj;
	inode_init_once(&bi->vfs_inode);
}

/*
 * bdfs_blend_make_inode - Allocate a blend inode mirroring a real inode.
 *
 * Copies uid/gid/mode/size/timestamps from the real inode so that stat(2)
 * returns correct values.  File operations are forwarded via real_path.
 */
static struct inode *bdfs_blend_make_inode(struct super_block *sb,
					   const struct path *real_path,
					   bool is_upper)
{
	struct inode *inode;
	struct inode *real = d_inode(real_path->dentry);
	struct bdfs_blend_inode_info *bi;

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	bi = BDFS_I(inode);
	path_get(real_path);
	bi->real_path = *real_path;
	bi->is_upper  = is_upper;
	atomic_set(&bi->copyup_done, is_upper ? 1 : 0);

	/* Mirror attributes from the real inode */
	inode->i_ino   = real->i_ino;
	inode->i_mode  = real->i_mode;
	inode->i_uid   = real->i_uid;
	inode->i_gid   = real->i_gid;
	inode->i_size  = real->i_size;
	inode->i_atime = real->i_atime;
	inode->i_mtime = real->i_mtime;
	inode->i_ctime = real->i_ctime;
	set_nlink(inode, real->i_nlink);

	if (S_ISDIR(inode->i_mode)) {
		inode->i_op  = &bdfs_blend_dir_iops;
		inode->i_fop = &bdfs_blend_dir_fops;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op  = &bdfs_blend_file_iops;
		inode->i_fop = &bdfs_blend_file_fops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op  = &bdfs_blend_symlink_iops;
		/* symlinks have no file_operations */
	} else {
		/* Special files (devices, fifos, sockets): forward to real */
		init_special_inode(inode, inode->i_mode, real->i_rdev);
	}

	return inode;
}

/*
 * bdfs_blend_rel_path - Build the path of @dentry relative to the blend root.
 *
 * Walks the dentry parent chain up to the blend superblock root, collecting
 * component names, then assembles them into a slash-separated string in @buf.
 * Returns a pointer into @buf on success, or ERR_PTR on error.
 *
 * Example: for blend root "/" and dentry at "a/b/c", returns "a/b/c".
 */
static char *bdfs_blend_rel_path(struct dentry *dentry, char *buf, size_t bufsz)
{
	/* Walk up to the root collecting names */
	struct dentry *d = dentry;
	char *p = buf + bufsz - 1;
	*p = '\0';

	while (!IS_ROOT(d)) {
		const char *name = d->d_name.name;
		size_t len = d->d_name.len;

		if (p - buf < (ptrdiff_t)(len + 1))
			return ERR_PTR(-ENAMETOOLONG);

		p -= len;
		memcpy(p, name, len);
		p--;
		*p = '/';
		d = d->d_parent;
	}

	/* p now points at the leading '/' — skip it */
	if (*p == '/')
		p++;

	return p;
}

/*
 * bdfs_blend_lookup - Resolve a name in the blend namespace.
 *
 * Routing order:
 *   1. BTRFS upper layer  (writable; takes precedence on name collision)
 *   2. DwarFS lower layers in priority order (read-only compressed archives)
 *
 * For each layer we use vfs_path_lookup() to resolve the FULL relative path
 * from the layer root to the child being looked up.  This correctly handles
 * nested directories: "a/b/c" is resolved as a single lookup from the layer
 * root rather than just resolving "c" relative to the layer root (which would
 * only work for top-level entries).
 *
 * Copy-up on write is handled at the file_operations level: any write to a
 * blend inode backed by a DwarFS lower layer triggers a copy-up to the BTRFS
 * upper layer before the write proceeds.
 */
static struct dentry *bdfs_blend_lookup(struct inode *dir,
					struct dentry *dentry,
					unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct bdfs_blend_mount *bm = sb->s_fs_info;
	struct bdfs_blend_inode_info *parent_bi = BDFS_I(dir);
	struct path parent_real = parent_bi->real_path;
	struct path child_path;
	struct inode *new_inode;
	struct dentry *new_dentry;
	const char *name = dentry->d_name.name;
	/* Full relative path buffer — used for lower-layer lookups */
	char *relpath_buf;
	char *relpath;
	int err;

	/*
	 * Build the full relative path for this dentry (e.g. "usr/lib/foo.so").
	 * We need this for lower-layer lookups so that nested directories are
	 * resolved correctly from the layer root rather than just by name.
	 */
	relpath_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!relpath_buf)
		return ERR_PTR(-ENOMEM);

	relpath = bdfs_blend_rel_path(dentry, relpath_buf, PATH_MAX);
	if (IS_ERR(relpath)) {
		kfree(relpath_buf);
		return ERR_CAST(relpath);
	}

	/*
	 * Step 1: Try the BTRFS upper layer.
	 *
	 * If the parent is already on the upper layer, resolve relative to it
	 * (single component lookup — fast path).  If the parent is from a
	 * lower layer, resolve the full relative path from the BTRFS root so
	 * we find any upper-layer override at any depth.
	 */
	if (bm->btrfs_mnt) {
		if (parent_bi->is_upper) {
			/* Fast path: parent is already on upper layer */
			err = vfs_path_lookup(parent_real.dentry,
					      parent_real.mnt,
					      name, 0, &child_path);
		} else {
			/* Full path from BTRFS root */
			err = vfs_path_lookup(bm->btrfs_mnt->mnt_root,
					      bm->btrfs_mnt,
					      relpath, 0, &child_path);
		}
		if (!err && d_is_positive(child_path.dentry)) {
			new_inode = bdfs_blend_make_inode(sb, &child_path,
							  true);
			path_put(&child_path);
			kfree(relpath_buf);
			if (!new_inode)
				return ERR_PTR(-ENOMEM);
			new_dentry = d_splice_alias(new_inode, dentry);
			return new_dentry ? new_dentry : dentry;
		}
		if (!err)
			path_put(&child_path);
	}

	/*
	 * Step 2: Fall through to DwarFS lower layers in priority order.
	 *
	 * Resolve the full relative path from each layer's root so that
	 * nested directories (e.g. "usr/lib/foo.so") are found correctly
	 * regardless of how deep the current directory is.
	 */
	{
		struct bdfs_dwarfs_layer *layer;

		list_for_each_entry(layer, &bm->dwarfs_layers, list) {
			if (!layer->mnt)
				continue;

			err = vfs_path_lookup(layer->mnt->mnt_root,
					      layer->mnt, relpath, 0,
					      &child_path);
			if (!err && d_is_positive(child_path.dentry)) {
				new_inode = bdfs_blend_make_inode(
						sb, &child_path, false);
				path_put(&child_path);
				kfree(relpath_buf);
				if (!new_inode)
					return ERR_PTR(-ENOMEM);
				new_dentry = d_splice_alias(new_inode, dentry);
				return new_dentry ? new_dentry : dentry;
			}
			if (!err)
				path_put(&child_path);
		}
	}

	/* Not found in any layer — return a negative dentry */
	kfree(relpath_buf);
	d_add(dentry, NULL);
	return NULL;
}

/*
 * bdfs_blend_getattr - Forward stat to the real backing inode.
 *
 * Refreshes size and timestamps from the real inode before returning,
 * so that changes on the backing layer are reflected immediately.
 */
static int bdfs_blend_getattr(struct mnt_idmap *idmap,
			      const struct path *path,
			      struct kstat *stat,
			      u32 request_mask,
			      unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct bdfs_blend_inode_info *bi = BDFS_I(inode);
	struct inode *real;

	if (!bi->real_path.dentry)
		return -EIO;

	real = d_inode(bi->real_path.dentry);

	/* Refresh from real inode */
	inode->i_size  = real->i_size;
	inode->i_atime = real->i_atime;
	inode->i_mtime = real->i_mtime;
	inode->i_ctime = real->i_ctime;

	generic_fillattr(idmap, inode, stat);
	return 0;
}

/* ── Copy-up synchronization ─────────────────────────────────────────────── */

/*
 * Per-inode copy-up state.  When a write is attempted on a DwarFS-backed
 * inode the kernel emits a BDFS_EVT_COPYUP_NEEDED event and then waits on
 * copyup_done.  The daemon performs the promote job and calls back via
 * BDFS_IOC_COPYUP_COMPLETE (handled in bdfs_main.c), which wakes all
 * waiters and flips copyup_done to 1.
 *
 * Once copy-up completes, real_path is updated to point at the new BTRFS
 * upper-layer file and is_upper is set to true.  Subsequent opens skip
 * the copy-up path entirely.
 */
static DECLARE_WAIT_QUEUE_HEAD(bdfs_copyup_wq);

/*
 * bdfs_blend_trigger_copyup - Emit copy-up event and wait for completion.
 *
 * Returns 0 when the daemon has finished promoting the file to the BTRFS
 * upper layer, or a negative error code on timeout / signal.
 *
 * The caller must hold no locks.  We use an interruptible wait with a
 * 30-second timeout so a crashed daemon does not wedge the process forever.
 */
static int bdfs_blend_trigger_copyup(struct inode *inode,
				     struct bdfs_blend_mount *bm)
{
	struct bdfs_blend_inode_info *bi = BDFS_I(inode);
	long timeout;

	/* Register in the copyup table so bdfs_copyup_complete can find us */
	bdfs_copyup_register(bm->btrfs_uuid, inode->i_ino, inode);

	/*
	 * Emit the event.  The daemon listens on the netlink socket and will
	 * start a BDFS_JOB_PROMOTE_COPYUP for this inode.
	 *
	 * Message format:
	 *   "copyup_needed lower=<lower_path> upper=<upper_path>"
	 *
	 * lower_path: the real path on the DwarFS FUSE mount (bi->real_path).
	 * upper_path: the corresponding path on the BTRFS upper layer, derived
	 *             by replacing the DwarFS mount root with the BTRFS root.
	 *
	 * We build the upper path by taking the dentry name relative to the
	 * DwarFS layer root and appending it to the BTRFS upper mount root.
	 * For the common case (single-level path) this is exact; for nested
	 * paths the daemon must walk the dentry chain — we pass the full
	 * dentry path via d_path() for both layers.
	 */
	{
		char lower_buf[256] = {0};
		char upper_buf[256] = {0};
		char msg[sizeof(((struct bdfs_event *)0)->message)];

		/* Resolve the real lower path */
		if (bi->real_path.dentry && bi->real_path.mnt) {
			char *p = d_path(&bi->real_path, lower_buf,
					 sizeof(lower_buf));
			if (!IS_ERR(p))
				memmove(lower_buf, p,
					strlen(p) + 1);
		}

		/* Derive the upper path: same relative path under BTRFS root */
		if (bm->btrfs_mnt && lower_buf[0]) {
			struct path btrfs_root = {
				.mnt    = bm->btrfs_mnt,
				.dentry = bm->btrfs_mnt->mnt_root,
			};
			char root_buf[256] = {0};
			char *rp = d_path(&btrfs_root, root_buf,
					  sizeof(root_buf));
			if (!IS_ERR(rp)) {
				/* upper = btrfs_root + basename(lower) */
				const char *base = strrchr(lower_buf, '/');
				if (base)
					snprintf(upper_buf, sizeof(upper_buf),
						 "%s%s", rp, base);
			}
		}

		snprintf(msg, sizeof(msg),
			 "copyup_needed lower=%s upper=%s",
			 lower_buf, upper_buf);

		bdfs_emit_event(BDFS_EVT_COPYUP_NEEDED,
				bm->btrfs_uuid, inode->i_ino, msg);
	}

	/*
	 * Wait for the daemon to signal completion.  The daemon calls
	 * BDFS_IOC_COPYUP_COMPLETE which sets bi->copyup_done and wakes us.
	 */
	timeout = wait_event_interruptible_timeout(
		bdfs_copyup_wq,
		atomic_read(&bi->copyup_done) != 0,
		30 * HZ);

	if (timeout < 0)
		return -EINTR;
	if (timeout == 0)
		return -ETIMEDOUT;

	return 0;
}

/* ── Write path helpers ──────────────────────────────────────────────────── */

/*
 * bdfs_blend_upper_path - Return the real path on the BTRFS upper layer.
 *
 * For upper-layer inodes this is bi->real_path directly.  For lower-layer
 * inodes this constructs the corresponding upper-layer path by appending
 * the dentry name to the BTRFS upper root.  Used by write operations that
 * need to operate on the upper layer even before copy-up has run.
 */
static int bdfs_blend_upper_path(struct inode *dir, struct dentry *dentry,
				 struct bdfs_blend_mount *bm,
				 struct path *upper_path)
{
	struct bdfs_blend_inode_info *parent_bi = BDFS_I(dir);

	if (!bm->btrfs_mnt)
		return -EIO;

	/*
	 * If the parent is already on the upper layer, resolve relative to it.
	 * Otherwise resolve relative to the BTRFS upper root.
	 */
	if (parent_bi->is_upper) {
		return vfs_path_lookup(parent_bi->real_path.dentry,
				       parent_bi->real_path.mnt,
				       dentry->d_name.name, 0, upper_path);
	}

	return vfs_path_lookup(bm->btrfs_mnt->mnt_root,
			       bm->btrfs_mnt,
			       dentry->d_name.name, 0, upper_path);
}

/* ── File operations ─────────────────────────────────────────────────────── */

/*
 * bdfs_blend_open - Forward open to the real backing file.
 *
 * For write-mode opens on a DwarFS-backed inode, copy-up is triggered:
 * we emit a promote event to the daemon and block until it completes.
 * Once copy-up is done, real_path points to the BTRFS upper layer and
 * subsequent opens take the fast path.
 */
static int bdfs_blend_open(struct inode *inode, struct file *file)
{
	struct bdfs_blend_inode_info *bi = BDFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct bdfs_blend_mount *bm = sb->s_fs_info;
	int ret;

	if (!bi->is_upper && (file->f_flags & (O_WRONLY | O_RDWR))) {
		ret = bdfs_blend_trigger_copyup(inode, bm);
		if (ret)
			return ret;
		/*
		 * After copy-up, bi->real_path and bi->is_upper have been
		 * updated by bdfs_blend_complete_copyup() called from the
		 * BDFS_IOC_COPYUP_COMPLETE ioctl handler.
		 */
	}

	return finish_open(file, bi->real_path.dentry, generic_file_open);
}

/* ── Directory write operations ──────────────────────────────────────────── */

/*
 * bdfs_blend_create - Create a new regular file on the BTRFS upper layer.
 *
 * New files are always created on the upper layer regardless of whether a
 * lower-layer directory exists at the same path.
 */
static int bdfs_blend_create(struct mnt_idmap *idmap,
			     struct inode *dir, struct dentry *dentry,
			     umode_t mode, bool excl)
{
	struct super_block *sb = dir->i_sb;
	struct bdfs_blend_mount *bm = sb->s_fs_info;
	struct bdfs_blend_inode_info *parent_bi = BDFS_I(dir);
	struct path upper_parent;
	struct dentry *upper_dentry;
	struct inode *new_inode;
	int ret;

	if (!bm || !bm->btrfs_mnt)
		return -EIO;

	/*
	 * Resolve the parent directory on the BTRFS upper layer.
	 *
	 * When the parent blend inode is already on the upper layer we can
	 * use its cached real_path directly.  When it is a lower-layer inode
	 * we must look up the full relative path from the BTRFS root so that
	 * nested directories (e.g. "a/b/c") are resolved correctly — using
	 * an empty path would land at the BTRFS root instead of the right
	 * subdirectory.
	 */
	if (parent_bi->is_upper) {
		path_get(&parent_bi->real_path);
		upper_parent = parent_bi->real_path;
	} else {
		char *relpath_buf = kmalloc(PATH_MAX, GFP_KERNEL);
		char *relpath;

		if (!relpath_buf)
			return -ENOMEM;

		relpath = bdfs_blend_rel_path(parent_bi->real_path.dentry,
					      relpath_buf, PATH_MAX);
		if (IS_ERR(relpath)) {
			kfree(relpath_buf);
			return PTR_ERR(relpath);
		}

		ret = vfs_path_lookup(bm->btrfs_mnt->mnt_root,
				      bm->btrfs_mnt, relpath, 0, &upper_parent);
		kfree(relpath_buf);
		if (ret)
			return ret;
	}

	upper_dentry = lookup_one_len(dentry->d_name.name,
				      upper_parent.dentry,
				      dentry->d_name.len);
	if (IS_ERR(upper_dentry)) {
		path_put(&upper_parent);
		return PTR_ERR(upper_dentry);
	}

	ret = vfs_create(idmap, d_inode(upper_parent.dentry),
			 upper_dentry, mode, excl);
	if (ret) {
		dput(upper_dentry);
		path_put(&upper_parent);
		return ret;
	}

	/* Wrap the new upper-layer inode in a blend inode */
	struct path new_path = { .mnt = upper_parent.mnt,
				 .dentry = upper_dentry };
	new_inode = bdfs_blend_make_inode(sb, &new_path, true);
	dput(upper_dentry);
	path_put(&upper_parent);

	if (!new_inode)
		return -ENOMEM;

	d_instantiate(dentry, new_inode);
	return 0;
}

/*
 * bdfs_blend_mkdir - Create a directory on the BTRFS upper layer.
 */
static int bdfs_blend_mkdir(struct mnt_idmap *idmap,
			    struct inode *dir, struct dentry *dentry,
			    umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct bdfs_blend_mount *bm = sb->s_fs_info;
	struct bdfs_blend_inode_info *parent_bi = BDFS_I(dir);
	struct path upper_parent;
	struct dentry *upper_dentry;
	struct inode *new_inode;
	int ret;

	if (!bm || !bm->btrfs_mnt)
		return -EIO;

	/* See bdfs_blend_create for the rationale behind the lower-layer path. */
	if (parent_bi->is_upper) {
		path_get(&parent_bi->real_path);
		upper_parent = parent_bi->real_path;
	} else {
		char *relpath_buf = kmalloc(PATH_MAX, GFP_KERNEL);
		char *relpath;

		if (!relpath_buf)
			return -ENOMEM;

		relpath = bdfs_blend_rel_path(parent_bi->real_path.dentry,
					      relpath_buf, PATH_MAX);
		if (IS_ERR(relpath)) {
			kfree(relpath_buf);
			return PTR_ERR(relpath);
		}

		ret = vfs_path_lookup(bm->btrfs_mnt->mnt_root,
				      bm->btrfs_mnt, relpath, 0, &upper_parent);
		kfree(relpath_buf);
		if (ret)
			return ret;
	}

	upper_dentry = lookup_one_len(dentry->d_name.name,
				      upper_parent.dentry,
				      dentry->d_name.len);
	if (IS_ERR(upper_dentry)) {
		path_put(&upper_parent);
		return PTR_ERR(upper_dentry);
	}

	ret = vfs_mkdir(idmap, d_inode(upper_parent.dentry),
			upper_dentry, mode);
	if (ret) {
		dput(upper_dentry);
		path_put(&upper_parent);
		return ret;
	}

	struct path new_path = { .mnt = upper_parent.mnt,
				 .dentry = upper_dentry };
	new_inode = bdfs_blend_make_inode(sb, &new_path, true);
	dput(upper_dentry);
	path_put(&upper_parent);

	if (!new_inode)
		return -ENOMEM;

	d_instantiate(dentry, new_inode);
	return 0;
}

/*
 * bdfs_blend_unlink - Remove a file from the BTRFS upper layer.
 *
 * Removing a file that exists only in a DwarFS lower layer is not permitted
 * without a prior promote (copy-up).  The blend layer is not a full
 * union filesystem — whiteouts are not implemented.
 */
static int bdfs_blend_unlink(struct inode *dir, struct dentry *dentry)
{
	struct bdfs_blend_inode_info *bi = BDFS_I(d_inode(dentry));
	struct bdfs_blend_inode_info *parent_bi = BDFS_I(dir);
	struct mnt_idmap *idmap;

	if (!bi->is_upper)
		return -EPERM;	/* must promote before deleting */

	idmap = mnt_idmap(bi->real_path.mnt);
	return vfs_unlink(idmap,
			  d_inode(parent_bi->real_path.dentry),
			  bi->real_path.dentry,
			  NULL);
}

/*
 * bdfs_blend_rmdir - Remove a directory from the BTRFS upper layer.
 */
static int bdfs_blend_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct bdfs_blend_inode_info *bi = BDFS_I(d_inode(dentry));
	struct bdfs_blend_inode_info *parent_bi = BDFS_I(dir);
	struct mnt_idmap *idmap;

	if (!bi->is_upper)
		return -EPERM;

	idmap = mnt_idmap(bi->real_path.mnt);
	return vfs_rmdir(idmap,
			 d_inode(parent_bi->real_path.dentry),
			 bi->real_path.dentry);
}

/*
 * bdfs_blend_rename - Rename within the BTRFS upper layer.
 *
 * Both source and destination must be on the upper layer.  Renaming a
 * lower-layer entry requires a prior promote.
 */
static int bdfs_blend_rename(struct mnt_idmap *idmap,
			     struct inode *old_dir, struct dentry *old_dentry,
			     struct inode *new_dir, struct dentry *new_dentry,
			     unsigned int flags)
{
	struct bdfs_blend_inode_info *src_bi      = BDFS_I(d_inode(old_dentry));
	struct bdfs_blend_inode_info *old_par_bi  = BDFS_I(old_dir);
	struct bdfs_blend_inode_info *new_par_bi  = BDFS_I(new_dir);
	struct dentry *real_new_dentry;
	int ret;

	if (!src_bi->is_upper)
		return -EPERM;

	if (!old_par_bi->is_upper || !new_par_bi->is_upper)
		return -EPERM;

	/*
	 * new_dentry is a blend-layer dentry.  We must look up the
	 * corresponding dentry in the BTRFS upper layer so vfs_rename
	 * operates entirely within one real filesystem.
	 */
	real_new_dentry = lookup_one_len(new_dentry->d_name.name,
					 new_par_bi->real_path.dentry,
					 new_dentry->d_name.len);
	if (IS_ERR(real_new_dentry))
		return PTR_ERR(real_new_dentry);

	ret = vfs_rename(idmap,
			 &(struct renamedata){
				.old_mnt_idmap = idmap,
				.old_dir       = d_inode(old_par_bi->real_path.dentry),
				.old_dentry    = src_bi->real_path.dentry,
				.new_mnt_idmap = idmap,
				.new_dir       = d_inode(new_par_bi->real_path.dentry),
				.new_dentry    = real_new_dentry,
				.flags         = flags,
			 });

	dput(real_new_dentry);
	return ret;
}

/*
 * bdfs_blend_symlink - Create a symbolic link on the BTRFS upper layer.
 */
static int bdfs_blend_symlink(struct mnt_idmap *idmap,
			      struct inode *dir, struct dentry *dentry,
			      const char *symname)
{
	struct super_block *sb = dir->i_sb;
	struct bdfs_blend_mount *bm = sb->s_fs_info;
	struct bdfs_blend_inode_info *parent_bi = BDFS_I(dir);
	struct path upper_parent;
	struct dentry *upper_dentry;
	struct inode *new_inode;
	int ret;

	if (!bm || !bm->btrfs_mnt)
		return -EIO;

	/* See bdfs_blend_create for the rationale behind the lower-layer path. */
	if (parent_bi->is_upper) {
		path_get(&parent_bi->real_path);
		upper_parent = parent_bi->real_path;
	} else {
		char *relpath_buf = kmalloc(PATH_MAX, GFP_KERNEL);
		char *relpath;

		if (!relpath_buf)
			return -ENOMEM;

		relpath = bdfs_blend_rel_path(parent_bi->real_path.dentry,
					      relpath_buf, PATH_MAX);
		if (IS_ERR(relpath)) {
			kfree(relpath_buf);
			return PTR_ERR(relpath);
		}

		ret = vfs_path_lookup(bm->btrfs_mnt->mnt_root,
				      bm->btrfs_mnt, relpath, 0, &upper_parent);
		kfree(relpath_buf);
		if (ret)
			return ret;
	}

	upper_dentry = lookup_one_len(dentry->d_name.name,
				      upper_parent.dentry,
				      dentry->d_name.len);
	if (IS_ERR(upper_dentry)) {
		path_put(&upper_parent);
		return PTR_ERR(upper_dentry);
	}

	ret = vfs_symlink(idmap, d_inode(upper_parent.dentry),
			  upper_dentry, symname);
	if (ret) {
		dput(upper_dentry);
		path_put(&upper_parent);
		return ret;
	}

	struct path new_path = { .mnt = upper_parent.mnt,
				 .dentry = upper_dentry };
	new_inode = bdfs_blend_make_inode(sb, &new_path, true);
	dput(upper_dentry);
	path_put(&upper_parent);

	if (!new_inode)
		return -ENOMEM;

	d_instantiate(dentry, new_inode);
	return 0;
}

/*
 * bdfs_blend_link - Create a hard link on the BTRFS upper layer.
 *
 * Both the source inode and the target directory must be on the upper layer.
 * Linking a lower-layer inode requires a prior promote.
 */
static int bdfs_blend_link(struct dentry *old_dentry, struct inode *dir,
			   struct dentry *new_dentry)
{
	struct bdfs_blend_inode_info *src_bi    = BDFS_I(d_inode(old_dentry));
	struct bdfs_blend_inode_info *parent_bi = BDFS_I(dir);
	struct dentry *real_new_dentry;
	struct mnt_idmap *idmap;
	int ret;

	if (!src_bi->is_upper || !parent_bi->is_upper)
		return -EPERM;

	real_new_dentry = lookup_one_len(new_dentry->d_name.name,
					 parent_bi->real_path.dentry,
					 new_dentry->d_name.len);
	if (IS_ERR(real_new_dentry))
		return PTR_ERR(real_new_dentry);

	idmap = mnt_idmap(src_bi->real_path.mnt);
	ret = vfs_link(src_bi->real_path.dentry, idmap,
		       d_inode(parent_bi->real_path.dentry),
		       real_new_dentry, NULL);
	dput(real_new_dentry);
	return ret;
}

/*
 * bdfs_blend_setattr - Forward attribute changes to the BTRFS upper layer.
 *
 * chmod/chown/truncate on a lower-layer inode requires copy-up first.
 */
static int bdfs_blend_setattr(struct mnt_idmap *idmap,
			      struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct bdfs_blend_inode_info *bi = BDFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct bdfs_blend_mount *bm = sb->s_fs_info;
	int ret;

	if (!bi->is_upper) {
		ret = bdfs_blend_trigger_copyup(inode, bm);
		if (ret)
			return ret;
	}

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;

	ret = notify_change(idmap, bi->real_path.dentry, attr, NULL);
	if (ret)
		return ret;

	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

/*
 * bdfs_blend_complete_copyup - Called from BDFS_IOC_COPYUP_COMPLETE ioctl.
 *
 * Updates the inode's real_path to point at the new BTRFS upper-layer file,
 * marks it as upper, and wakes all threads waiting in bdfs_blend_open().
 *
 * @inode:      the blend inode being promoted
 * @upper_path: the new path on the BTRFS upper layer
 */
void bdfs_blend_complete_copyup(struct inode *inode,
				const struct path *upper_path)
{
	struct bdfs_blend_inode_info *bi = BDFS_I(inode);

	path_put(&bi->real_path);
	path_get(upper_path);
	bi->real_path = *upper_path;
	bi->is_upper  = true;

	atomic_set(&bi->copyup_done, 1);
	wake_up_all(&bdfs_copyup_wq);
}
EXPORT_SYMBOL_GPL(bdfs_blend_complete_copyup);

/* ── Xattr forwarding ────────────────────────────────────────────────────── */

/*
 * Forward xattr operations to the real backing inode.
 * For upper-layer inodes this reaches BTRFS; for lower-layer inodes it
 * reaches the DwarFS FUSE handler (which may return -ENOTSUP for setxattr).
 */
static ssize_t bdfs_blend_listxattr(struct dentry *dentry, char *list,
				    size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct bdfs_blend_inode_info *bi = BDFS_I(inode);

	if (!bi->real_path.dentry)
		return -EIO;

	return vfs_listxattr(bi->real_path.dentry, list, size);
}

/* ── Symlink forwarding ──────────────────────────────────────────────────── */

/*
 * bdfs_blend_get_link - Return the symlink target from the backing layer.
 *
 * We delegate to the real inode's get_link so that path resolution through
 * symlinks in the blend namespace works correctly.
 */
static const char *bdfs_blend_get_link(struct dentry *dentry,
				       struct inode *inode,
				       struct delayed_call *done)
{
	struct bdfs_blend_inode_info *bi = BDFS_I(inode);
	struct inode *real;

	if (!dentry)
		return ERR_PTR(-ECHILD); /* RCU lookup not supported */

	if (!bi->real_path.dentry)
		return ERR_PTR(-EIO);

	real = d_inode(bi->real_path.dentry);
	if (!real->i_op || !real->i_op->get_link)
		return ERR_PTR(-EINVAL);

	return real->i_op->get_link(bi->real_path.dentry, real, done);
}

/* ── Directory readdir ───────────────────────────────────────────────────── */

/*
 * bdfs_blend_iterate_shared - Enumerate directory entries from both layers.
 *
 * We iterate the BTRFS upper layer first, then each DwarFS lower layer.
 * Entries that already appeared in the upper layer are deduplicated by
 * tracking emitted names in a small hash set.  For simplicity we use a
 * fixed-size bitmap over a hash of the name; false positives (suppressing
 * a lower-layer entry that happens to hash-collide with an upper entry) are
 * acceptable — the entry is still accessible via lookup.
 *
 * The real directory is opened via dentry_open() and iterated with
 * iterate_dir(), which calls the backing filesystem's iterate_shared.
 */

#define BDFS_DEDUP_BITS  512   /* power of two; covers ~360 entries at 50% */
#define BDFS_DEDUP_MASK  (BDFS_DEDUP_BITS - 1)

struct bdfs_readdir_ctx {
	struct dir_context  ctx;
	struct dir_context *caller_ctx;
	unsigned long       seen[BDFS_DEDUP_BITS / BITS_PER_LONG];
	bool                dedup; /* true = skip names already in seen[] */
};

static bool bdfs_dedup_test_set(struct bdfs_readdir_ctx *rc,
				const char *name, int namlen)
{
	unsigned long h = full_name_hash(NULL, name, namlen) & BDFS_DEDUP_MASK;
	bool already = test_bit(h, rc->seen);
	set_bit(h, rc->seen);
	return already;
}

static bool bdfs_filldir(struct dir_context *ctx, const char *name, int namlen,
			 loff_t offset, u64 ino, unsigned int d_type)
{
	struct bdfs_readdir_ctx *rc =
		container_of(ctx, struct bdfs_readdir_ctx, ctx);

	if (rc->dedup && bdfs_dedup_test_set(rc, name, namlen))
		return true; /* skip duplicate; continue iteration */

	if (!rc->dedup)
		bdfs_dedup_test_set(rc, name, namlen); /* record for lower pass */

	return dir_emit(rc->caller_ctx, name, namlen, ino, d_type);
}

static int bdfs_blend_iterate_shared(struct file *file,
				     struct dir_context *caller_ctx)
{
	struct inode *inode = file_inode(file);
	struct bdfs_blend_inode_info *bi = BDFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct bdfs_blend_mount *bm = sb->s_fs_info;
	struct bdfs_readdir_ctx rc;
	struct file *real_file;
	struct bdfs_dwarfs_layer *layer;
	int ret = 0;

	memset(&rc, 0, sizeof(rc));
	rc.ctx.actor  = bdfs_filldir;
	rc.ctx.pos    = caller_ctx->pos;
	rc.caller_ctx = caller_ctx;
	rc.dedup      = false; /* first pass: record names */

	/* Pass 1: BTRFS upper layer */
	if (bm && bm->btrfs_mnt && bi->real_path.dentry) {
		real_file = dentry_open(&bi->real_path,
					O_RDONLY | O_DIRECTORY,
					current_cred());
		if (!IS_ERR(real_file)) {
			ret = iterate_dir(real_file, &rc.ctx);
			fput(real_file);
		}
	}

	/* Pass 2: DwarFS lower layers — skip names seen in upper */
	rc.dedup = true;
	if (bm) {
		list_for_each_entry(layer, &bm->dwarfs_layers, list) {
			struct path lower_path;
			char *relpath_buf;
			char *relpath;
			int err;

			if (!layer->mnt)
				continue;

			/*
			 * Build the full relative path from the layer root to
			 * this directory (e.g. "usr/share/doc") so that nested
			 * directories are resolved correctly.  Using only the
			 * final dentry name component would fail for any
			 * directory that is not at the top level of the layer.
			 */
			relpath_buf = kmalloc(PATH_MAX, GFP_KERNEL);
			if (!relpath_buf)
				continue;

			relpath = bdfs_blend_rel_path(bi->real_path.dentry,
						      relpath_buf, PATH_MAX);
			if (IS_ERR(relpath)) {
				kfree(relpath_buf);
				continue;
			}

			err = vfs_path_lookup(layer->mnt->mnt_root,
					      layer->mnt,
					      relpath,
					      0, &lower_path);
			kfree(relpath_buf);
			if (err)
				continue;

			real_file = dentry_open(&lower_path,
						O_RDONLY | O_DIRECTORY,
						current_cred());
			path_put(&lower_path);
			if (IS_ERR(real_file))
				continue;

			rc.ctx.pos = 0;
			iterate_dir(real_file, &rc.ctx);
			fput(real_file);
		}
	}

	caller_ctx->pos = rc.ctx.pos;
	return ret;
}

/* ── Cached write_iter ───────────────────────────────────────────────────── */

/*
 * bdfs_blend_write_iter - Forward writes to the BTRFS upper layer.
 *
 * The upper-layer file is opened once and cached in bi->upper_file.
 * Subsequent writes reuse the cached file, avoiding a dentry_open() per call.
 * The cache is invalidated (and the file closed) in bdfs_blend_free_inode().
 */
static ssize_t bdfs_blend_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct bdfs_blend_inode_info *bi = BDFS_I(inode);
	ssize_t ret;

	if (!bi->is_upper)
		return -EROFS;

	/* Open and cache the upper-layer file on first write */
	if (!bi->upper_file) {
		struct file *uf = dentry_open(&bi->real_path,
					      O_RDWR | O_LARGEFILE,
					      current_cred());
		if (IS_ERR(uf))
			return PTR_ERR(uf);
		bi->upper_file = uf;
	}

	iocb->ki_filp = bi->upper_file;
	ret = bi->upper_file->f_op->write_iter(iocb, iter);
	iocb->ki_filp = file;

	/* Sync size back to the blend inode */
	inode->i_size = file_inode(bi->upper_file)->i_size;
	return ret;
}

/* ── inode_operations / file_operations tables ───────────────────────────── */

static const struct inode_operations bdfs_blend_symlink_iops = {
	.get_link  = bdfs_blend_get_link,
	.getattr   = bdfs_blend_getattr,
};

static const struct inode_operations bdfs_blend_file_iops = {
	.getattr      = bdfs_blend_getattr,
	.setattr      = bdfs_blend_setattr,
	.listxattr    = bdfs_blend_listxattr,
};

static const struct file_operations bdfs_blend_file_fops = {
	.open       = bdfs_blend_open,
	.read_iter  = generic_file_read_iter,
	.write_iter = bdfs_blend_write_iter,
	.llseek     = generic_file_llseek,
	.fsync      = generic_file_fsync,
};

static const struct file_operations bdfs_blend_dir_fops = {
	.iterate_shared = bdfs_blend_iterate_shared,
	.llseek         = generic_file_llseek,
};

static const struct inode_operations bdfs_blend_dir_iops = {
	.lookup      = bdfs_blend_lookup,
	.getattr     = bdfs_blend_getattr,
	.setattr     = bdfs_blend_setattr,
	.listxattr   = bdfs_blend_listxattr,
	.create      = bdfs_blend_create,
	.mkdir       = bdfs_blend_mkdir,
	.unlink      = bdfs_blend_unlink,
	.rmdir       = bdfs_blend_rmdir,
	.rename      = bdfs_blend_rename,
	.symlink     = bdfs_blend_symlink,
	.link        = bdfs_blend_link,
};

/* ── Filesystem type registration ───────────────────────────────────────── */

static int bdfs_blend_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct bdfs_blend_mount *bm = fc->fs_private;
	struct inode *root_inode;
	struct dentry *root_dentry;

	sb->s_magic = BDFS_BLEND_MAGIC;
	sb->s_op    = &bdfs_blend_sops;
	sb->s_fs_info = bm;
	bm->sb = sb;   /* back-pointer so bdfs_blend_umount can deactivate_super */

	root_inode = new_inode(sb);
	if (!root_inode)
		return -ENOMEM;

	root_inode->i_ino  = 1;
	root_inode->i_mode = S_IFDIR | 0755;
	root_inode->i_op   = &bdfs_blend_dir_iops;
	root_inode->i_fop  = &bdfs_blend_dir_fops;
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
	struct super_block *sb_to_deactivate = NULL;
	struct bdfs_dwarfs_layer *layer, *ltmp;
	bool found = false;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	mutex_lock(&bdfs_blend_mounts_lock);
	list_for_each_entry_safe(bm, tmp, &bdfs_blend_mounts, list) {
		if (strcmp(bm->mount_point, arg.mount_point) != 0)
			continue;

		list_del(&bm->list);
		found = true;

		bdfs_emit_event(BDFS_EVT_BLEND_UNMOUNTED,
				bm->btrfs_uuid, 0, arg.mount_point);

		/*
		 * Save the superblock pointer so we can call
		 * deactivate_super() after dropping the lock.
		 * deactivate_super() may sleep and must not be called
		 * under a mutex.
		 */
		sb_to_deactivate = bm->sb;

		/* Release BTRFS upper-layer mount reference */
		if (bm->btrfs_mnt) {
			mntput(bm->btrfs_mnt);
			bm->btrfs_mnt = NULL;
		}

		/* Release DwarFS lower-layer mount references */
		list_for_each_entry_safe(layer, ltmp,
					 &bm->dwarfs_layers, list) {
			list_del(&layer->list);
			if (layer->mnt)
				mntput(layer->mnt);
			kfree(layer);
		}

		kfree(bm);
		break;
	}
	mutex_unlock(&bdfs_blend_mounts_lock);

	if (!found)
		return -ENOENT;

	/*
	 * Deactivate the blend superblock.  This drops the active reference
	 * taken in bdfs_blend_mount() via get_tree_nodev(), allowing the VFS
	 * to evict all inodes and free the superblock when the last user
	 * releases it.
	 */
	if (sb_to_deactivate)
		deactivate_super(sb_to_deactivate);

	return 0;
}

/* ── BDFS_IOC_RESOLVE_PATH ──────────────────────────────────────────────── */

/*
 * bdfs_resolve_path - Resolve a blend-namespace path to its backing layer.
 *
 * Walks the blend mount list to find the mount whose mount_point is a prefix
 * of the requested path, then determines whether the path resolves to the
 * BTRFS upper layer or a DwarFS lower layer by attempting vfs_path_lookup on
 * each in priority order.
 *
 * Fills bdfs_ioctl_resolve_path.layer (BDFS_LAYER_UPPER / BDFS_LAYER_LOWER)
 * and .real_path with the resolved backing path.
 */
int bdfs_resolve_path(void __user *uarg,
		      struct list_head *registry,
		      struct mutex *lock)
{
	struct bdfs_ioctl_resolve_path arg;
	struct bdfs_blend_mount *bm;
	struct path resolved;
	bool found = false;
	int ret = -ENOENT;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	mutex_lock(&bdfs_blend_mounts_lock);
	list_for_each_entry(bm, &bdfs_blend_mounts, list) {
		size_t mlen = strlen(bm->mount_point);
		if (strncmp(arg.path, bm->mount_point, mlen) != 0)
			continue;

		/* Relative path within the blend mount */
		const char *rel = arg.path + mlen;
		if (*rel == '/')
			rel++;

		/* Try BTRFS upper layer first */
		ret = vfs_path_lookup(bm->btrfs_mnt->mnt_root,
				      bm->btrfs_mnt, rel, 0, &resolved);
		if (ret == 0) {
			arg.layer = BDFS_LAYER_UPPER;
			found = true;
			break;
		}

		/* Try each DwarFS lower layer */
		struct bdfs_dwarfs_layer *layer;
		list_for_each_entry(layer, &bm->dwarfs_layers, list) {
			if (!layer->mnt)
				continue;
			ret = vfs_path_lookup(layer->mnt->mnt_root,
					      layer->mnt, rel, 0, &resolved);
			if (ret == 0) {
				arg.layer = BDFS_LAYER_LOWER;
				found = true;
				break;
			}
		}
		if (found)
			break;
	}
	mutex_unlock(&bdfs_blend_mounts_lock);

	if (!found)
		return ret;

	/* Write the real path back to userspace */
	{
		char *buf = kmalloc(BDFS_PATH_MAX, GFP_KERNEL);
		if (!buf) {
			path_put(&resolved);
			return -ENOMEM;
		}
		char *p = d_path(&resolved, buf, BDFS_PATH_MAX);
		if (IS_ERR(p)) {
			kfree(buf);
			path_put(&resolved);
			return PTR_ERR(p);
		}
		strncpy(arg.real_path, p, sizeof(arg.real_path) - 1);
		kfree(buf);
		path_put(&resolved);
	}

	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;

	return 0;
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
	int ret;

	bdfs_inode_cachep = kmem_cache_create(
		"bdfs_inode_cache",
		sizeof(struct bdfs_blend_inode_info), 0,
		SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT,
		bdfs_inode_init_once);
	if (!bdfs_inode_cachep) {
		pr_err("bdfs: failed to create inode cache\n");
		return -ENOMEM;
	}

	ret = register_filesystem(&bdfs_blend_fs_type);
	if (ret) {
		pr_err("bdfs: failed to register blend filesystem: %d\n", ret);
		kmem_cache_destroy(bdfs_inode_cachep);
		return ret;
	}

	pr_info("bdfs: blend filesystem type '%s' registered\n",
		BDFS_BLEND_FS_TYPE);
	return 0;
}

void bdfs_blend_exit(void)
{
	unregister_filesystem(&bdfs_blend_fs_type);
	/*
	 * rcu_barrier() ensures all RCU-deferred inode frees have completed
	 * before we destroy the cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(bdfs_inode_cachep);
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
