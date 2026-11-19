/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bdfs/target.h"

#include <string.h>

static int valid_target(const struct bdfs_target_context *target)
{
    if (!target || target->struct_version != BDFS_TARGET_API_VERSION)
        return 0;
    if (!memchr(target->target_id, '\0', sizeof(target->target_id)) ||
        target->target_id[0] == '\0')
        return 0;
    if (!memchr(target->kernel_abi, '\0', sizeof(target->kernel_abi)) ||
        !memchr(target->architecture, '\0', sizeof(target->architecture)) ||
        !memchr(target->object_format, '\0', sizeof(target->object_format)) ||
        !memchr(target->runtime, '\0', sizeof(target->runtime)))
        return 0;
    if (target->os_family < BDFS_OS_LINUX ||
        target->os_family > BDFS_OS_DARWIN ||
        target->kernel_family < BDFS_KERNEL_LINUX ||
        target->kernel_family > BDFS_KERNEL_XNU ||
        target->compatibility_tier < BDFS_TIER_UNSUPPORTED ||
        target->compatibility_tier > BDFS_TIER_NATIVE ||
        target->architecture[0] == '\0' ||
        target->kernel_abi[0] == '\0' ||
        target->object_format[0] == '\0' ||
        target->runtime[0] == '\0')
        return 0;
    if (target->os_family == BDFS_OS_LINUX &&
        target->kernel_family != BDFS_KERNEL_LINUX)
        return 0;
    if (target->os_family == BDFS_OS_FREEBSD &&
        target->kernel_family != BDFS_KERNEL_FREEBSD)
        return 0;
    if (target->os_family == BDFS_OS_DARWIN &&
        target->kernel_family != BDFS_KERNEL_XNU)
        return 0;
    return 1;
}

int bdfs_target_negotiate(const struct bdfs_target_context *target,
                          const struct bdfs_capability_set *declared,
                          const struct bdfs_capability_request *request,
                          struct bdfs_negotiated_target *result)
{
    uint64_t supported = 0;
    unsigned int i;

    if (!target || !declared || !request || !result)
        return BDFS_TARGET_E_ARGUMENT;
    if (target->struct_version != BDFS_TARGET_API_VERSION ||
        declared->struct_version != BDFS_TARGET_API_VERSION ||
        request->struct_version != BDFS_TARGET_API_VERSION)
        return BDFS_TARGET_E_VERSION;
    if (!valid_target(target))
        return BDFS_TARGET_E_UNSUPPORTED;
    if (!memchr(request->required_kernel_abi, '\0',
                sizeof(request->required_kernel_abi)))
        return BDFS_TARGET_E_ARGUMENT;
    if (request->required_kernel_abi[0] != '\0' &&
        strcmp(request->required_kernel_abi, target->kernel_abi) != 0)
        return BDFS_TARGET_E_ABI;
    if (BDFS_CAP_COUNT > BDFS_TARGET_MAX_CAPABILITIES)
        return BDFS_TARGET_E_VERSION;
    if ((request->required_mask >> BDFS_CAP_COUNT) != 0)
        return BDFS_TARGET_E_ARGUMENT;

    for (i = 0; i < BDFS_CAP_COUNT; ++i) {
        const uint64_t bit = UINT64_C(1) << i;
        const enum bdfs_capability_state state = declared->states[i];

        if (state < BDFS_CAP_UNKNOWN || state > BDFS_CAP_SUPPORTED)
            return BDFS_TARGET_E_ARGUMENT;
        if (state == BDFS_CAP_SUPPORTED)
            supported |= bit;
        if ((request->required_mask & bit) == 0)
            continue;
        if (state == BDFS_CAP_UNKNOWN)
            return BDFS_TARGET_E_UNKNOWN_CAPABILITY;
        if (state == BDFS_CAP_UNSUPPORTED)
            return BDFS_TARGET_E_MISSING_CAPABILITY;
    }

    memset(result, 0, sizeof(*result));
    result->struct_version = BDFS_TARGET_API_VERSION;
    result->target = *target;
    result->supported_mask = supported;
    return BDFS_TARGET_OK;
}

int bdfs_metadata_report_validate(const struct bdfs_metadata_report *report,
                                  int strict)
{
    uint64_t accounted;

    if (!report)
        return BDFS_TARGET_E_ARGUMENT;
    if (report->struct_version != BDFS_TARGET_API_VERSION)
        return BDFS_TARGET_E_VERSION;

    {
        const uint64_t valid = (UINT64_C(1) << BDFS_META_FIELD_COUNT) - 1;
        const uint64_t masks[] = {
            report->preserved_mask, report->translated_mask,
            report->unsupported_mask, report->omitted_mask,
            report->unknown_mask
        };
        uint64_t seen = 0;
        size_t i;

        if ((report->required_mask & ~valid) != 0)
            return BDFS_TARGET_E_ARGUMENT;
        for (i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i) {
            if ((masks[i] & ~valid) != 0 || (seen & masks[i]) != 0)
                return BDFS_TARGET_E_ARGUMENT;
            seen |= masks[i];
        }
    }

    accounted = report->preserved_mask | report->translated_mask |
                report->unsupported_mask | report->omitted_mask |
                report->unknown_mask;
    if ((report->required_mask & ~accounted) != 0)
        return BDFS_TARGET_E_UNKNOWN_CAPABILITY;

    if (strict && (report->required_mask &
                   (report->unsupported_mask | report->omitted_mask |
                    report->unknown_mask)) != 0)
        return BDFS_TARGET_E_METADATA_LOSS;
    return BDFS_TARGET_OK;
}


const char *bdfs_target_error_string(int error)
{
    switch (error) {
    case BDFS_TARGET_OK: return "success";
    case BDFS_TARGET_E_VERSION: return "unsupported API structure version";
    case BDFS_TARGET_E_ARGUMENT: return "invalid argument or malformed capability report";
    case BDFS_TARGET_E_UNSUPPORTED: return "unsupported or inconsistent target identity";
    case BDFS_TARGET_E_UNKNOWN_CAPABILITY: return "required capability is unknown or unaccounted";
    case BDFS_TARGET_E_MISSING_CAPABILITY: return "required capability is unsupported";
    case BDFS_TARGET_E_ABI: return "kernel ABI does not match the requested ABI";
    case BDFS_TARGET_E_METADATA_LOSS: return "strict metadata policy rejected fidelity loss";
    default: return "unrecognized target API error";
    }
}
