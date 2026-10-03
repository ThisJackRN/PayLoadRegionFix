#include "../upstream/libiosuhax/source/devoptab_registry.h"
#include <assert.h>
#include <stdio.h>

const devoptab_t *devoptab_list[STD_MAX];
static int prefixBug = 0;
int AddDevice(const devoptab_t *device) {
    for (int i = 3; i < STD_MAX; ++i) {
        if (!devoptab_list[i] || !strcmp(devoptab_list[i]->name, device->name)) {
            devoptab_list[i] = device;
            return i;
        }
    }
    return -1;
}
int FindDevice(const char *name) {
    for (int i = 3; i < STD_MAX; ++i) {
        if (!devoptab_list[i]) continue;
        if (prefixBug ? !strncmp(name, devoptab_list[i]->name, strlen(devoptab_list[i]->name))
                      : !strcmp(name, devoptab_list[i]->name)) return i;
    }
    return -1;
}
int RemoveDevice(const char *name) {
    const int i = FindDevice(name);
    if (i < 0) return -1;
    devoptab_list[i] = NULL;
    return 0;
}

int main(void) {
    const devoptab_t stdinDev = {"stdin"}, slc = {"storage_slc_installer"}, mlc = {"storage_mlc_installer"};
    const devoptab_t duplicate = {"storage_slc_installer"}, filler = {"occupied"};
    devoptab_list[0] = &stdinDev;
    // Reproduce the exact convention mismatch: NULL slots != stdin pointer.
    int legacyFree = 0;
    for (int i = 3; i < STD_MAX; ++i) legacyFree += devoptab_list[i] == devoptab_list[0];
    assert(legacyFree == 0);
    assert(fs_registry_add(&slc) == 0 && FindDevice(slc.name) == 3);
    assert(fs_registry_add(&mlc) == 0 && FindDevice(mlc.name) == 4);
    assert(fs_registry_add(&duplicate) == -1 && errno == EEXIST);
    assert(devoptab_list[3] == &slc);
    assert(fs_registry_remove(&duplicate) == -1 && errno == ENODEV);
    assert(fs_registry_remove(&slc) == 0 && devoptab_list[3] == NULL);
    assert(devoptab_list[4] == &mlc && devoptab_list[0] == &stdinDev);
    assert(fs_registry_add(&slc) == 0 && FindDevice(slc.name) == 3);
    assert(fs_registry_remove(&slc) == 0 && fs_registry_remove(&mlc) == 0);
    const devoptab_t shortName = {"ntfs"}, longName = {"ntfs1"};
    assert(fs_registry_add(&shortName) == 0 && fs_registry_add(&longName) == 0);
    prefixBug = 1;
    assert(fs_registry_remove(&longName) == -1 && errno == EINVAL);
    assert(devoptab_list[3] == &shortName && devoptab_list[4] == &longName);
    prefixBug = 0;
    assert(fs_registry_remove(&longName) == 0 && devoptab_list[3] == &shortName);
    assert(fs_registry_remove(&shortName) == 0);
    for (int i = 3; i < STD_MAX; ++i) devoptab_list[i] = &filler;
    assert(fs_registry_add(&slc) == -1 && errno == EADDRNOTAVAIL);
    assert(fs_registry_add(NULL) == -1 && errno == EINVAL);
    assert(fs_registry_remove(NULL) == -1 && errno == EINVAL);
    puts("Device registry tests passed: NULL slots, duplicates, cleanup, reuse, prefix guard, full table.");
}
