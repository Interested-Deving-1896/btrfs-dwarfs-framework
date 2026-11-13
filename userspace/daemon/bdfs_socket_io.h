// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BDFS_SOCKET_IO_H
#define BDFS_SOCKET_IO_H

#include <stddef.h>

int bdfs_socket_read_request(int fd, char *buf, size_t capacity);
int bdfs_socket_write_response(int fd, const char *buf, size_t length);

#endif
