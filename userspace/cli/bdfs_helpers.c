// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_helpers.c - Pure userspace helpers shared by the BDFS CLI and tests.
 */
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "bdfs.h"

int bdfs_cli_open_ctl(struct bdfs_cli *cli)
{
    const char *path;

    if (!cli)
        return -EINVAL;
    if (cli->ctl_fd >= 0)
        return 0;

    path = cli->ctl_device[0] ? cli->ctl_device : "/dev/bdfs_ctl";
    cli->ctl_fd = open(path, O_RDWR | O_CLOEXEC);
    if (cli->ctl_fd < 0) {
        int err = errno;
        bdfs_err("cannot open %s: %s", path, strerror(err));
        return -err;
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

void bdfs_uuid_to_str(const uint8_t uuid[16], char out[37])
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    size_t pos = 0;

    if (!uuid || !out)
        return;

    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[pos++] = '-';
        out[pos++] = hex[uuid[i] >> 4];
        out[pos++] = hex[uuid[i] & 0x0f];
    }
    out[pos] = '\0';
}

int bdfs_str_to_uuid(const char *str, uint8_t uuid[16])
{
    size_t i;
    size_t src = 0;

    if (!str || !uuid || strlen(str) != 36)
        return -1;

    for (i = 0; i < 16; i++) {
        int hi;
        int lo;

        if (i == 4 || i == 6 || i == 8 || i == 10) {
            if (str[src++] != '-')
                return -1;
        }

        if (!isxdigit((unsigned char)str[src]) ||
            !isxdigit((unsigned char)str[src + 1]))
            return -1;

        hi = isdigit((unsigned char)str[src])
                 ? str[src] - '0'
                 : tolower((unsigned char)str[src]) - 'a' + 10;
        lo = isdigit((unsigned char)str[src + 1])
                 ? str[src + 1] - '0'
                 : tolower((unsigned char)str[src + 1]) - 'a' + 10;
        uuid[i] = (uint8_t)((hi << 4) | lo);
        src += 2;
    }

    return src == 36 ? 0 : -1;
}

const char *bdfs_compression_name(uint32_t compression)
{
    switch (compression) {
    case BDFS_COMPRESS_NONE:
        return "none";
    case BDFS_COMPRESS_LZMA:
        return "lzma";
    case BDFS_COMPRESS_ZSTD:
        return "zstd";
    case BDFS_COMPRESS_LZ4:
        return "lz4";
    case BDFS_COMPRESS_BROTLI:
        return "brotli";
    default:
        return "unknown";
    }
}

uint32_t bdfs_compression_from_name(const char *name)
{
    if (!name)
        return BDFS_COMPRESS_ZSTD;
    if (strcmp(name, "none") == 0)
        return BDFS_COMPRESS_NONE;
    if (strcmp(name, "lzma") == 0)
        return BDFS_COMPRESS_LZMA;
    if (strcmp(name, "lz4") == 0)
        return BDFS_COMPRESS_LZ4;
    if (strcmp(name, "brotli") == 0)
        return BDFS_COMPRESS_BROTLI;
    return BDFS_COMPRESS_ZSTD;
}


static void print_json_string(const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    putchar('"');
    for (; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", stdout); break;
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

void bdfs_print_partition(const struct bdfs_partition *p, bool json)
{
    char uuid[37];

    if (!p)
        return;
    bdfs_uuid_to_str(p->uuid, uuid);

    if (json) {
        printf("{\\"uuid\\":");
        print_json_string(uuid);
        printf(",\\"type\\":");
        print_json_string(partition_type_name(p->type));
        printf(",\\"label\\":");
        print_json_string(p->label);
        printf(",\\"device_path\\":");
        print_json_string(p->device_path);
        printf(",\\"mount_point\\":");
        print_json_string(p->mount_point);
        printf(",\\"version_major\\":%u,\\"version_minor\\":%u}",
               p->version_major, p->version_minor);
        return;
    }

    printf("  Type:        %s\\n", partition_type_name(p->type));
    printf("  Label:       %s\\n", p->label);
    printf("  Device:      %s\\n", p->device_path);
    printf("  Mount point: %s\\n", p->mount_point);
    printf("  Version:     %u.%u\\n", p->version_major, p->version_minor);
}

void bdfs_print_image(const struct bdfs_dwarfs_image *img, bool json)
{
    char uuid[37];

    if (!img)
        return;
    bdfs_uuid_to_str(img->uuid, uuid);

    if (json) {
        printf("{\\"image_id\\":%llu,\\"name\\":",
               (unsigned long long)img->image_id);
        print_json_string(img->name);
        printf(",\\"uuid\\":");
        print_json_string(uuid);
        printf(",\\"size_bytes\\":%llu,\\"uncompressed_bytes\\":%llu,"
               "\\"compression\\":",
               (unsigned long long)img->size_bytes,
               (unsigned long long)img->uncompressed_bytes);
        print_json_string(bdfs_compression_name(img->compression));
        printf(",\\"mounted\\":%s,\\"mount_point\\":",
               img->mounted ? "true" : "false");
        print_json_string(img->mount_point);
        putchar('}');
        return;
    }

    printf("  Image ID:       %llu\\n", (unsigned long long)img->image_id);
    printf("  Name:           %s\\n", img->name);
    printf("  UUID:           %s\\n", uuid);
    printf("  Size:           %llu bytes\\n",
           (unsigned long long)img->size_bytes);
    printf("  Uncompressed:   %llu bytes\\n",
           (unsigned long long)img->uncompressed_bytes);
    printf("  Compression:    %s\\n",
           bdfs_compression_name(img->compression));
    printf("  Mounted:        %s\\n", img->mounted ? "yes" : "no");
    if (img->mounted)
        printf("  Mount point:    %s\\n", img->mount_point);
}

void bdfs_print_subvol(const struct bdfs_btrfs_subvol *sv, bool json)
{
    if (!sv)
        return;

    if (json) {
        printf("{\\"subvol_id\\":%llu,\\"name\\":",
               (unsigned long long)sv->subvol_id);
        print_json_string(sv->name);
        printf(",\\"path\\":");
        print_json_string(sv->path);
        printf(",\\"generation\\":%llu,\\"is_snapshot\\":%s,"
               "\\"is_readonly\\":%s}",
               (unsigned long long)sv->generation,
               sv->is_snapshot ? "true" : "false",
               sv->is_readonly ? "true" : "false");
        return;
    }

    printf("  Subvolume ID: %llu\\n", (unsigned long long)sv->subvol_id);
    printf("  Name:         %s\\n", sv->name);
    printf("  Path:         %s\\n", sv->path);
    printf("  Generation:   %llu\\n", (unsigned long long)sv->generation);
    printf("  Snapshot:     %s\\n", sv->is_snapshot ? "yes" : "no");
    printf("  Read-only:    %s\\n", sv->is_readonly ? "yes" : "no");
}

int bdfs_cli_send_recv(struct bdfs_cli *cli, const char *request,
                       char *response, size_t response_size)
{
    struct sockaddr_un addr;
    size_t sent = 0;
    size_t used = 0;
    int fd;

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

    while (sent < strlen(request)) {
        ssize_t n = send(fd, request + sent, strlen(request) - sent,
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

    while (used < response_size - 1) {
        ssize_t n = recv(fd, response + used, response_size - 1 - used, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            int err = errno;
            close(fd);
            return -err;
        }
        if (n == 0)
            break;
        used += (size_t)n;
        response[used] = '\0';
        if (memchr(response, '\n', used))
            break;
    }

    close(fd);
    if (used == 0)
        return -EIO;
    response[used] = '\0';
    return 0;
}
