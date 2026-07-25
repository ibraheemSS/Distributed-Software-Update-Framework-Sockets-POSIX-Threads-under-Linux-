#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void cfg_errorf(const char *fmt, ...)
{
    static int stderr_is_tty = -1;
    va_list ap;

    if (stderr_is_tty < 0) {
        stderr_is_tty = isatty(fileno(stderr));
    }
    if (stderr_is_tty) {
        fputs("\033[31m", stderr);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (stderr_is_tty) {
        fputs("\033[0m", stderr);
    }
    fputs("\n\n", stderr);
}

static char *trim_ascii(char *s)
{
    unsigned char *p = (unsigned char *)s;
    while (*p && isspace(*p)) {
        p++;
    }

    char *start = (char *)p;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return start;
}

static int cfg_set(cfg_t *cfg, const char *key, const char *value)
{
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            return -1;
        }
    }

    if (cfg->count >= CONFIG_MAX_ENTRIES) {
        cfg_errorf("config: too many entries near '%s'", key);
        return -1;
    }

    snprintf(cfg->entries[cfg->count].key, sizeof(cfg->entries[cfg->count].key), "%s", key);
    snprintf(cfg->entries[cfg->count].value, sizeof(cfg->entries[cfg->count].value), "%s", value);
    cfg->count++;
    return 0;
}

int cfg_load(cfg_t *cfg, const char *path)
{
    if (!cfg || !path) {
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));
    FILE *fp = fopen(path, "r");
    if (!fp) {
        cfg_errorf("config: cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    char line[1024];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        line[strcspn(line, "\r\n")] = '\0';
        char *s = trim_ascii(line);
        if (*s == '\0' || *s == '#') {
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            cfg_errorf("%s:%d: expected KEY=VALUE", path, line_no);
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        char *key = trim_ascii(s);
        char *value = trim_ascii(eq + 1);
        if (*key == '\0' || strlen(key) >= MAX_KEY_LEN || strlen(value) >= MAX_VALUE_LEN) {
            cfg_errorf("%s:%d: invalid key or value", path, line_no);
            fclose(fp);
            return -1;
        }
        if (cfg_set(cfg, key, value) != 0) {
            cfg_errorf("%s:%d: duplicate config key '%s'", path, line_no, key);
            fclose(fp);
            return -1;
        }
    }

    if (ferror(fp)) {
        cfg_errorf("config: cannot read %s: %s", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

const char *cfg_get(const cfg_t *cfg, const char *key, const char *fallback)
{
    if (!cfg || !key) {
        return fallback;
    }
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            return cfg->entries[i].value;
        }
    }
    return fallback;
}

int cfg_get_int(const cfg_t *cfg, const char *key, int fallback)
{
    const char *s = cfg_get(cfg, key, NULL);
    if (!s || *s == '\0') {
        return fallback;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0') {
        return fallback;
    }
    return (int)v;
}

int cfg_get_int_range(const cfg_t *cfg, const char *key, int fallback,
                      int min, int max, int *out)
{
    if (!out || min > max || fallback < min || fallback > max) {
        return -1;
    }
    const char *s = cfg_get(cfg, key, NULL);
    if (!s) {
        *out = fallback;
        return 0;
    }
    if (*s == '\0') {
        cfg_errorf("config: '%s' must be a number", key);
        return -1;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v < min || v > max) {
        cfg_errorf("config: '%s' must be a number from %d to %d", key, min, max);
        return -1;
    }
    *out = (int)v;
    return 0;
}

uint64_t cfg_get_u64(const cfg_t *cfg, const char *key, uint64_t fallback)
{
    const char *s = cfg_get(cfg, key, NULL);
    if (!s || *s == '\0') {
        return fallback;
    }
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end != '\0') {
        return fallback;
    }
    return (uint64_t)v;
}

int cfg_get_bool(const cfg_t *cfg, const char *key, int fallback)
{
    const char *s = cfg_get(cfg, key, NULL);
    if (!s) {
        return fallback;
    }
    if (strcasecmp(s, "1") == 0 || strcasecmp(s, "true") == 0 ||
        strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0) {
        return 1;
    }
    if (strcasecmp(s, "0") == 0 || strcasecmp(s, "false") == 0 ||
        strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0) {
        return 0;
    }
    return fallback;
}

void cfg_copy_str(const cfg_t *cfg, const char *key, const char *fallback,
                  char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", cfg_get(cfg, key, fallback ? fallback : ""));
}
