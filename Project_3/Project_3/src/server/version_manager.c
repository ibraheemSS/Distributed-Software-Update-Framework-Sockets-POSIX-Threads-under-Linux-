#include "version_manager.h"
#include "config.h"
#include "logger.h"
#include "package_service.h"
#include "sha256.h"
#include "version.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int hex64_ok(const char *s)
{
    unsigned char raw[32];
    return s && strlen(s) == SHA256_HEX_LEN && hex_to_bytes(s, raw, sizeof(raw)) == 0;
}

int vm_init(version_manager_t *vm, const char *manifest_path,
            const char *package_dir)
{
    if (!vm || !manifest_path || !package_dir) {
        return -1;
    }
    memset(vm, 0, sizeof(*vm));
    pthread_rwlock_init(&vm->lock, NULL);
    snprintf(vm->manifest_path, sizeof(vm->manifest_path), "%s", manifest_path);
    snprintf(vm->package_dir, sizeof(vm->package_dir), "%s", package_dir);
    return vm_reload(vm);
}

int vm_reload(version_manager_t *vm)
{
    if (!vm) {
        return -1;
    }

    cfg_t cfg;
    if (cfg_load(&cfg, vm->manifest_path) != 0) {
        log_msg(LOG_ERROR, "main", "-", "MANIFEST_RELOAD failed path=%s", vm->manifest_path);
        return -1;
    }

    version_manifest_t next;
    memset(&next, 0, sizeof(next));
    cfg_copy_str(&cfg, "LATEST_VERSION", "", next.latest_version, sizeof(next.latest_version));
    cfg_copy_str(&cfg, "PACKAGE_FILE", "", next.package_file, sizeof(next.package_file));
    cfg_copy_str(&cfg, "MIN_SUPPORTED", "0.0.0", next.min_supported, sizeof(next.min_supported));
    cfg_copy_str(&cfg, "RELEASE_NOTES", "", next.release_notes, sizeof(next.release_notes));

    if (!ver_is_valid(next.latest_version) || !ver_is_valid(next.min_supported) ||
        next.package_file[0] == '\0') {
        log_msg(LOG_ERROR, "main", "-", "MANIFEST_RELOAD invalid version/package fields");
        return -1;
    }

    char package_path[MAX_PATH_LEN * 2];
    if (pkg_resolve_path(vm->package_dir, next.package_file,
                         package_path, sizeof(package_path)) != 0 ||
        pkg_file_size(package_path, &next.package_size) != 0) {
        log_msg(LOG_ERROR, "main", "-", "MANIFEST_RELOAD package missing file=%s",
                next.package_file);
        return -1;
    }

    char computed[SHA256_HEX_BUF];
    if (sha256_file_hex(package_path, computed) != 0) {
        log_msg(LOG_ERROR, "main", "-", "MANIFEST_RELOAD checksum failed file=%s",
                next.package_file);
        return -1;
    }

    const char *configured = cfg_get(&cfg, "CHECKSUM_SHA256", "");
    if (configured[0] != '\0') {
        if (!hex64_ok(configured) || strcasecmp(configured, computed) != 0) {
            log_msg(LOG_ERROR, "main", "-",
                    "MANIFEST_RELOAD checksum mismatch file=%s configured=%s computed=%s",
                    next.package_file, configured, computed);
            return -1;
        }
    }
    snprintf(next.checksum_sha256, sizeof(next.checksum_sha256), "%s", computed);

    pthread_rwlock_wrlock(&vm->lock);
    vm->manifest = next;
    pthread_rwlock_unlock(&vm->lock);

    log_msg(LOG_INFO, "main", "-", "MANIFEST_RELOAD ok latest=%s package=%s size=%llu",
            next.latest_version, next.package_file, next.package_size);
    return 0;
}

int vm_get_snapshot(version_manager_t *vm, version_manifest_t *out)
{
    if (!vm || !out) {
        return -1;
    }
    pthread_rwlock_rdlock(&vm->lock);
    *out = vm->manifest;
    pthread_rwlock_unlock(&vm->lock);
    return 0;
}

void vm_destroy(version_manager_t *vm)
{
    if (!vm) {
        return;
    }
    pthread_rwlock_destroy(&vm->lock);
}
