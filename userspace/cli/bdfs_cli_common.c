// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_cli_common.c - Shared CLI lifecycle, daemon transport, and printers.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "bdfs.h"
#include "../../include/btrfs_dwarfs/types.h"

void bdfs_cli_init(struct bdfs_cli *cli)
{
	memset(cli, 0, sizeof(*cli));
	cli->ctl_fd = -1;
	cli->sock_fd = -1;
	strncpy(cli->ctl_device, "/dev/bdfs_ctl", sizeof(cli->ctl_device) - 1);
	strncpy(cli->socket_path, "/run/bdfs/daemon.sock", sizeof(cli->socket_path) - 1);
}

void bdfs_cli_close(struct bdfs_cli *cli)
{
	if (cli->ctl_fd >= 0) {
		close(cli->ctl_fd);
		cli->ctl_fd = -1;
	}
}

void bdfs_cli_cleanup(struct bdfs_cli *cli)
{
	bdfs_cli_close(cli);
	if (cli->sock_fd >= 0) {
		close(cli->sock_fd);
		cli->sock_fd = -1;
	}
}

int bdfs_cli_open_ctl(struct bdfs_cli *cli)
{
	if (cli->ctl_fd >= 0)
		return 0;
	cli->ctl_fd = open(cli->ctl_device, O_RDWR | O_CLOEXEC);
	if (cli->ctl_fd < 0) {
		bdfs_err("open %s: %s", cli->ctl_device, strerror(errno));
		return -errno;
	}
	return 0;
}

int bdfs_cli_send_recv(struct bdfs_cli *cli, const char *request,
		       char *response, size_t response_size)
{
	struct sockaddr_un address;
	size_t sent = 0;
	size_t used = 0;
	int fd;
	int ret = 0;

	if (!cli || !request || !response || response_size < 2)
		return -EINVAL;
	response[0] = '\0';
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, cli->socket_path, sizeof(address.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		ret = -errno;
		goto out;
	}

	while (sent < strlen(request)) {
		ssize_t n = send(fd, request + sent, strlen(request) - sent, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			ret = -errno;
			goto out;
		}
		if (n == 0) {
			ret = -EPIPE;
			goto out;
		}
		sent += (size_t)n;
	}

	for (;;) {
		ssize_t n = recv(fd, response + used, response_size - used - 1, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			ret = -errno;
			goto out;
		}
		if (n == 0)
			break;
		used += (size_t)n;
		response[used] = '\0';
		if (memchr(response, '\n', used))
			break;
		if (used == response_size - 1) {
			ret = -ENOSPC;
			goto out;
		}
	}
	if (used == 0)
		ret = -ECONNRESET;
out:
	close(fd);
	if (ret)
		response[0] = '\0';
	return ret;
}

static void print_json_string(const char *value)
{
	const unsigned char *p = (const unsigned char *)(value ? value : "");
	putchar('"');
	for (; *p; p++) {
		switch (*p) {
		case '"': fputs("\\\"", stdout); break;
		case '\\': fputs("\\\\", stdout); break;
		case '\b': fputs("\\b", stdout); break;
		case '\f': fputs("\\f", stdout); break;
		case '\n': fputs("\\n", stdout); break;
		case '\r': fputs("\\r", stdout); break;
		case '\t': fputs("\\t", stdout); break;
		default:
			if (*p < 0x20)
				printf("\\u%04x", *p);
			else
				putchar(*p);
		}
	}
	putchar('"');
}

static const char *partition_type_name(uint32_t type)
{
	switch (type) {
	case BDFS_PART_DWARFS_BACKED: return "dwarfs-backed";
	case BDFS_PART_BTRFS_BACKED: return "btrfs-backed";
	case BDFS_PART_HYBRID_BLEND: return "hybrid-blend";
	default: return "unknown";
	}
}

void bdfs_print_partition(const struct bdfs_partition *part, bool json)
{
	char uuid[37];
	if (!part)
		return;
	bdfs_uuid_to_str(part->uuid, uuid);
	if (json) {
		printf("{\"uuid\":");
		print_json_string(uuid);
		printf(",\"label\":");
		print_json_string(part->label);
		printf(",\"type\":");
		print_json_string(partition_type_name(part->type));
		printf(",\"devicePath\":");
		print_json_string(part->device_path);
		printf(",\"mountPoint\":");
		print_json_string(part->mount_point);
		printf("}");
		return;
	}
	printf("  Label:       %s\n", part->label);
	printf("  Type:        %s\n", partition_type_name(part->type));
	printf("  Device:      %s\n", part->device_path);
	printf("  Mount point: %s\n", part->mount_point);
}

void bdfs_print_image(const struct bdfs_dwarfs_image *image, bool json)
{
	char uuid[37];
	if (!image)
		return;
	bdfs_uuid_to_str(image->uuid, uuid);
	if (json) {
		printf("{\"id\":%llu,\"uuid\":", (unsigned long long)image->image_id);
		print_json_string(uuid);
		printf(",\"name\":");
		print_json_string(image->name);
		printf(",\"sizeBytes\":%llu,\"uncompressedBytes\":%llu,\"compression\":",
		       (unsigned long long)image->size_bytes,
		       (unsigned long long)image->uncompressed_bytes);
		print_json_string(bdfs_compression_name(image->compression));
		printf(",\"mounted\":%s,\"mountPoint\":",
		       image->mounted ? "true" : "false");
		print_json_string(image->mount_point);
		printf("}");
		return;
	}
	printf("  Image:       %s (id=%llu)\n", image->name,
	       (unsigned long long)image->image_id);
	printf("  Size:        %llu bytes (unpacked %llu bytes)\n",
	       (unsigned long long)image->size_bytes,
	       (unsigned long long)image->uncompressed_bytes);
	printf("  Compression: %s\n", bdfs_compression_name(image->compression));
	printf("  Mounted:     %s\n", image->mounted ? "yes" : "no");
	if (image->mounted)
		printf("  Mount point: %s\n", image->mount_point);
}

void bdfs_print_subvol(const struct bdfs_btrfs_subvol *subvol, bool json)
{
	if (!subvol)
		return;
	if (json) {
		printf("{\"id\":%llu,\"parentId\":%llu,\"name\":",
		       (unsigned long long)subvol->subvol_id,
		       (unsigned long long)subvol->parent_id);
		print_json_string(subvol->name);
		printf(",\"path\":");
		print_json_string(subvol->path);
		printf(",\"snapshot\":%s,\"readonly\":%s}",
		       subvol->is_snapshot ? "true" : "false",
		       subvol->is_readonly ? "true" : "false");
		return;
	}
	printf("  Subvolume:   %s (id=%llu, parent=%llu)\n",
	       subvol->name, (unsigned long long)subvol->subvol_id,
	       (unsigned long long)subvol->parent_id);
	printf("  Path:        %s\n", subvol->path);
	printf("  Snapshot:    %s\n", subvol->is_snapshot ? "yes" : "no");
	printf("  Read-only:   %s\n", subvol->is_readonly ? "yes" : "no");
}

static void cli_vprint(FILE *stream, const char *level, const char *format, va_list args)
{
	fprintf(stream, "bdfs: %s: ", level);
	vfprintf(stream, format, args);
	fputc('\n', stream);
}

void bdfs_err(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	cli_vprint(stderr, "error", format, args);
	va_end(args);
}

void bdfs_info(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	cli_vprint(stdout, "info", format, args);
	va_end(args);
}
