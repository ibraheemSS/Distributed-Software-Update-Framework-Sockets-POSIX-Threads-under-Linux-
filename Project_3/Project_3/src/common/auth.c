#include "auth.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static void trim_text(char *s)
{
    s[strcspn(s, "\r\n")] = '\0';

    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static int valid_registry_state(const char *state, int *enabled)
{
    if (!enabled) {
        return 0;
    }
    if (!state || state[0] == '\0' ||
        strcasecmp(state, "enabled") == 0 ||
        strcasecmp(state, "allow") == 0 ||
        strcmp(state, "1") == 0) {
        *enabled = 1;
        return 1;
    }
    if (strcasecmp(state, "disabled") == 0 ||
        strcasecmp(state, "deny") == 0 ||
        strcmp(state, "0") == 0) {
        *enabled = 0;
        return 1;
    }
    return 0;
}

auth_client_status_t auth_client_status(const char *registry_path,
                                        const char *client_id)
{
    if (!registry_path || !client_id || client_id[0] == '\0') {
        return AUTH_CLIENT_INVALID_REGISTRY;
    }

    FILE *fp = fopen(registry_path, "r");
    if (!fp) {
        return AUTH_CLIENT_INVALID_REGISTRY;
    }

    char line[256];
    char seen[512][MAX_ID_LEN];
    size_t seen_count = 0;
    auth_client_status_t result = AUTH_CLIENT_NOT_FOUND;
    while (fgets(line, sizeof(line), fp)) {
        if (!strchr(line, '\n') && !feof(fp)) {
            fclose(fp);
            return AUTH_CLIENT_INVALID_REGISTRY;
        }
        trim_text(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        char *state = NULL;
        char *sep = strchr(line, ':');
        if (!sep) {
            sep = strchr(line, '=');
        }
        if (sep) {
            *sep = '\0';
            state = sep + 1;
            trim_text(state);
        }

        trim_text(line);
        if (line[0] == '\0' || strlen(line) >= MAX_ID_LEN) {
            fclose(fp);
            return AUTH_CLIENT_INVALID_REGISTRY;
        }
        for (size_t i = 0; i < seen_count; i++) {
            if (strcmp(seen[i], line) == 0) {
                fclose(fp);
                return AUTH_CLIENT_INVALID_REGISTRY;
            }
        }
        if (seen_count >= sizeof(seen) / sizeof(seen[0])) {
            fclose(fp);
            return AUTH_CLIENT_INVALID_REGISTRY;
        }
        snprintf(seen[seen_count++], sizeof(seen[0]), "%s", line);

        int enabled = 0;
        if (!valid_registry_state(state, &enabled)) {
            fclose(fp);
            return AUTH_CLIENT_INVALID_REGISTRY;
        }
        if (strcmp(line, client_id) == 0) {
            result = enabled ? AUTH_CLIENT_OK : AUTH_CLIENT_DISABLED;
        }
    }

    fclose(fp);
    return result;
}

int auth_client_registered(const char *registry_path, const char *client_id)
{
    return auth_client_status(registry_path, client_id) == AUTH_CLIENT_OK;
}
