// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_main.c - bdfs CLI entry point
 *
 * Usage:
 *   bdfs [global-opts] <command> [command-opts] [args...]
 *
 * Global options:
 *   -v, --verbose       Verbose output
 *   -j, --json          JSON output
 *   -C, --no-color      Disable ANSI colour
 *   -c, --ctl <dev>     Control device (default: /dev/bdfs_ctl)
 *   -s, --socket <path> Daemon socket (default: /run/bdfs/daemon.sock)
 *   -h, --help          Show help
 *   -V, --version       Show version
 *
 * Commands:
 *   partition add       Register a partition with the framework
 *   partition remove    Unregister a partition
 *   partition list      List registered partitions
 *   partition show      Show partition details
 *
 *   export              Export a BTRFS subvolume/snapshot to a DwarFS image
 *   import              Import a DwarFS image into a BTRFS subvolume
 *
 *   mount               Mount a DwarFS image
 *   umount              Unmount a DwarFS image
 *
 *   snapshot            Snapshot the BTRFS container of a DwarFS image
 *
 *   promote             Promote a DwarFS-backed path to a writable BTRFS subvolume
 *   demote              Demote a BTRFS subvolume to a compressed DwarFS image
 *
 *   blend mount         Mount the unified blend namespace
 *   blend umount        Unmount the blend namespace
 *
 *   status              Show daemon and framework status
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <getopt.h>
#include <sys/ioctl.h>

#include "bdfs.h"

/* ── Output helpers ─────────────────────────────────────────────────────── */

static bool g_verbose = false;
static bool g_no_color = false;

void bdfs_err(const char *fmt, ...)
{
	va_list ap;
	if (!g_no_color)
		fprintf(stderr, "\033[1;31merror:\033[0m ");
	else
		fprintf(stderr, "error: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void bdfs_info(const char *fmt, ...)
{
	va_list ap;
	if (!g_verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
}

/* ── UUID helpers ───────────────────────────────────────────────────────── */

void bdfs_uuid_to_str(const uint8_t uuid[16], char out[37])
{
	snprintf(out, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		 "%02x%02x%02x%02x%02x%02x",
		 uuid[0],  uuid[1],  uuid[2],  uuid[3],
		 uuid[4],  uuid[5],  uuid[6],  uuid[7],
		 uuid[8],  uuid[9],  uuid[10], uuid[11],
		 uuid[12], uuid[13], uuid[14], uuid[15]);
}

int bdfs_str_to_uuid(const char *str, uint8_t uuid[16])
{
	unsigned int b[16];
	int i;
	if (sscanf(str,
		   "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		   "%02x%02x%02x%02x%02x%02x",
		   &b[0],  &b[1],  &b[2],  &b[3],
		   &b[4],  &b[5],  &b[6],  &b[7],
		   &b[8],  &b[9],  &b[10], &b[11],
		   &b[12], &b[13], &b[14], &b[15]) != 16)
		return -1;
	for (i = 0; i < 16; i++)
		uuid[i] = (uint8_t)b[i];
	return 0;
}

/* ── Compression helpers ────────────────────────────────────────────────── */

const char *bdfs_compression_name(uint32_t c)
{
	switch (c) {
	case BDFS_COMPRESS_NONE:   return "none";
	case BDFS_COMPRESS_LZMA:   return "lzma";
	case BDFS_COMPRESS_ZSTD:   return "zstd";
	case BDFS_COMPRESS_LZ4:    return "lz4";
	case BDFS_COMPRESS_BROTLI: return "brotli";
	default:                   return "unknown";
	}
}

uint32_t bdfs_compression_from_name(const char *name)
{
	if (!strcmp(name, "none"))   return BDFS_COMPRESS_NONE;
	if (!strcmp(name, "lzma"))   return BDFS_COMPRESS_LZMA;
	if (!strcmp(name, "zstd"))   return BDFS_COMPRESS_ZSTD;
	if (!strcmp(name, "lz4"))    return BDFS_COMPRESS_LZ4;
	if (!strcmp(name, "brotli")) return BDFS_COMPRESS_BROTLI;
	return BDFS_COMPRESS_ZSTD; /* default */
}

/* ── Print helpers ──────────────────────────────────────────────────────── */

void bdfs_print_partition(const struct bdfs_partition *p, bool json)
{
	char uuid_str[37];
	const char *type_str;

	bdfs_uuid_to_str(p->uuid, uuid_str);

	switch (p->type) {
	case BDFS_PART_DWARFS_BACKED: type_str = "dwarfs-backed"; break;
	case BDFS_PART_BTRFS_BACKED:  type_str = "btrfs-backed";  break;
	case BDFS_PART_HYBRID_BLEND:  type_str = "hybrid-blend";  break;
	default:                      type_str = "unknown";        break;
	}

	if (json) {
		printf("{\"uuid\":\"%s\",\"label\":\"%s\",\"type\":\"%s\","
		       "\"device\":\"%s\",\"mount\":\"%s\"}\n",
		       uuid_str, p->label, type_str,
		       p->device_path, p->mount_point);
	} else {
		printf("  UUID:    %s\n", uuid_str);
		printf("  Label:   %s\n", p->label[0] ? p->label : "(none)");
		printf("  Type:    %s\n", type_str);
		printf("  Device:  %s\n", p->device_path);
		printf("  Mount:   %s\n", p->mount_point[0] ? p->mount_point : "(not mounted)");
		if (p->type == BDFS_PART_DWARFS_BACKED)
			printf("  Images:  %llu\n",
			       (unsigned long long)p->dwarfs_image_count);
		if (p->type == BDFS_PART_BTRFS_BACKED)
			printf("  Subvols: %llu  Snapshots: %llu\n",
			       (unsigned long long)p->btrfs_subvol_count,
			       (unsigned long long)p->btrfs_snapshot_count);
	}
}

void bdfs_print_image(const struct bdfs_dwarfs_image *img, bool json)
{
	char uuid_str[37];
	bdfs_uuid_to_str(img->uuid, uuid_str);

	if (json) {
		printf("{\"id\":%llu,\"name\":\"%s\",\"uuid\":\"%s\","
		       "\"size\":%llu,\"uncompressed\":%llu,"
		       "\"compression\":\"%s\",\"mounted\":%s,"
		       "\"mount_point\":\"%s\"}\n",
		       (unsigned long long)img->image_id, img->name, uuid_str,
		       (unsigned long long)img->size_bytes,
		       (unsigned long long)img->uncompressed_bytes,
		       bdfs_compression_name(img->compression),
		       img->mounted ? "true" : "false",
		       img->mount_point);
	} else {
		printf("  ID:           %llu\n", (unsigned long long)img->image_id);
		printf("  Name:         %s\n", img->name);
		printf("  UUID:         %s\n", uuid_str);
		printf("  Size:         %llu bytes\n", (unsigned long long)img->size_bytes);
		printf("  Uncompressed: %llu bytes\n", (unsigned long long)img->uncompressed_bytes);
		if (img->uncompressed_bytes > 0) {
			double ratio = (double)img->uncompressed_bytes /
				       (double)img->size_bytes;
			printf("  Ratio:        %.2fx\n", ratio);
		}
		printf("  Compression:  %s\n", bdfs_compression_name(img->compression));
		printf("  Mounted:      %s\n", img->mounted ? "yes" : "no");
		if (img->mounted)
			printf("  Mount point:  %s\n", img->mount_point);
		printf("  Backing:      %s\n", img->backing_path);
	}
}

void bdfs_print_subvol(const struct bdfs_btrfs_subvol *sv, bool json)
{
	char uuid_str[37];
	bdfs_uuid_to_str(sv->uuid, uuid_str);

	if (json) {
		printf("{\"id\":%llu,\"name\":\"%s\",\"uuid\":\"%s\","
		       "\"snapshot\":%s,\"readonly\":%s,\"path\":\"%s\"}\n",
		       (unsigned long long)sv->subvol_id, sv->name, uuid_str,
		       sv->is_snapshot ? "true" : "false",
		       sv->is_readonly ? "true" : "false",
		       sv->path);
	} else {
		printf("  ID:       %llu\n", (unsigned long long)sv->subvol_id);
		printf("  Name:     %s\n", sv->name);
		printf("  UUID:     %s\n", uuid_str);
		printf("  Snapshot: %s\n", sv->is_snapshot ? "yes" : "no");
		printf("  Readonly: %s\n", sv->is_readonly ? "yes" : "no");
		printf("  Path:     %s\n", sv->path);
	}
}

/* ── Control device ─────────────────────────────────────────────────────── */

int bdfs_cli_open_ctl(struct bdfs_cli *cli)
{
	if (cli->ctl_fd >= 0)
		return 0;

	cli->ctl_fd = open(cli->ctl_device, O_RDWR | O_CLOEXEC);
	if (cli->ctl_fd < 0) {
		bdfs_err("cannot open %s: %s", cli->ctl_device, strerror(errno));
		return -errno;
	}
	return 0;
}

void bdfs_cli_close(struct bdfs_cli *cli)
{
	if (cli->ctl_fd >= 0) {
		close(cli->ctl_fd);
		cli->ctl_fd = -1;
	}
	if (cli->sock_fd >= 0) {
		close(cli->sock_fd);
		cli->sock_fd = -1;
	}
}

/* ── Command dispatch table ─────────────────────────────────────────────── */

static void print_usage(void)
{
	printf(
"Usage: bdfs [OPTIONS] COMMAND [ARGS...]\n"
"\n"
"BTRFS+DwarFS hybrid filesystem management tool.\n"
"\n"
"Options:\n"
"  -v, --verbose          Verbose output\n"
"  -j, --json             JSON output\n"
"  -C, --no-color         Disable ANSI colour\n"
"  -c, --ctl <device>     Control device (default: /dev/bdfs_ctl)\n"
"  -s, --socket <path>    Daemon socket (default: /run/bdfs/daemon.sock)\n"
"  -h, --help             Show this help\n"
"  -V, --version          Show version\n"
"\n"
"Partition commands:\n"
"  partition add          Register a partition with the framework\n"
"  partition remove       Unregister a partition\n"
"  partition list         List registered partitions\n"
"  partition show         Show partition details\n"
"\n"
"Data movement:\n"
"  export                 Export a BTRFS subvolume/snapshot to a DwarFS image\n"
"  import                 Import a DwarFS image into a BTRFS subvolume\n"
"\n"
"Mount management:\n"
"  mount                  Mount a DwarFS image via FUSE\n"
"  umount                 Unmount a DwarFS image\n"
"  blend mount            Mount the unified BTRFS+DwarFS blend namespace\n"
"  blend umount           Unmount the blend namespace\n"
"\n"
"Snapshot / archive:\n"
"  snapshot               Snapshot the BTRFS container of a DwarFS image\n"
"  promote                Promote a DwarFS-backed path to a writable BTRFS subvolume\n"
"  demote                 Demote a BTRFS subvolume to a compressed DwarFS image\n"
"\n"
"Other:\n"
"  status                 Show daemon and framework status\n"
"\n"
"Run 'bdfs COMMAND --help' for command-specific help.\n"
	);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	struct bdfs_cli cli = {
		.ctl_fd  = -1,
		.sock_fd = -1,
	};
	int opt, ret;

	strncpy(cli.ctl_device,  "/dev/bdfs_ctl",          sizeof(cli.ctl_device) - 1);
	strncpy(cli.socket_path, "/run/bdfs/daemon.sock",   sizeof(cli.socket_path) - 1);

	static const struct option long_opts[] = {
		{ "verbose",  no_argument,       NULL, 'v' },
		{ "json",     no_argument,       NULL, 'j' },
		{ "no-color", no_argument,       NULL, 'C' },
		{ "ctl",      required_argument, NULL, 'c' },
		{ "socket",   required_argument, NULL, 's' },
		{ "help",     no_argument,       NULL, 'h' },
		{ "version",  no_argument,       NULL, 'V' },
		{ NULL, 0, NULL, 0 }
	};

	/* Parse global options (stop at first non-option) */
	while ((opt = getopt_long(argc, argv, "+vjCc:s:hV",
				  long_opts, NULL)) != -1) {
		switch (opt) {
		case 'v': cli.verbose = true;  g_verbose = true;   break;
		case 'j': cli.json_output = true;                  break;
		case 'C': cli.no_color = true; g_no_color = true;  break;
		case 'c': strncpy(cli.ctl_device, optarg,
				  sizeof(cli.ctl_device) - 1);     break;
		case 's': strncpy(cli.socket_path, optarg,
				  sizeof(cli.socket_path) - 1);    break;
		case 'h': print_usage(); return 0;
		case 'V': printf("bdfs %s\n", BDFS_CLI_VERSION); return 0;
		default:  print_usage(); return 1;
		}
	}

	if (optind >= argc) {
		print_usage();
		return 1;
	}

	const char *cmd = argv[optind];
	int sub_argc = argc - optind;
	char **sub_argv = argv + optind;

	/* Two-word commands */
	if (!strcmp(cmd, "partition")) {
		if (sub_argc < 2) {
			bdfs_err("partition requires a subcommand: add|remove|list|show");
			return 1;
		}
		const char *sub = sub_argv[1];
		sub_argc--; sub_argv++;
		if (!strcmp(sub, "add"))    ret = cmd_partition_add(&cli, sub_argc, sub_argv);
		else if (!strcmp(sub, "remove")) ret = cmd_partition_remove(&cli, sub_argc, sub_argv);
		else if (!strcmp(sub, "list"))   ret = cmd_partition_list(&cli, sub_argc, sub_argv);
		else if (!strcmp(sub, "show"))   ret = cmd_partition_show(&cli, sub_argc, sub_argv);
		else { bdfs_err("unknown partition subcommand: %s", sub); ret = 1; }
	} else if (!strcmp(cmd, "blend")) {
		if (sub_argc < 2) {
			bdfs_err("blend requires a subcommand: mount|umount");
			return 1;
		}
		const char *sub = sub_argv[1];
		sub_argc--; sub_argv++;
		if (!strcmp(sub, "mount"))       ret = cmd_blend_mount(&cli, sub_argc, sub_argv);
		else if (!strcmp(sub, "umount")) ret = cmd_blend_umount(&cli, sub_argc, sub_argv);
		else { bdfs_err("unknown blend subcommand: %s", sub); ret = 1; }
	}
	/* Single-word commands */
	else if (!strcmp(cmd, "export"))   ret = cmd_export(&cli, sub_argc, sub_argv);
	else if (!strcmp(cmd, "import"))   ret = cmd_import(&cli, sub_argc, sub_argv);
	else if (!strcmp(cmd, "mount"))    ret = cmd_mount(&cli, sub_argc, sub_argv);
	else if (!strcmp(cmd, "umount"))   ret = cmd_umount(&cli, sub_argc, sub_argv);
	else if (!strcmp(cmd, "snapshot")) ret = cmd_snapshot(&cli, sub_argc, sub_argv);
	else if (!strcmp(cmd, "promote"))  ret = cmd_promote(&cli, sub_argc, sub_argv);
	else if (!strcmp(cmd, "demote"))   ret = cmd_demote(&cli, sub_argc, sub_argv);
	else if (!strcmp(cmd, "status"))   ret = cmd_status(&cli, sub_argc, sub_argv);
	else {
		bdfs_err("unknown command: %s", cmd);
		print_usage();
		ret = 1;
	}

	bdfs_cli_close(&cli);
	return ret ? 1 : 0;
}
