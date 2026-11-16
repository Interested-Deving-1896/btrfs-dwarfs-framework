// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include "../../userspace/cli/bdfs.h"

static int failures;

static void check(const char *name, int condition)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", name);
		failures++;
	} else {
		printf("PASS: %s\n", name);
	}
}

int main(void)
{
	uint32_t value = 99;

	check("zero accepted when minimum is zero",
	      bdfs_parse_u32("0", 0, UINT32_MAX, &value) == 0 && value == 0);
	check("maximum accepted",
	      bdfs_parse_u32("4294967295", 0, UINT32_MAX, &value) == 0 &&
	      value == UINT32_MAX);
	check("minimum enforced",
	      bdfs_parse_u32("0", 1, UINT32_MAX, &value) < 0);
	check("maximum enforced",
	      bdfs_parse_u32("11", 1, 10, &value) < 0);
	check("empty rejected", bdfs_parse_u32("", 0, UINT32_MAX, &value) < 0);
	check("whitespace rejected",
	      bdfs_parse_u32(" 1", 0, UINT32_MAX, &value) < 0);
	check("leading plus rejected",
	      bdfs_parse_u32("+1", 0, UINT32_MAX, &value) < 0);
	check("negative rejected",
	      bdfs_parse_u32("-1", 0, UINT32_MAX, &value) < 0);
	check("trailing junk rejected",
	      bdfs_parse_u32("12x", 0, UINT32_MAX, &value) < 0);
	check("overflow rejected",
	      bdfs_parse_u32("4294967296", 0, UINT32_MAX, &value) < 0);
	check("null output rejected",
	      bdfs_parse_u32("1", 0, UINT32_MAX, NULL) < 0);

	uint64_t wide = 0;
	check("u64 maximum accepted",
	      bdfs_parse_u64("18446744073709551615", 0, UINT64_MAX,
			     &wide) == 0 && wide == UINT64_MAX);
	check("u64 overflow rejected",
	      bdfs_parse_u64("18446744073709551616", 0, UINT64_MAX,
			     &wide) < 0);
	check("u64 trailing junk rejected",
	      bdfs_parse_u64("42x", 0, UINT64_MAX, &wide) < 0);
	check("u64 negative rejected",
	      bdfs_parse_u64("-42", 0, UINT64_MAX, &wide) < 0);

	printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
