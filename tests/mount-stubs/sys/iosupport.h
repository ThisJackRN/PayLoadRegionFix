#pragma once
// Minimal host stand-in for testing the actual registry helper.
enum { STD_MAX = 35 };
typedef struct { const char *name; } devoptab_t;
extern const devoptab_t *devoptab_list[STD_MAX];
int AddDevice(const devoptab_t *device);
int FindDevice(const char *name);
int RemoveDevice(const char *name);
