#ifndef VERSION_MANAGER_H
#define VERSION_MANAGER_H

#include "common.h"
#include <pthread.h>

typedef struct {
    char latest_version[MAX_VERSION_STR];
    char package_file[MAX_PATH_LEN];
    char checksum_sha256[SHA256_HEX_BUF];
    char min_supported[MAX_VERSION_STR];
    char release_notes[MAX_VALUE_LEN];
    unsigned long long package_size;
} version_manifest_t;

typedef struct {
    pthread_rwlock_t lock;
    version_manifest_t manifest;
    char manifest_path[MAX_PATH_LEN];
    char package_dir[MAX_PATH_LEN];
} version_manager_t;

int vm_init(version_manager_t *vm, const char *manifest_path,
            const char *package_dir);
int vm_reload(version_manager_t *vm);
int vm_get_snapshot(version_manager_t *vm, version_manifest_t *out);
void vm_destroy(version_manager_t *vm);

#endif
