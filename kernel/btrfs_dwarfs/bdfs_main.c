// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_main.c - BTRFS+DwarFS framework kernel module entry point
 *
 * Registers the /dev/bdfs_ctl control device, the netlink event socket,
 * and the partition registry.  The actual filesystem operations are
 * delegated to the blend VFS layer (bdfs_blend.c) and the partition
 * backends (bdfs_btrfs_part.c, bdfs_dwarfs_part.c).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/uuid.h>
#include <linux/miscdevice.h>
#include <linux/netlink.h>
#include <net/sock.h>

#include "../../include/uapi/bdfs_ioctl.h"
#include "bdfs_internal.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("BTRFS+DwarFS Framework Contributors");
MODULE_DESCRIPTION("BTRFS and DwarFS hybrid filesystem framework");
MODULE_VERSION("1.0.0");

/* Global partition registry */
static DEFINE_MUTEX(bdfs_registry_lock);
static LIST_HEAD(bdfs_partition_list);
static atomic_t bdfs_partition_count = ATOMIC_INIT(0);

/* Netlink socket for userspace event delivery */
static struct sock *bdfs_nl_sock;
#define BDFS_NETLINK_PROTO  31

/* ── Partition registry ─────────────────────────────────────────────────── */

struct bdfs_partition_entry {
	struct list_head        list;
	struct bdfs_partition   desc;
	struct bdfs_part_ops   *ops;        /* backend operations */
	void                   *private;    /* backend private data */
	struct kref             refcount;
};

static struct bdfs_partition_entry *
bdfs_find_partition_locked(const u8 uuid[16])
{
	struct bdfs_partition_entry *entry;

	list_for_each_entry(entry, &bdfs_partition_list, list) {
		if (memcmp(entry->desc.uuid, uuid, 16) == 0)
			return entry;
	}
	return NULL;
}

static int bdfs_register_partition(struct bdfs_ioctl_register_partition __user *uarg)
{
	struct bdfs_ioctl_register_partition arg;
	struct bdfs_partition_entry *entry;
	int ret = 0;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	if (arg.part.magic != BDFS_MAGIC)
		return -EINVAL;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	memcpy(&entry->desc, &arg.part, sizeof(arg.part));

	/* Assign a UUID if not provided */
	if (memchr_inv(entry->desc.uuid, 0, 16) == NULL)
		generate_random_uuid(entry->desc.uuid);

	kref_init(&entry->refcount);

	/* Select backend operations based on partition type */
	switch (entry->desc.type) {
	case BDFS_PART_DWARFS_BACKED:
		entry->ops = &bdfs_dwarfs_part_ops;
		break;
	case BDFS_PART_BTRFS_BACKED:
		entry->ops = &bdfs_btrfs_part_ops;
		break;
	case BDFS_PART_HYBRID_BLEND:
		entry->ops = &bdfs_blend_part_ops;
		break;
	default:
		kfree(entry);
		return -EINVAL;
	}

	ret = entry->ops->init(entry);
	if (ret) {
		kfree(entry);
		return ret;
	}

	mutex_lock(&bdfs_registry_lock);
	list_add_tail(&entry->list, &bdfs_partition_list);
	atomic_inc(&bdfs_partition_count);
	mutex_unlock(&bdfs_registry_lock);

	/* Return the UUID to userspace */
	if (copy_to_user(arg.uuid_out, entry->desc.uuid, 16)) {
		/* Best-effort; partition is registered regardless */
		ret = -EFAULT;
	}

	bdfs_emit_event(BDFS_EVT_PARTITION_ADDED, entry->desc.uuid, 0, NULL);
	return ret;
}

static int bdfs_unregister_partition(struct bdfs_ioctl_unregister_partition __user *uarg)
{
	struct bdfs_ioctl_unregister_partition arg;
	struct bdfs_partition_entry *entry;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	mutex_lock(&bdfs_registry_lock);
	entry = bdfs_find_partition_locked(arg.uuid);
	if (!entry) {
		mutex_unlock(&bdfs_registry_lock);
		return -ENOENT;
	}
	list_del(&entry->list);
	atomic_dec(&bdfs_partition_count);
	mutex_unlock(&bdfs_registry_lock);

	if (entry->ops->destroy)
		entry->ops->destroy(entry);

	bdfs_emit_event(BDFS_EVT_PARTITION_REMOVED, entry->desc.uuid, 0, NULL);
	kfree(entry);
	return 0;
}

/* ── Netlink event emission ─────────────────────────────────────────────── */

void bdfs_emit_event(enum bdfs_event_type type, const u8 uuid[16],
		     u64 object_id, const char *message)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct bdfs_event *evt;
	int msg_size = sizeof(*evt);

	if (!bdfs_nl_sock)
		return;

	skb = nlmsg_new(msg_size, GFP_KERNEL);
	if (!skb)
		return;

	nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, msg_size, 0);
	if (!nlh) {
		kfree_skb(skb);
		return;
	}

	evt = nlmsg_data(nlh);
	memset(evt, 0, sizeof(*evt));
	evt->type = type;
	if (uuid)
		memcpy(evt->partition_uuid, uuid, 16);
	evt->object_id = object_id;
	evt->timestamp_ns = ktime_get_real_ns();
	if (message)
		strscpy(evt->message, message, sizeof(evt->message));

	/* Broadcast to all listeners in the BDFS netlink group */
	nlmsg_multicast(bdfs_nl_sock, skb, 0, 1, GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(bdfs_emit_event);

/* ── Control device file operations ────────────────────────────────────── */

static long bdfs_ctl_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case BDFS_IOC_REGISTER_PARTITION:
		return bdfs_register_partition(uarg);

	case BDFS_IOC_UNREGISTER_PARTITION:
		return bdfs_unregister_partition(uarg);

	case BDFS_IOC_LIST_PARTITIONS:
		return bdfs_list_partitions(uarg,
					    &bdfs_partition_list,
					    &bdfs_registry_lock);

	case BDFS_IOC_MOUNT_BLEND:
		return bdfs_blend_mount(uarg, &bdfs_partition_list,
					&bdfs_registry_lock);

	case BDFS_IOC_UMOUNT_BLEND:
		return bdfs_blend_umount(uarg);

	/* DwarFS-backed partition ioctls */
	case BDFS_IOC_EXPORT_TO_DWARFS:
		return bdfs_dwarfs_export(uarg, &bdfs_partition_list,
					  &bdfs_registry_lock);

	case BDFS_IOC_MOUNT_DWARFS_IMAGE:
		return bdfs_dwarfs_mount_image(uarg, &bdfs_partition_list,
					       &bdfs_registry_lock);

	case BDFS_IOC_UMOUNT_DWARFS_IMAGE:
		return bdfs_dwarfs_umount_image(uarg, &bdfs_partition_list,
						&bdfs_registry_lock);

	case BDFS_IOC_LIST_DWARFS_IMAGES:
		return bdfs_dwarfs_list_images(uarg, &bdfs_partition_list,
					       &bdfs_registry_lock);

	/* BTRFS-backed partition ioctls */
	case BDFS_IOC_IMPORT_FROM_DWARFS:
		return bdfs_btrfs_import(uarg, &bdfs_partition_list,
					 &bdfs_registry_lock);

	case BDFS_IOC_STORE_DWARFS_IMAGE:
		return bdfs_btrfs_store_image(uarg, &bdfs_partition_list,
					      &bdfs_registry_lock);

	case BDFS_IOC_SNAPSHOT_DWARFS_CONTAINER:
		return bdfs_btrfs_snapshot_container(uarg,
						      &bdfs_partition_list,
						      &bdfs_registry_lock);

	case BDFS_IOC_LIST_BTRFS_SUBVOLS:
		return bdfs_btrfs_list_subvols(uarg, &bdfs_partition_list,
					       &bdfs_registry_lock);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations bdfs_ctl_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = bdfs_ctl_ioctl,
	.compat_ioctl   = bdfs_ctl_ioctl,
	.llseek         = noop_llseek,
};

static struct miscdevice bdfs_ctl_dev = {
	.minor  = MISC_DYNAMIC_MINOR,
	.name   = "bdfs_ctl",
	.fops   = &bdfs_ctl_fops,
	.mode   = 0600,
};

/* ── Module init / exit ─────────────────────────────────────────────────── */

static struct netlink_kernel_cfg bdfs_nl_cfg = {
	.groups = 1,
};

static int __init bdfs_init(void)
{
	int ret;

	pr_info("bdfs: BTRFS+DwarFS framework v%d.%d initialising\n",
		BDFS_VERSION_MAJOR, BDFS_VERSION_MINOR);

	bdfs_nl_sock = netlink_kernel_create(&init_net, BDFS_NETLINK_PROTO,
					     &bdfs_nl_cfg);
	if (!bdfs_nl_sock) {
		pr_err("bdfs: failed to create netlink socket\n");
		return -ENOMEM;
	}

	ret = misc_register(&bdfs_ctl_dev);
	if (ret) {
		pr_err("bdfs: failed to register control device: %d\n", ret);
		netlink_kernel_release(bdfs_nl_sock);
		return ret;
	}

	ret = bdfs_blend_init();
	if (ret) {
		pr_err("bdfs: blend layer init failed: %d\n", ret);
		misc_deregister(&bdfs_ctl_dev);
		netlink_kernel_release(bdfs_nl_sock);
		return ret;
	}

	pr_info("bdfs: control device /dev/bdfs_ctl registered\n");
	return 0;
}

static void __exit bdfs_exit(void)
{
	struct bdfs_partition_entry *entry, *tmp;

	bdfs_blend_exit();

	mutex_lock(&bdfs_registry_lock);
	list_for_each_entry_safe(entry, tmp, &bdfs_partition_list, list) {
		list_del(&entry->list);
		if (entry->ops->destroy)
			entry->ops->destroy(entry);
		kfree(entry);
	}
	mutex_unlock(&bdfs_registry_lock);

	misc_deregister(&bdfs_ctl_dev);
	netlink_kernel_release(bdfs_nl_sock);
	pr_info("bdfs: unloaded\n");
}

module_init(bdfs_init);
module_exit(bdfs_exit);
