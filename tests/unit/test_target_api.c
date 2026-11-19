/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bdfs/target.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(name, expression) do { \
    if (expression) printf("PASS: %s\n", name); \
    else { fprintf(stderr, "FAIL: %s\n", name); failures++; } \
} while (0)

static struct bdfs_target_context linux_target(void)
{
    struct bdfs_target_context target;
    memset(&target, 0, sizeof(target));
    target.struct_version = BDFS_TARGET_API_VERSION;
    strcpy(target.target_id, "linux-btrfs-dwarfs");
    target.os_family = BDFS_OS_LINUX;
    target.kernel_family = BDFS_KERNEL_LINUX;
    strcpy(target.kernel_abi, "linux-kbuild");
    strcpy(target.architecture, "x86_64");
    strcpy(target.object_format, "ELF");
    strcpy(target.runtime, "glibc");
    target.compatibility_tier = BDFS_TIER_NATIVE;
    return target;
}

static void test_capability_negotiation(void)
{
    struct bdfs_target_context target = linux_target();
    struct bdfs_capability_set caps;
    struct bdfs_capability_request request;
    struct bdfs_negotiated_target result;
    memset(&caps, 0, sizeof(caps));
    memset(&request, 0, sizeof(request));
    caps.struct_version = BDFS_TARGET_API_VERSION;
    request.struct_version = BDFS_TARGET_API_VERSION;
    caps.states[BDFS_CAP_BTRFS] = BDFS_CAP_SUPPORTED;
    caps.states[BDFS_CAP_DWARFS] = BDFS_CAP_SUPPORTED;
    request.required_mask = (UINT64_C(1) << BDFS_CAP_BTRFS) |
                            (UINT64_C(1) << BDFS_CAP_DWARFS);
    strcpy(request.required_kernel_abi, "linux-kbuild");
    CHECK("Linux baseline negotiates declared Btrfs and DwarFS",
          bdfs_target_negotiate(&target, &caps, &request, &result) == 0);
    CHECK("negotiated result preserves target identity",
          strcmp(result.target.target_id, target.target_id) == 0);
    CHECK("negotiated result exposes declared capabilities",
          result.supported_mask == request.required_mask);
    strcpy(request.required_kernel_abi, "incompatible-kernel-abi");
    CHECK("incompatible kernel ABI is rejected",
          bdfs_target_negotiate(&target, &caps, &request, &result) ==
          BDFS_TARGET_E_ABI);
    request.required_kernel_abi[0] = '\0';

    caps.states[BDFS_CAP_DWARFS] = BDFS_CAP_UNKNOWN;
    CHECK("unknown required capability fails closed",
          bdfs_target_negotiate(&target, &caps, &request, &result) ==
          BDFS_TARGET_E_UNKNOWN_CAPABILITY);
    caps.states[BDFS_CAP_DWARFS] = BDFS_CAP_UNSUPPORTED;
    CHECK("unsupported required capability fails closed",
          bdfs_target_negotiate(&target, &caps, &request, &result) ==
          BDFS_TARGET_E_MISSING_CAPABILITY);
    request.required_mask |= UINT64_C(1) << BDFS_CAP_ACL;
    caps.states[BDFS_CAP_DWARFS] = BDFS_CAP_SUPPORTED;
    CHECK("omitted capability remains unknown",
          bdfs_target_negotiate(&target, &caps, &request, &result) ==
          BDFS_TARGET_E_UNKNOWN_CAPABILITY);
}

static void test_kernel_family_boundaries(void)
{
    struct bdfs_target_context target = linux_target();
    struct bdfs_capability_set caps;
    struct bdfs_capability_request request;
    struct bdfs_negotiated_target result;
    memset(&caps, 0, sizeof(caps));
    memset(&request, 0, sizeof(request));
    caps.struct_version = request.struct_version = BDFS_TARGET_API_VERSION;
    target.os_family = BDFS_OS_FREEBSD;
    target.kernel_family = BDFS_KERNEL_LINUX;
    CHECK("FreeBSD cannot be mislabeled as Linux-kernel target",
          bdfs_target_negotiate(&target, &caps, &request, &result) ==
          BDFS_TARGET_E_UNSUPPORTED);
    target.os_family = BDFS_OS_DARWIN;
    target.kernel_family = BDFS_KERNEL_XNU;
    target.compatibility_tier = BDFS_TIER_READ_ONLY_INGESTION;
    CHECK("Darwin/XNU remains a distinct read-only target family",
          bdfs_target_negotiate(&target, &caps, &request, &result) == 0);
    target.kernel_family = BDFS_KERNEL_FREEBSD;
    CHECK("Darwin cannot be paired with FreeBSD kernel family",
          bdfs_target_negotiate(&target, &caps, &request, &result) ==
          BDFS_TARGET_E_UNSUPPORTED);
}

static void test_metadata_policy(void)
{
    struct bdfs_metadata_report report;
    memset(&report, 0, sizeof(report));
    report.struct_version = BDFS_TARGET_API_VERSION;
    report.required_mask = (UINT64_C(1) << BDFS_META_OWNER) |
                           (UINT64_C(1) << BDFS_META_XATTRS);
    report.preserved_mask = UINT64_C(1) << BDFS_META_OWNER;
    report.unsupported_mask = UINT64_C(1) << BDFS_META_XATTRS;
    CHECK("permissive metadata report records unsupported fields",
          bdfs_metadata_report_validate(&report, 0) == 0);
    CHECK("strict metadata policy rejects required metadata loss",
          bdfs_metadata_report_validate(&report, 1) ==
          BDFS_TARGET_E_METADATA_LOSS);
    report.unsupported_mask = 0;
    report.unknown_mask = UINT64_C(1) << BDFS_META_XATTRS;
    CHECK("strict metadata policy rejects unknown fidelity",
          bdfs_metadata_report_validate(&report, 1) ==
          BDFS_TARGET_E_METADATA_LOSS);
    report.unknown_mask = 0;
    report.translated_mask = UINT64_C(1) << BDFS_META_XATTRS;
    CHECK("strict metadata policy accepts explicit translation",
          bdfs_metadata_report_validate(&report, 1) == 0);
    report.translated_mask = 0;
    CHECK("missing disposition is rejected",
          bdfs_metadata_report_validate(&report, 0) ==
          BDFS_TARGET_E_UNKNOWN_CAPABILITY);
    report.preserved_mask = UINT64_C(1) << BDFS_META_OWNER;
    report.translated_mask = UINT64_C(1) << BDFS_META_OWNER;
    CHECK("overlapping metadata dispositions are rejected",
          bdfs_metadata_report_validate(&report, 0) ==
          BDFS_TARGET_E_ARGUMENT);
    report.preserved_mask = 0;
    report.translated_mask = 0;
    report.required_mask = UINT64_C(1) << BDFS_META_FIELD_COUNT;
    CHECK("out-of-range metadata field bits are rejected",
          bdfs_metadata_report_validate(&report, 0) ==
          BDFS_TARGET_E_ARGUMENT);
}

int main(void)
{
    test_capability_negotiation();
    test_kernel_family_boundaries();
    test_metadata_policy();
    return failures ? 1 : 0;
}
