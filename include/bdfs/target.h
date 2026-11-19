/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BDFS_TARGET_H
#define BDFS_TARGET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BDFS_TARGET_API_VERSION 1u
#define BDFS_TARGET_ID_MAX 64u
#define BDFS_TARGET_TEXT_MAX 96u
#define BDFS_TARGET_MAX_CAPABILITIES 64u

enum bdfs_os_family {
    BDFS_OS_UNKNOWN = 0,
    BDFS_OS_LINUX,
    BDFS_OS_FREEBSD,
    BDFS_OS_DARWIN
};

enum bdfs_kernel_family {
    BDFS_KERNEL_UNKNOWN = 0,
    BDFS_KERNEL_LINUX,
    BDFS_KERNEL_FREEBSD,
    BDFS_KERNEL_XNU
};

enum bdfs_compatibility_tier {
    BDFS_TIER_UNSUPPORTED = 0,
    BDFS_TIER_READ_ONLY_INGESTION,
    BDFS_TIER_HOST_ARTIFACT_TOOLING,
    BDFS_TIER_EXPERIMENTAL_PORT,
    BDFS_TIER_NATIVE
};

struct bdfs_target_context {
    uint32_t struct_version;
    char target_id[BDFS_TARGET_ID_MAX];
    enum bdfs_os_family os_family;
    enum bdfs_kernel_family kernel_family;
    char kernel_abi[BDFS_TARGET_TEXT_MAX];
    char architecture[BDFS_TARGET_TEXT_MAX];
    char object_format[BDFS_TARGET_TEXT_MAX];
    char runtime[BDFS_TARGET_TEXT_MAX];
    enum bdfs_compatibility_tier compatibility_tier;
};

enum bdfs_capability_id {
    BDFS_CAP_BTRFS = 0,
    BDFS_CAP_DWARFS,
    BDFS_CAP_COPY_ON_WRITE,
    BDFS_CAP_SNAPSHOTS,
    BDFS_CAP_OVERLAY,
    BDFS_CAP_XATTRS,
    BDFS_CAP_ACL,
    BDFS_CAP_SECURITY_LABELS,
    BDFS_CAP_SPARSE_EXTENTS,
    BDFS_CAP_HARD_LINKS,
    BDFS_CAP_SYMLINKS,
    BDFS_CAP_SPECIAL_OBJECTS,
    BDFS_CAP_CASE_SENSITIVE_PATHS,
    BDFS_CAP_UNICODE_PATHS,
    BDFS_CAP_MOUNT_NAMESPACES,
    BDFS_CAP_COUNT
};

enum bdfs_capability_state {
    BDFS_CAP_UNKNOWN = 0,
    BDFS_CAP_UNSUPPORTED,
    BDFS_CAP_SUPPORTED
};

enum bdfs_target_error {
    BDFS_TARGET_OK = 0,
    BDFS_TARGET_E_VERSION = -1,
    BDFS_TARGET_E_ARGUMENT = -2,
    BDFS_TARGET_E_UNSUPPORTED = -3,
    BDFS_TARGET_E_UNKNOWN_CAPABILITY = -4,
    BDFS_TARGET_E_MISSING_CAPABILITY = -5,
    BDFS_TARGET_E_ABI = -6,
    BDFS_TARGET_E_METADATA_LOSS = -7
};

struct bdfs_capability_set {
    uint32_t struct_version;
    enum bdfs_capability_state states[BDFS_TARGET_MAX_CAPABILITIES];
};

struct bdfs_capability_request {
    uint32_t struct_version;
    uint64_t required_mask;
};

struct bdfs_negotiated_target {
    uint32_t struct_version;
    struct bdfs_target_context target;
    uint64_t supported_mask;
};

int bdfs_target_negotiate(const struct bdfs_target_context *target,
                          const struct bdfs_capability_set *declared,
                          const struct bdfs_capability_request *request,
                          struct bdfs_negotiated_target *result);

enum bdfs_metadata_field {
    BDFS_META_OWNER = 0,
    BDFS_META_MODE,
    BDFS_META_TIMESTAMPS,
    BDFS_META_XATTRS,
    BDFS_META_ACLS,
    BDFS_META_SECURITY_LABELS,
    BDFS_META_FILE_CAPABILITIES,
    BDFS_META_SPARSE_EXTENTS,
    BDFS_META_HARD_LINKS,
    BDFS_META_SYMLINKS,
    BDFS_META_SPECIAL_OBJECTS,
    BDFS_META_CASE_SEMANTICS,
    BDFS_META_PATH_ENCODING,
    BDFS_META_FIELD_COUNT
};

enum bdfs_metadata_disposition {
    BDFS_META_UNKNOWN = 0,
    BDFS_META_PRESERVED,
    BDFS_META_TRANSLATED,
    BDFS_META_UNSUPPORTED,
    BDFS_META_OMITTED_BY_POLICY
};

struct bdfs_metadata_report {
    uint32_t struct_version;
    uint64_t required_mask;
    uint64_t preserved_mask;
    uint64_t translated_mask;
    uint64_t unsupported_mask;
    uint64_t omitted_mask;
    uint64_t unknown_mask;
};

int bdfs_metadata_report_validate(const struct bdfs_metadata_report *report,
                                 int strict);

#ifdef __cplusplus
}
#endif

#endif
