// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_helpers.c - Small, reusable CLI value conversion helpers.
 *
 * Keep these helpers independent of the CLI entry point so they can be
 * compiled and unit-tested without pulling command dispatch into a test.
 */
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "bdfs.h"

void bdfs_uuid_to_str(const uint8_t uuid[16], char out[37])
{
	static const char hex[] = "0123456789abcdef";
	size_t out_pos = 0;

	for (size_t i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			out[out_pos++] = '-';
		out[out_pos++] = hex[uuid[i] >> 4];
		out[out_pos++] = hex[uuid[i] & 0x0f];
	}
	out[out_pos] = '\0';
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int bdfs_str_to_uuid(const char *str, uint8_t uuid[16])
{
	static const size_t hyphen_positions[] = { 8, 13, 18, 23 };
	uint8_t parsed[16];
	size_t input_pos = 0;

	if (!str || !uuid || strlen(str) != 36)
		return -1;

	for (size_t i = 0; i < sizeof(hyphen_positions) / sizeof(hyphen_positions[0]); i++) {
		if (str[hyphen_positions[i]] != '-')
			return -1;
	}

	for (size_t i = 0; i < 16; i++) {
		while (input_pos == 8 || input_pos == 13 ||
		       input_pos == 18 || input_pos == 23)
			input_pos++;

		int high = hex_value(str[input_pos++]);
		int low = hex_value(str[input_pos++]);
		if (high < 0 || low < 0)
			return -1;
		parsed[i] = (uint8_t)((high << 4) | low);
	}

	memcpy(uuid, parsed, sizeof(parsed));
	return 0;
}

const char *bdfs_compression_name(uint32_t compression)
{
	switch (compression) {
	case BDFS_COMPRESS_NONE:   return "none";
	case BDFS_COMPRESS_LZMA:   return "lzma";
	case BDFS_COMPRESS_ZSTD:   return "zstd";
	case BDFS_COMPRESS_LZ4:    return "lz4";
	case BDFS_COMPRESS_BROTLI: return "brotli";
	default:                  return "zstd";
	}
}

uint32_t bdfs_compression_from_name(const char *name)
{
	if (!name)
		return BDFS_COMPRESS_ZSTD;
	if (strcmp(name, "none") == 0)   return BDFS_COMPRESS_NONE;
	if (strcmp(name, "lzma") == 0)   return BDFS_COMPRESS_LZMA;
	if (strcmp(name, "zstd") == 0)   return BDFS_COMPRESS_ZSTD;
	if (strcmp(name, "lz4") == 0)    return BDFS_COMPRESS_LZ4;
	if (strcmp(name, "brotli") == 0) return BDFS_COMPRESS_BROTLI;
	return BDFS_COMPRESS_ZSTD;
}

/* CLI context lifecycle and common output/device helpers. */
void bdfs_cli_init(struct bdfs_cli *cli)
{
	if (!cli)
		return;
	memset(cli, 0, sizeof(*cli));
	cli->ctl_fd = -1;
	cli->sock_fd = -1;
	(void)snprintf(cli->ctl_device, sizeof(cli->ctl_device), "%s", "/dev/bdfs_ctl");
	(void)snprintf(cli->socket_path, sizeof(cli->socket_path), "%s", "/run/bdfs/daemon.sock");
	(void)snprintf(cli->dwarfsextract_bin, sizeof(cli->dwarfsextract_bin), "%s", "dwarfsextract");
}

void bdfs_cli_close(struct bdfs_cli *cli)
{
	if (!cli)
		return;
	if (cli->ctl_fd >= 0) {
		(void)close(cli->ctl_fd);
		cli->ctl_fd = -1;
	}
	if (cli->sock_fd >= 0) {
		(void)close(cli->sock_fd);
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
	cli->ctl_fd = open(cli->ctl_device, O_RDWR | O_CLOEXEC);
	if (cli->ctl_fd < 0)
		return -errno;
	return 0;
}

static void bdfs_vmessage(FILE *stream, const char *prefix,
			  const char *fmt, va_list ap)
{
	if (prefix)
		(void)fprintf(stream, "%s", prefix);
	(void)vfprintf(stream, fmt, ap);
	(void)fputc('\n', stream);
}

void bdfs_err(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	bdfs_vmessage(stderr, "bdfs: error: ", fmt, ap);
	va_end(ap);
}

void bdfs_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	bdfs_vmessage(stdout, NULL, fmt, ap);
	va_end(ap);
}

/* Send one newline-framed request to the daemon and read one response. */
int bdfs_cli_send_recv(struct bdfs_cli *cli, const char *request,
                       char *response, size_t response_size)
{
	struct sockaddr_un addr;
	size_t sent = 0, used = 0;
	ssize_t n;

	if (!cli || !request || !response || response_size < 2)
		return -EINVAL;
	response[0] = '\0';
	if (cli->sock_fd < 0) {
		cli->sock_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (cli->sock_fd < 0)
			return -errno;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		if (strlen(cli->socket_path) >= sizeof(addr.sun_path)) {
			bdfs_cli_close(cli);
			return -ENAMETOOLONG;
		}
		memcpy(addr.sun_path, cli->socket_path, strlen(cli->socket_path) + 1);
		if (connect(cli->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			int err = errno;
			bdfs_cli_close(cli);
			return -err;
		}
	}
	while (sent < strlen(request)) {
		n = send(cli->sock_fd, request + sent, strlen(request) - sent, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -errno;
		}
		if (n == 0) return -EPIPE;
		sent += (size_t)n;
	}
	while (used + 1 < response_size) {
		n = recv(cli->sock_fd, response + used, response_size - used - 1, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -errno;
		}
		if (n == 0) break;
		used += (size_t)n;
		response[used] = '\0';
		if (memchr(response, '\n', used)) break;
	}
	response[used] = '\0';
	return used ? 0 : -EPIPE;
}

#include <inttypes.h>

static void bdfs_print_json_string(const char *value)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\" ", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (*p < 0x20) printf("\\u%04x", (unsigned int)*p);
            else putchar(*p);
        }
    }
    putchar('"');
}

static const char *bdfs_partition_type_name(uint32_t type)
{
    switch (type) {
    case BDFS_PART_DWARFS_BACKED: return "dwarfs-backed";
    case BDFS_PART_BTRFS_BACKED: return "btrfs-backed";
    case BDFS_PART_HYBRID_BLEND: return "hybrid-blend";
    default: return "unknown";
    }
}

void bdfs_print_partition(const struct bdfs_partition *p, bool json)
{
    char uuid[37];
    if (!p) return;
    bdfs_uuid_to_str(p->uuid, uuid);
    if (json) {
        fputs("{\\"uuid\\":", stdout); bdfs_print_json_string(uuid);
        fputs(",\\"type\\":", stdout); bdfs_print_json_string(bdfs_partition_type_name(p->type));
        fputs(",\\"label\\":", stdout); bdfs_print_json_string(p->label);
        fputs(",\\"device\\":", stdout); bdfs_print_json_string(p->device_path);
        fputs(",\\"mount_point\\":", stdout); bdfs_print_json_string(p->mount_point);
        printf(",\\"dwarfs_image_count\\":%" PRIu64 ",\\"btrfs_subvol_count\\":%" PRIu64
               ",\\"btrfs_snapshot_count\\":%" PRIu64 "}",
               (uint64_t)p->dwarfs_image_count, (uint64_t)p->btrfs_subvol_count,
               (uint64_t)p->btrfs_snapshot_count);
    } else {
        printf("  Type:        %s\\n  Label:       %s\\n  Device:      %s\\n"
               "  Mount point: %s\\n  DwarFS images: %" PRIu64
               "\\n  BTRFS subvolumes: %" PRIu64 " (snapshots: %" PRIu64 ")\\n",
               bdfs_partition_type_name(p->type), p->label, p->device_path,
               p->mount_point, (uint64_t)p->dwarfs_image_count,
               (uint64_t)p->btrfs_subvol_count, (uint64_t)p->btrfs_snapshot_count);
    }
}

void bdfs_print_image(const struct bdfs_dwarfs_image *img, bool json)
{
    if (!img) return;
    if (json) {
        fputs("{\\"id\\":", stdout); printf("%" PRIu64, (uint64_t)img->image_id);
        fputs(",\\"name\\":", stdout); bdfs_print_json_string(img->name);
        printf(",\\"size_bytes\\":%" PRIu64 ",\\"uncompressed_bytes\\":%" PRIu64
               ",\\"compression\\":", (uint64_t)img->size_bytes,
               (uint64_t)img->uncompressed_bytes);
        bdfs_print_json_string(bdfs_compression_name(img->compression));
        printf(",\\"mounted\\":%s,\\"mount_point\\":", img->mounted ? "true" : "false");
        bdfs_print_json_string(img->mount_point);
        putchar('}');
    } else {
        printf("    Size: %" PRIu64 " bytes (uncompressed %" PRIu64 ")\\n"
               "    Compression: %s\\n    Mounted: %s%s%s\\n",
               (uint64_t)img->size_bytes, (uint64_t)img->uncompressed_bytes,
               bdfs_compression_name(img->compression), img->mounted ? "yes" : "no",
               img->mounted ? " at " : "", img->mounted ? img->mount_point : "");
    }
}

void bdfs_print_subvol(const struct bdfs_btrfs_subvol *sv, bool json)
{
    if (!sv) return;
    if (json) {
        printf("{\\"id\\":%" PRIu64 ",\\"parent_id\\":%" PRIu64 ",\\"name\\":",
               (uint64_t)sv->subvol_id, (uint64_t)sv->parent_id);
        bdfs_print_json_string(sv->name);
        fputs(",\\"path\\":", stdout); bdfs_print_json_string(sv->path);
        printf(",\\"snapshot\\":%s,\\"readonly\\":%s}",
               sv->is_snapshot ? "true" : "false",
               sv->is_readonly ? "true" : "false");
    } else {
        printf("    Parent ID: %" PRIu64 "\\n    Path: %s\\n    Snapshot: %s\\n"
               "    Read-only: %s\\n", (uint64_t)sv->parent_id, sv->path,
               sv->is_snapshot ? "yes" : "no", sv->is_readonly ? "yes" : "no");
    }
}
