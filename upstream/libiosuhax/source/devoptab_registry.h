// RegionFix compatibility patch: let the runtime own device-slot bookkeeping.
#pragma once
#include <errno.h>
#include <string.h>
#include <sys/iosupport.h>

static inline int fs_registry_add(const devoptab_t *dev) {
    if (!dev || !dev->name || !dev->name[0]) {
        errno = EINVAL;
        return -1;
    }
    // AddDevice can replace an existing name. Do not take over another mount.
    for (int i = 3; i < STD_MAX; ++i) {
        const devoptab_t *existing = devoptab_list[i];
        if (existing && existing->name && strcmp(existing->name, dev->name) == 0) {
            errno = EEXIST;
            return -1;
        }
    }
    if (AddDevice(dev) < 0) {
        errno = EADDRNOTAVAIL;
        return -1;
    }
    return 0; // preserve mount_fs's existing success convention
}

static inline int fs_registry_remove(const devoptab_t *dev) {
    if (!dev || !dev->name) {
        errno = EINVAL;
        return -1;
    }
    for (int i = 3; i < STD_MAX; ++i) {
        if (devoptab_list[i] != dev) continue;
        // Guard against a runtime with the historical prefix-matching bug.
        if (FindDevice(dev->name) != i) {
            errno = EINVAL;
            return -1;
        }
        return RemoveDevice(dev->name);
    }
    errno = ENODEV;
    return -1;
}
