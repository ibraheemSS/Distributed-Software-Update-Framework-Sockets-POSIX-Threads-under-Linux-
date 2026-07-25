#ifndef PACKAGE_SERVICE_H
#define PACKAGE_SERVICE_H

#include "common.h"
#include "netutil.h"
#include "stats.h"

int pkg_resolve_path(const char *package_dir, const char *package_file,
                     char *out, size_t out_len);
int pkg_file_size(const char *path, unsigned long long *size_out);
int pkg_stream_file(conn_t *conn, const char *path, unsigned long long offset,
                    int chunk_size, int packet_delay_us, int pause_hold_timeout_sec,
                    stats_t *stats,
                    const char *thread_tag, const char *client_info);

#endif
