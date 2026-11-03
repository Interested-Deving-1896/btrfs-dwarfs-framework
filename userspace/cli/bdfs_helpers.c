// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * bdfs_helpers.c - Small, reusable CLI value conversion helpers.
 *
 * Keep these helpers independent of the CLI entry point so they can be
 * compiled and unit-tested without pulling command dispatch into a test.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
