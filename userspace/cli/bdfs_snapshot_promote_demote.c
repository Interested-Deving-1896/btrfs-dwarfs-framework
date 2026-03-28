// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_snapshot_promote_demote.c - snapshot, promote, demote subcommands
 *
 *   bdfs snapshot  --partition <uuid> --image-id <id>
 *                  --name <snapshot-name>  [--readonly]
 *
 *   bdfs promote   --blend-path <path> --subvol-name <name>
 *
 *   bdfs demote    --blend-path <path> --image-name <name>
 *                  [--compression zstd|lzma|lz4|brotli|none]
 *                  [--delete-subvol]
 *
 * Promote/demote operate on paths within a mounted blend namespace.
 * They are the primary mechanism for moving data between the live
 * (writable BTRFS) and archived (compressed DwarFS) tiers.
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

/* ── snapshot ───────────────────────────────────────────────────────────── */

int cmd_snapshot(struct bdfs_cli *cli, int argc, char *argv[])
{
	struct bdfs_ioctl_snapshot_dwarfs_container arg;
	int opt, ret;

	static const struct option opts[] = {
		{ "partition", required_argument, NULL, 'p' },
		{ "image-id",  required_argument, NULL, 'I' },
		{ "name",      required_argument, NULL, 'n' },
		{ "readonly",  no_argument,       NULL, 'r' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	memset(&arg, 0, sizeof(arg));

	while ((opt = getopt_long(argc, argv, "p:I:n:rh", opts, NULL)) != -1) {
		switch (opt) {
		case 'p':
			if (bdfs_str_to_uuid(optarg, arg.partition_uuid) < 0) {
				bdfs_err("invalid partition UUID: %s", optarg);
				return 1;
			}
			break;
		case 'I': arg.image_id = (uint64_t)strtoull(optarg, NULL, 0); break;
		case 'n':
			strncpy(arg.snapshot_name, optarg,
				sizeof(arg.snapshot_name) - 1);
			break;
		case 'r': arg.flags |= BDFS_SNAP_READONLY; break;
		case 'h':
			printf("Usage: bdfs snapshot --partition <uuid> "
			       "--image-id <id> --name <snapshot-name> "
			       "[--readonly]\n"
			       "\n"
			       "Creates a BTRFS snapshot of the subvolume that "
			       "contains the specified DwarFS image.\n"
			       "The snapshot captures the image file at its "
			       "current state via BTRFS CoW.\n");
			return 0;
		default: return 1;
		}
	}

	if (!arg.snapshot_name[0]) { bdfs_err("--name is required"); return 1; }

	ret = bdfs_cli_open_ctl(cli);
	if (ret) return 1;

	if (ioctl(cli->ctl_fd, BDFS_IOC_SNAPSHOT_DWARFS_CONTAINER, &arg) < 0) {
		bdfs_err("BDFS_IOC_SNAPSHOT_DWARFS_CONTAINER: %s",
			 strerror(errno));
		return 1;
	}

	if (cli->json_output)
		printf("{\"snapshot_subvol_id\":%llu}\n",
		       (unsigned long long)arg.snapshot_subvol_id_out);
	else
		printf("Snapshot '%s' created (subvol ID: %llu)\n",
		       arg.snapshot_name,
		       (unsigned long long)arg.snapshot_subvol_id_out);

	return 0;
}

/* ── promote ────────────────────────────────────────────────────────────── */

/*
 * Promote a DwarFS-backed path in the blend namespace to a writable BTRFS
 * subvolume.  The DwarFS image is extracted into a new subvolume, making
 * the data fully writable.  The DwarFS image remains in place as the lower
 * layer until explicitly removed.
 */
int cmd_promote(struct bdfs_cli *cli, int argc, char *argv[])
{
	struct bdfs_ioctl_promote_to_btrfs arg;
	int opt, ret;

	static const struct option opts[] = {
		{ "blend-path",  required_argument, NULL, 'P' },
		{ "subvol-name", required_argument, NULL, 'n' },
		{ "help",        no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	memset(&arg, 0, sizeof(arg));

	while ((opt = getopt_long(argc, argv, "P:n:h", opts, NULL)) != -1) {
		switch (opt) {
		case 'P':
			strncpy(arg.blend_path, optarg,
				sizeof(arg.blend_path) - 1);
			break;
		case 'n':
			strncpy(arg.subvol_name, optarg,
				sizeof(arg.subvol_name) - 1);
			break;
		case 'h':
			printf("Usage: bdfs promote --blend-path <path> "
			       "--subvol-name <name>\n"
			       "\n"
			       "Extracts the DwarFS image at <path> in the blend "
			       "namespace into a new writable BTRFS subvolume.\n"
			       "The path becomes writable; the DwarFS lower layer "
			       "remains until 'bdfs demote' is run.\n");
			return 0;
		default: return 1;
		}
	}

	if (!arg.blend_path[0])  { bdfs_err("--blend-path is required");  return 1; }
	if (!arg.subvol_name[0]) { bdfs_err("--subvol-name is required"); return 1; }

	ret = bdfs_cli_open_ctl(cli);
	if (ret) return 1;

	if (!cli->json_output)
		printf("Promoting '%s' → BTRFS subvolume '%s'...\n",
		       arg.blend_path, arg.subvol_name);

	if (ioctl(cli->ctl_fd, BDFS_IOC_PROMOTE_TO_BTRFS, &arg) < 0) {
		bdfs_err("BDFS_IOC_PROMOTE_TO_BTRFS: %s", strerror(errno));
		return 1;
	}

	if (cli->json_output)
		printf("{\"subvol_id\":%llu}\n",
		       (unsigned long long)arg.subvol_id_out);
	else
		printf("Promoted. New subvolume ID: %llu\n",
		       (unsigned long long)arg.subvol_id_out);

	return 0;
}

/* ── demote ─────────────────────────────────────────────────────────────── */

/*
 * Demote a BTRFS subvolume in the blend namespace to a compressed DwarFS
 * image.  This is the primary archival operation: live data is compressed
 * and moved to the DwarFS lower layer, freeing BTRFS space.
 *
 * With --delete-subvol the BTRFS subvolume is removed after the image is
 * successfully created, completing the tier-down.
 */
int cmd_demote(struct bdfs_cli *cli, int argc, char *argv[])
{
	struct bdfs_ioctl_demote_to_dwarfs arg;
	int opt, ret;

	static const struct option opts[] = {
		{ "blend-path",    required_argument, NULL, 'P' },
		{ "image-name",    required_argument, NULL, 'n' },
		{ "compression",   required_argument, NULL, 'c' },
		{ "delete-subvol", no_argument,       NULL, 'd' },
		{ "help",          no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	memset(&arg, 0, sizeof(arg));
	arg.compression = BDFS_COMPRESS_ZSTD;

	while ((opt = getopt_long(argc, argv, "P:n:c:dh", opts, NULL)) != -1) {
		switch (opt) {
		case 'P':
			strncpy(arg.blend_path, optarg,
				sizeof(arg.blend_path) - 1);
			break;
		case 'n':
			strncpy(arg.image_name, optarg,
				sizeof(arg.image_name) - 1);
			break;
		case 'c':
			arg.compression = bdfs_compression_from_name(optarg);
			break;
		case 'd':
			arg.flags |= BDFS_DEMOTE_DELETE_SUBVOL;
			break;
		case 'h':
			printf("Usage: bdfs demote --blend-path <path> "
			       "--image-name <name>\n"
			       "  [--compression zstd|lzma|lz4|brotli|none]\n"
			       "  [--delete-subvol]\n"
			       "\n"
			       "Compresses the BTRFS subvolume at <path> into a "
			       "DwarFS image.\n"
			       "--delete-subvol removes the BTRFS subvolume after "
			       "successful image creation.\n");
			return 0;
		default: return 1;
		}
	}

	if (!arg.blend_path[0])  { bdfs_err("--blend-path is required");  return 1; }
	if (!arg.image_name[0])  { bdfs_err("--image-name is required");  return 1; }

	ret = bdfs_cli_open_ctl(cli);
	if (ret) return 1;

	if (!cli->json_output)
		printf("Demoting '%s' → DwarFS image '%s' "
		       "(compression: %s)%s...\n",
		       arg.blend_path, arg.image_name,
		       bdfs_compression_name(arg.compression),
		       (arg.flags & BDFS_DEMOTE_DELETE_SUBVOL)
		           ? ", will delete subvol" : "");

	if (ioctl(cli->ctl_fd, BDFS_IOC_DEMOTE_TO_DWARFS, &arg) < 0) {
		bdfs_err("BDFS_IOC_DEMOTE_TO_DWARFS: %s", strerror(errno));
		return 1;
	}

	if (cli->json_output)
		printf("{\"image_id\":%llu}\n",
		       (unsigned long long)arg.image_id_out);
	else
		printf("Demote queued. Image ID: %llu\n",
		       (unsigned long long)arg.image_id_out);

	return 0;
}
