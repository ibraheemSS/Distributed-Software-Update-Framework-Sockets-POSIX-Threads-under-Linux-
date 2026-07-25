#ifndef VERSION_H
#define VERSION_H

#include "common.h"

typedef struct {
    int major;
    int minor;
    int patch;
} semver_t;

int ver_parse(const char *text, semver_t *out);
int ver_compare(const semver_t *a, const semver_t *b);
int ver_compare_text(const char *a, const char *b, int *cmp_out);
int ver_is_valid(const char *text);

#endif
