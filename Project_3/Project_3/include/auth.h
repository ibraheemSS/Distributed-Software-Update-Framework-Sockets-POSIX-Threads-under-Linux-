#ifndef AUTH_H
#define AUTH_H

#include "common.h"

typedef enum {
    AUTH_CLIENT_OK = 0,
    AUTH_CLIENT_NOT_FOUND = 1,
    AUTH_CLIENT_DISABLED = 2,
    AUTH_CLIENT_INVALID_REGISTRY = 3
} auth_client_status_t;

auth_client_status_t auth_client_status(const char *registry_path,
                                        const char *client_id);
int auth_client_registered(const char *registry_path, const char *client_id);

#endif
