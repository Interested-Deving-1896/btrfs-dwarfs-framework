// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_common.c - Shared CLI transport, formatting, and utility functions.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "bdfs.h"

int bdfs_cli_open_ctl(struct bdfs_cli *cli)
{
	if (!cli)
		return -EINVAL;
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

int bdfs_cli_send_recv(struct bdfs_cli *cli, const char *request,
		       char *response, size_t response_size)
{
	struct sockaddr_un addr;
	int fd;
	size_t sent = 0;
	size_t request_size;
	size_t used = 0;

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
	strncpy(addr.sun_path, cli->socket_path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int err = errno;
		close(fd);
		return -err;
	}

	request_size = strlen(request);
	while (sent < request_size) {
		ssize_t n = send(fd, request + sent, request_size - sent,
				 MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			int err = errno;
			close(fd);
			return -err;
		}
		if (n == 0) {
			close(fd);
			return -EIO;
		}
		sent += (size_t)n;
	}

	if (request_size == 0 || request[request_size - 1] != '\n') {
		const char newline = '\n';
		if (send(fd, &newline, 1, MSG_NOSIGNAL) < 0 && errno != EINTR) {
			int err = errno;
			close(fd);
			return -err;
		}
	}

	while (used < response_size - 1) {
		ssize_t n = recv(fd, response + used,
				 response_size - 1 - used, 0);
		if (n > 0) {
			used += (size_t)n;
			response[used] = '\0';
			if (memchr(response, '\n', used))
				break;
			continue;
		}
		if (n == 0)
			break;
		if (errno == EINTR)
			continue;
		int err = errno;
		close(fd);
		return -err;
	}

	close(fd);
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
	static const int hyphens[] = {8, 13, 18, 23};
	int byte = 0;

	if (!str || !uuid || strlen(str) != 36)
		return -EINVAL;
	for (int i = 0; i < 36;) {
		if (i == hyphens[0] || i == hyphens[1] ||
		    i == hyphens[2] || i == hyphens[3]) {
			if (str[i++] != '-')
				return -EINVAL;
			continue;
		}
		char pair[3] = { str[i], str[i + 1], '\0' };
		char *end = NULL;
		long value = strtol(pair, &end, 16);
		if (!end || *end != '\0' || value < 0 || value > 255)
			return -EINVAL;
		uuid[byte++] = (uint8_t)value;
		i += 2;
	}
	return byte == 16 ? 0 : -EINVAL;
}

const char *bdfs_compression_name(uint32_t compression)
{
	switch (compression) {
	case BDFS_COMPRESS_LZMA:   return "lzma";
	case BDFS_COMPRESS_ZSTD:   return "zstd";
	case BDFS_COMPRESS_LZ4:    return "lz4";
	case BDFS_COMPRESS_BROTLI: return "brotli";
	case BDFS_COMPRESS_NONE:   return "none";
	default:                   return "unknown";
	}
}

uint32_t bdfs_compression_from_name(const char *name)
{
	if (!name)
		return BDFS_COMPRESS_ZSTD;
	if (strcmp(name, "lzma") == 0) return BDFS_COMPRESS_LZMA;
	if (strcmp(name, "zstd") == 0) return BDFS_COMPRESS_ZSTD;
	if (strcmp(name, "lz4") == 0) return BDFS_COMPRESS_LZ4;
	if (strcmp(name, "brotli") == 0) return BDFS_COMPRESS_BROTLI;
	return BDFS_COMPRESS_ZSTD;
}

void bdfs_err(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("bdfs: error: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void bdfs_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
}

void bdfs_print_partition(const struct bdfs_partition *part, bool json)
{
	char uuid[37];
	if (!part)
		return;
	bdfs_uuid_to_str(part->uuid, uuid);
	if (json) {
		printf("{\"uuid\":\"%s\",\"label\":\"%s\",\"device\":\"%s\",\"mount_point\":\"%s\",\"type\":%u}\n",
		       uuid, part->label, part->device_path, part->mount_point,
		       part->type);
	} else {
		printf("Partition %s  %s\n  device: %s\n  mount:  %s\n  type:   %u\n",
		       uuid, part->label, part->device_path, part->mount_point,
		       part->type);
	}
}

void bdfs_print_image(const struct bdfs_dwarfs_image *image, bool json)
{
	char uuid[37];
	if (!image)
		return;
	bdfs_uuid_to_str(image->uuid, uuid);
	if (json) {
		printf("{\"id\":%llu,\"uuid\":\"%s\",\"name\":\"%s\",\"size_bytes\":%llu,\"mounted\":%s}\n",
		       (unsigned long long)image->image_id, uuid, image->name,
		       (unsigned long long)image->size_bytes,
		       image->mounted ? "true" : "false");
	} else {
		printf("Image %llu  %s\n  uuid: %s\n  size: %llu bytes\n  path: %s\n",
		       (unsigned long long)image->image_id, image->name, uuid,
		       (unsigned long long)image->size_bytes, image->backing_path);
	}
}

void bdfs_print_subvol(const struct bdfs_btrfs_subvol *subvol, bool json)
{
	char uuid[37];
	if (!subvol)
		return;
	bdfs_uuid_to_str(subvol->uuid, uuid);
	if (json) {
		printf("{\"id\":%llu,\"uuid\":\"%s\",\"name\":\"%s\",\"path\":\"%s\",\"readonly\":%s}\n",
		       (unsigned long long)subvol->subvol_id, uuid, subvol->name,
		       subvol->path, subvol->is_readonly ? "true" : "false");
	} else {
		printf("Subvolume %llu  %s\n  uuid: %s\n  path: %s\n  readonly: %s\n",
		       (unsigned long long)subvol->subvol_id, subvol->name, uuid,
		       subvol->path, subvol->is_readonly ? "yes" : "no");
	}
}
