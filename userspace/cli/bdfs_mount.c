// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_mount.c - mount, umount, blend mount, blend umount subcommands
 *
 *   bdfs mount  --partition <uuid> --image-id <id>
 *               --mountpoint <path>  [--cache-mb <n>]
 *
 *   bdfs umount --partition <uuid> --image-id <id>  [--force]
 *
 *   bdfs blend mount  --btrfs-uuid <uuid> --dwarfs-uuid <uuid>
 *                     --mountpoint <path>
 *                     [--compression zstd|...]  [--cache-mb <n>]
 *                     [--writeback]  [--lazy-load]
 *
 *   bdfs blend umount --mountpoint <path>  [--force]  [--lazy]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "bdfs.h"

/* ── mount ──────────────────────────────────────────────────────────────── */

int cmd_mount(struct bdfs_cli *cli, int argc, char *argv[])
{
	struct bdfs_ioctl_mount_dwarfs_image arg;
	int opt, ret;

	static const struct option opts[] = {
		{ "partition",  required_argument, NULL, 'p' },
		{ "image-id",   required_argument, NULL, 'I' },
		{ "mountpoint", required_argument, NULL, 'm' },
		{ "cache-mb",   required_argument, NULL, 'c' },
		{ "help",       no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	memset(&arg, 0, sizeof(arg));
	arg.cache_size_mb = 256;

	while ((opt = getopt_long(argc, argv, "p:I:m:c:h", opts, NULL)) != -1) {
		switch (opt) {
		case 'p':
			if (bdfs_str_to_uuid(optarg, arg.partition_uuid) < 0) {
				bdfs_err("invalid partition UUID: %s", optarg);
				return 1;
			}
			break;
		case 'I': arg.image_id = (uint64_t)strtoull(optarg, NULL, 0); break;
		case 'm': strncpy(arg.mount_point, optarg, sizeof(arg.mount_point) - 1); break;
		case 'c': arg.cache_size_mb = (uint32_t)atoi(optarg); break;
		case 'h':
			printf("Usage: bdfs mount --partition <uuid> "
			       "--image-id <id> --mountpoint <path> "
			       "[--cache-mb <n>]\n");
			return 0;
		default: return 1;
		}
	}

	if (!arg.mount_point[0]) { bdfs_err("--mountpoint is required"); return 1; }

	ret = bdfs_cli_open_ctl(cli);
	if (ret) return 1;

	if (ioctl(cli->ctl_fd, BDFS_IOC_MOUNT_DWARFS_IMAGE, &arg) < 0) {
		bdfs_err("BDFS_IOC_MOUNT_DWARFS_IMAGE: %s", strerror(errno));
		return 1;
	}

	if (!cli->json_output)
		printf("Mount requested: image %llu → %s\n",
		       (unsigned long long)arg.image_id, arg.mount_point);
	return 0;
}

/* ── umount ─────────────────────────────────────────────────────────────── */

int cmd_umount(struct bdfs_cli *cli, int argc, char *argv[])
{
	struct bdfs_ioctl_umount_dwarfs_image arg;
	int opt, ret;

	static const struct option opts[] = {
		{ "partition", required_argument, NULL, 'p' },
		{ "image-id",  required_argument, NULL, 'I' },
		{ "force",     no_argument,       NULL, 'f' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	memset(&arg, 0, sizeof(arg));

	while ((opt = getopt_long(argc, argv, "p:I:fh", opts, NULL)) != -1) {
		switch (opt) {
		case 'p':
			if (bdfs_str_to_uuid(optarg, arg.partition_uuid) < 0) {
				bdfs_err("invalid partition UUID: %s", optarg);
				return 1;
			}
			break;
		case 'I': arg.image_id = (uint64_t)strtoull(optarg, NULL, 0); break;
		case 'f': arg.flags |= BDFS_UMOUNT_FORCE; break;
		case 'h':
			printf("Usage: bdfs umount --partition <uuid> "
			       "--image-id <id>  [--force]\n");
			return 0;
		default: return 1;
		}
	}

	ret = bdfs_cli_open_ctl(cli);
	if (ret) return 1;

	if (ioctl(cli->ctl_fd, BDFS_IOC_UMOUNT_DWARFS_IMAGE, &arg) < 0) {
		bdfs_err("BDFS_IOC_UMOUNT_DWARFS_IMAGE: %s", strerror(errno));
		return 1;
	}

	if (!cli->json_output)
		printf("Unmounted image %llu\n", (unsigned long long)arg.image_id);
	return 0;
}

/* ── blend mount ────────────────────────────────────────────────────────── */

int cmd_blend_mount(struct bdfs_cli *cli, int argc, char *argv[])
{
	struct bdfs_ioctl_mount_blend arg;
	int opt, ret;

	static const struct option opts[] = {
		{ "btrfs-uuid",  required_argument, NULL, 'B' },
		{ "dwarfs-uuid", required_argument, NULL, 'D' },
		{ "mountpoint",  required_argument, NULL, 'm' },
		{ "compression", required_argument, NULL, 'c' },
		{ "cache-mb",    required_argument, NULL, 'C' },
		{ "writeback",   no_argument,       NULL, 'w' },
		{ "lazy-load",   no_argument,       NULL, 'L' },
		{ "rdonly",      no_argument,       NULL, 'r' },
		{ "help",        no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	memset(&arg, 0, sizeof(arg));
	arg.opts.cache_size_mb  = 256;
	arg.opts.worker_threads = 4;

	while ((opt = getopt_long(argc, argv, "B:D:m:c:C:wLrh",
				  opts, NULL)) != -1) {
		switch (opt) {
		case 'B':
			if (bdfs_str_to_uuid(optarg, arg.btrfs_uuid) < 0) {
				bdfs_err("invalid BTRFS UUID: %s", optarg);
				return 1;
			}
			break;
		case 'D':
			if (bdfs_str_to_uuid(optarg, arg.dwarfs_uuid) < 0) {
				bdfs_err("invalid DwarFS UUID: %s", optarg);
				return 1;
			}
			break;
		case 'm':
			strncpy(arg.mount_point, optarg,
				sizeof(arg.mount_point) - 1);
			break;
		case 'c':
			arg.opts.compression = bdfs_compression_from_name(optarg);
			break;
		case 'C': arg.opts.cache_size_mb  = (uint32_t)atoi(optarg); break;
		case 'w': arg.opts.flags |= BDFS_MOUNT_WRITEBACK;  break;
		case 'L': arg.opts.flags |= BDFS_MOUNT_LAZY_LOAD;  break;
		case 'r': arg.opts.flags |= BDFS_MOUNT_RDONLY;     break;
		case 'h':
			printf("Usage: bdfs blend mount "
			       "--btrfs-uuid <uuid> --dwarfs-uuid <uuid> "
			       "--mountpoint <path>\n"
			       "  [--compression zstd|lzma|lz4|brotli|none]\n"
			       "  [--cache-mb <n>]  [--writeback]  "
			       "[--lazy-load]  [--rdonly]\n");
			return 0;
		default: return 1;
		}
	}

	if (!arg.mount_point[0]) { bdfs_err("--mountpoint is required"); return 1; }

	ret = bdfs_cli_open_ctl(cli);
	if (ret) return 1;

	if (ioctl(cli->ctl_fd, BDFS_IOC_MOUNT_BLEND, &arg) < 0) {
		bdfs_err("BDFS_IOC_MOUNT_BLEND: %s", strerror(errno));
		return 1;
	}

	if (!cli->json_output)
		printf("Blend mount requested at %s\n", arg.mount_point);
	return 0;
}

/* ── blend umount ───────────────────────────────────────────────────────── */

int cmd_blend_umount(struct bdfs_cli *cli, int argc, char *argv[])
{
	struct bdfs_ioctl_umount_blend arg;
	int opt, ret;

	static const struct option opts[] = {
		{ "mountpoint", required_argument, NULL, 'm' },
		{ "force",      no_argument,       NULL, 'f' },
		{ "lazy",       no_argument,       NULL, 'l' },
		{ "help",       no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	memset(&arg, 0, sizeof(arg));

	while ((opt = getopt_long(argc, argv, "m:flh", opts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			strncpy(arg.mount_point, optarg,
				sizeof(arg.mount_point) - 1);
			break;
		case 'f': arg.flags |= BDFS_UMOUNT_FORCE; break;
		case 'l': arg.flags |= BDFS_UMOUNT_LAZY;  break;
		case 'h':
			printf("Usage: bdfs blend umount --mountpoint <path> "
			       "[--force]  [--lazy]\n");
			return 0;
		default: return 1;
		}
	}

	/* Also accept positional mountpoint */
	if (!arg.mount_point[0] && optind < argc)
		strncpy(arg.mount_point, argv[optind],
			sizeof(arg.mount_point) - 1);

	if (!arg.mount_point[0]) { bdfs_err("--mountpoint is required"); return 1; }

	ret = bdfs_cli_open_ctl(cli);
	if (ret) return 1;

	if (ioctl(cli->ctl_fd, BDFS_IOC_UMOUNT_BLEND, &arg) < 0) {
		bdfs_err("BDFS_IOC_UMOUNT_BLEND: %s", strerror(errno));
		return 1;
	}

	if (!cli->json_output)
		printf("Blend unmounted: %s\n", arg.mount_point);
	return 0;
}
