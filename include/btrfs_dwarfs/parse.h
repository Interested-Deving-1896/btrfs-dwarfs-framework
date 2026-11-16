/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BDFS_DWARFS_PARSE_H
#define BDFS_DWARFS_PARSE_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Parse an unsigned 32-bit decimal integer without accepting signs,
 * whitespace, trailing data, or overflow. Bounds are inclusive.
 */
static inline int bdfs_parse_u32(const char *text, uint32_t min_value,
				 uint32_t max_value, uint32_t *out)
{
	unsigned long long value;
	char *end = NULL;

	if (!text || !text[0] || !out || min_value > max_value)
		return -EINVAL;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (*p < '0' || *p > '9')
			return -EINVAL;
	}
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno == ERANGE || !end || *end != '\0' ||
	    value > UINT32_MAX || value < min_value || value > max_value)
		return -ERANGE;
	*out = (uint32_t)value;
	return 0;
}


/* Strict decimal parser for unsigned 64-bit identifiers and sizes. */
static inline int bdfs_parse_u64(const char *text, uint64_t min_value,
				 uint64_t max_value, uint64_t *out)
{
	unsigned long long value;
	char *end = NULL;

	if (!text || !text[0] || !out || min_value > max_value)
		return -EINVAL;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (*p < '0' || *p > '9')
			return -EINVAL;
	}
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno == ERANGE || !end || *end != '\0' ||
	    value < min_value || value > max_value)
		return -ERANGE;
	*out = (uint64_t)value;
	return 0;
}

#endif /* BDFS_DWARFS_PARSE_H */
