#include "version.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int parse_part(const char **p, int *out)
{
    if (!isdigit((unsigned char)**p)) {
        return -1;
    }
    char *end = NULL;
    long value = strtol(*p, &end, 10);
    if (end == *p || value < 0 || value > 999999) {
        return -1;
    }
    *out = (int)value;
    *p = end;
    return 0;
}

int ver_parse(const char *text, semver_t *out)
{
    if (!text || !out) {
        return -1;
    }

    const char *p = text;
    semver_t v;
    if (parse_part(&p, &v.major) != 0 || *p++ != '.') {
        return -1;
    }
    if (parse_part(&p, &v.minor) != 0 || *p++ != '.') {
        return -1;
    }
    if (parse_part(&p, &v.patch) != 0 || *p != '\0') {
        return -1;
    }

    *out = v;
    return 0;
}

int ver_compare(const semver_t *a, const semver_t *b)
{
    if (a->major != b->major) {
        return (a->major > b->major) ? 1 : -1;
    }
    if (a->minor != b->minor) {
        return (a->minor > b->minor) ? 1 : -1;
    }
    if (a->patch != b->patch) {
        return (a->patch > b->patch) ? 1 : -1;
    }
    return 0;
}

int ver_compare_text(const char *a, const char *b, int *cmp_out)
{
    semver_t va;
    semver_t vb;
    if (ver_parse(a, &va) != 0 || ver_parse(b, &vb) != 0 || !cmp_out) {
        return -1;
    }
    *cmp_out = ver_compare(&va, &vb);
    return 0;
}

int ver_is_valid(const char *text)
{
    semver_t v;
    return ver_parse(text, &v) == 0;
}
