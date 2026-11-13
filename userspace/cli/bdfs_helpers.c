// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_helpers.c - Pure CLI conversion helpers shared by commands and tests.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "bdfs.h"
#include "../../include/btrfs_dwarfs/types.h"

void bdfs_uuid_to_str(const uint8_t uuid[16], char out[37])
{
	if (!uuid || !out)
		return;

	snprintf(out, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 uuid[0], uuid[1], uuid[2], uuid[3],
		 uuid[4], uuid[5], uuid[6], uuid[7],
		 uuid[8], uuid[9], uuid[10], uuid[11],
		 uuid[12], uuid[13], uuid[14], uuid[15]);
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
	static const size_t dash_positions[] = { 8, 13, 18, 23 };
	size_t input = 0;
	size_t output = 0;

	if (!str || !uuid || strlen(str) != 36)
		return -EINVAL;

	for (size_t i = 0; i < sizeof(dash_positions) / sizeof(dash_positions[0]); i++) {
		if (str[dash_positions[i]] != '-')
			return -EINVAL;
	}

	while (input < 36) {
		int high;
		int low;

		if (str[input] == '-') {
			input++;
			continue;
		}
		if (input + 1 >= 36 || output >= 16)
			return -EINVAL;
		high = hex_value(str[input]);
		low = hex_value(str[input + 1]);
		if (high < 0 || low < 0)
			return -EINVAL;
		uuid[output++] = (uint8_t)((high << 4) | low);
		input += 2;
	}

	return output == 16 ? 0 : -EINVAL;
}

const char *bdfs_compression_name(uint32_t compression)
{
	switch (compression) {
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
	if (!name)
		return BDFS_COMPRESS_ZSTD;
	if (strcmp(name, "lzma") == 0)
		return BDFS_COMPRESS_LZMA;
	if (strcmp(name, "lz4") == 0)
		return BDFS_COMPRESS_LZ4;
	if (strcmp(name, "brotli") == 0)
		return BDFS_COMPRESS_BROTLI;
	if (strcmp(name, "none") == 0)
		return BDFS_COMPRESS_NONE;
	return BDFS_COMPRESS_ZSTD;
}
