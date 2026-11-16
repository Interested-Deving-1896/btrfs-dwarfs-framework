// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_helpers.c - Pure userspace helpers shared by the BDFS CLI and tests.
 */
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

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
    fputc('\\n', stderr);
    va_end(ap);
}

void bdfs_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fputc('\\n', stdout);
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
