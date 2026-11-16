// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_common.c - Shared CLI lifecycle, transport, formatting and value helpers.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "bdfs.h"

void bdfs_cli_init(struct bdfs_cli *cli)
{
	if (!cli)
		return;

	memset(cli, 0, sizeof(*cli));
	cli->ctl_fd = -1;
	cli->sock_fd = -1;
	snprintf(cli->ctl_device, sizeof(cli->ctl_device), "%s", "/dev/bdfs_ctl");
	snprintf(cli->socket_path, sizeof(cli->socket_path), "%s",
		 "/run/bdfs/daemon.sock");
}

void bdfs_cli_close(struct bdfs_cli *cli)
{
	if (!cli)
		return;

	if (cli->ctl_fd >= 0) {
		close(cli->ctl_fd);
		cli->ctl_fd = -1;
	}
	if (cli->sock_fd >= 0) {
		close(cli->sock_fd);
		cli->sock_fd = -1;
	}
}

void bdfs_cli_cleanup(struct bdfs_cli *cli)
{
	bdfs_cli_close(cli);
}

int bdfs_cli_open_ctl(struct bdfs_cli *cli)
{
	if (!cli)
		return -EINVAL;
	if (cli->ctl_fd >= 0)
		return 0;
	if (!cli->ctl_device[0])
		snprintf(cli->ctl_device, sizeof(cli->ctl_device), "%s",
			 "/dev/bdfs_ctl");

	cli->ctl_fd = open(cli->ctl_device, O_RDWR | O_CLOEXEC);
	if (cli->ctl_fd < 0) {
		int err = errno;
		bdfs_err("cannot open %s: %s", cli->ctl_device, strerror(err));
		return -err;
	}
	return 0;
}

int bdfs_cli_send_recv(struct bdfs_cli *cli, const char *request,
		       char *response, size_t response_size)
{
	struct sockaddr_un addr;
	size_t sent = 0, used = 0, request_len;
	int fd, saved_errno;

	if (!cli || !request || !response || response_size < 2)
		return -EINVAL;
	response[0] = '\0';

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(cli->socket_path) >= sizeof(addr.sun_path)) {
		close(fd);
		return -ENAMETOOLONG;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", cli->socket_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		saved_errno = errno;
		close(fd);
		return -saved_errno;
	}

	request_len = strlen(request);
	while (sent < request_len) {
		ssize_t n = send(fd, request + sent, request_len - sent,
				 MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			saved_errno = errno;
			close(fd);
			return -saved_errno;
		}
		if (n == 0) {
			close(fd);
			return -EIO;
		}
		sent += (size_t)n;
	}

	while (used + 1 < response_size) {
		ssize_t n = recv(fd, response + used,
				 response_size - used - 1, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			saved_errno = errno;
			close(fd);
			response[used] = '\0';
			return -saved_errno;
		}
		if (n == 0)
			break;
		used += (size_t)n;
		response[used] = '\0';
		if (memchr(response, '\n', used))
			break;
	}
	close(fd);
	response[used] = '\0';
	return used ? 0 : -EIO;
}

void bdfs_uuid_to_str(const uint8_t uuid[16], char out[37])
{
	if (!uuid || !out)
		return;
	snprintf(out, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5],
		 uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
		 uuid[12], uuid[13], uuid[14], uuid[15]);
}

int bdfs_str_to_uuid(const char *str, uint8_t uuid[16])
{
	static const size_t hyphens[] = { 8, 13, 18, 23 };
	size_t i, pos = 0;
	int high = -1;

	if (!str || !uuid || strlen(str) != 36)
		return -EINVAL;
	for (i = 0; i < 36; i++) {
		int value;
		if (i == hyphens[0] || i == hyphens[1] ||
		    i == hyphens[2] || i == hyphens[3]) {
			if (str[i] != '-')
				return -EINVAL;
			continue;
		}
		if (str[i] >= '0' && str[i] <= '9')
			value = str[i] - '0';
		else if (str[i] >= 'a' && str[i] <= 'f')
			value = str[i] - 'a' + 10;
		else if (str[i] >= 'A' && str[i] <= 'F')
			value = str[i] - 'A' + 10;
		else
			return -EINVAL;

		if (high < 0)
			high = value;
		else {
			uuid[pos++] = (uint8_t)((high << 4) | value);
			high = -1;
		}
	}
	return pos == 16 && high < 0 ? 0 : -EINVAL;
}

const char *bdfs_compression_name(uint32_t compression)
{
	switch (compression) {
	case BDFS_COMPRESS_NONE: return "none";
	case BDFS_COMPRESS_LZMA: return "lzma";
	case BDFS_COMPRESS_ZSTD: return "zstd";
	case BDFS_COMPRESS_LZ4: return "lz4";
	case BDFS_COMPRESS_BROTLI: return "brotli";
	default: return "unknown";
	}
}

uint32_t bdfs_compression_from_name(const char *name)
{
	if (!name)
		return BDFS_COMPRESS_ZSTD;
	if (strcasecmp(name, "none") == 0) return BDFS_COMPRESS_NONE;
	if (strcasecmp(name, "lzma") == 0) return BDFS_COMPRESS_LZMA;
	if (strcasecmp(name, "lz4") == 0) return BDFS_COMPRESS_LZ4;
	if (strcasecmp(name, "brotli") == 0) return BDFS_COMPRESS_BROTLI;
	return BDFS_COMPRESS_ZSTD;
}

static void print_json_string(const char *value)
{
	const unsigned char *p = (const unsigned char *)(value ? value : "");
	putchar('"');
	for (; *p; p++) {
		if (*p == '"' || *p == '\\')
			printf("\\%c", *p);
		else if (*p < 0x20)
			printf("\\u%04x", *p);
		else
			putchar(*p);
	}
	putchar('"');
}

void bdfs_print_partition(const struct bdfs_partition *part, bool json)
{
	char uuid[37];
	if (!part)
		return;
	bdfs_uuid_to_str(part->uuid, uuid);
	if (json) {
		printf("{\"uuid\":"); print_json_string(uuid);
		printf(",\"label\":"); print_json_string(part->label);
		printf(",\"device\":"); print_json_string(part->device_path);
		printf(",\"mount_point\":"); print_json_string(part->mount_point);
		printf(",\"type\":%u,\"flags\":%u}", part->type, part->flags);
	} else {
		printf("  Label: %s\n  Device: %s\n  Mount: %s\n  Type: %u\n",
		       part->label, part->device_path, part->mount_point,
		       part->type);
	}
}

void bdfs_print_image(const struct bdfs_dwarfs_image *image, bool json)
{
	if (!image)
		return;
	if (json) {
		printf("{\"id\":%" PRIu64 ",\"name\":", (uint64_t)image->image_id);
		print_json_string(image->name);
		printf(",\"size_bytes\":%" PRIu64 ",\"compression\":",
		       (uint64_t)image->size_bytes);
		print_json_string(bdfs_compression_name(image->compression));
		printf(",\"mounted\":%s}", image->mounted ? "true" : "false");
	} else {
		printf("  Image: %s (id=%" PRIu64 ", size=%" PRIu64
		       ", compression=%s, mounted=%s)\n",
		       image->name, (uint64_t)image->image_id,
		       (uint64_t)image->size_bytes,
		       bdfs_compression_name(image->compression),
		       image->mounted ? "yes" : "no");
	}
}

void bdfs_print_subvol(const struct bdfs_btrfs_subvol *subvol, bool json)
{
	if (!subvol)
		return;
	if (json) {
		printf("{\"id\":%" PRIu64 ",\"name\":",
		       (uint64_t)subvol->subvol_id);
		print_json_string(subvol->name);
		printf(",\"path\":"); print_json_string(subvol->path);
		printf(",\"snapshot\":%s,\"readonly\":%s}",
		       subvol->is_snapshot ? "true" : "false",
		       subvol->is_readonly ? "true" : "false");
	} else {
		printf("  Subvolume: %s (id=%" PRIu64 ", snapshot=%s, readonly=%s)\n",
		       subvol->name, (uint64_t)subvol->subvol_id,
		       subvol->is_snapshot ? "yes" : "no",
		       subvol->is_readonly ? "yes" : "no");
	}
}

static void cli_vprint(FILE *stream, const char *prefix,
		       const char *fmt, va_list ap)
{
	if (prefix)
		fprintf(stream, "%s", prefix);
	vfprintf(stream, fmt, ap);
	fputc('\n', stream);
}

void bdfs_err(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	cli_vprint(stderr, "bdfs: error: ", fmt, ap);
	va_end(ap);
}

void bdfs_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	cli_vprint(stdout, "bdfs: ", fmt, ap);
	va_end(ap);
}
