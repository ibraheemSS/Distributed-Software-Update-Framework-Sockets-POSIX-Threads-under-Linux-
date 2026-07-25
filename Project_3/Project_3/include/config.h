#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
} cfg_entry_t;

typedef struct {
    cfg_entry_t entries[CONFIG_MAX_ENTRIES];
    size_t count;
} cfg_t;

int cfg_load(cfg_t *cfg, const char *path);
const char *cfg_get(const cfg_t *cfg, const char *key, const char *fallback);
int cfg_get_int(const cfg_t *cfg, const char *key, int fallback);
int cfg_get_int_range(const cfg_t *cfg, const char *key, int fallback,
                      int min, int max, int *out);
uint64_t cfg_get_u64(const cfg_t *cfg, const char *key, uint64_t fallback);
int cfg_get_bool(const cfg_t *cfg, const char *key, int fallback);
void cfg_copy_str(const cfg_t *cfg, const char *key, const char *fallback,
                  char *dst, size_t dst_len);

#endif
