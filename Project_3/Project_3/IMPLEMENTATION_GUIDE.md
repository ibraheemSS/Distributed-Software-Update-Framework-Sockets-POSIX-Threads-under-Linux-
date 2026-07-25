# Implementation Guide — Distributed Software Update Framework

**Read this file first.** It defines everything shared across modules: environment, conventions, the exact wire protocol, config grammar, security primitives, logging format, the threading model, the build system, and the recommended build order. The three companion documents specify the modules themselves:

- `IMPLEMENTATION_COMMON.md` — `src/common/` and `include/` (config, channel, protocol, tls, auth, sha256, version, logger, threadpool, stats)
- `IMPLEMENTATION_SERVER.md` — `src/server/` (version_manager, package_service, acceptor, worker, dashboard, server_main)
- `IMPLEMENTATION_CLIENT.md` — `src/client/` (getCurrentVersion, downloader, CheckForUpdate, client_gui, client_main)

Every symbol named in the layer documents (types, constants, function prototypes) is defined either here or in `IMPLEMENTATION_COMMON.md`. Where pseudocode appears, it is normative: implement that behavior exactly, including the order of operations and error handling.

---

## 1. Target environment & dependencies

- **OS:** Linux (Ubuntu 22.04 / 24.04 assumed). POSIX APIs only.
- **Compiler:** `gcc`, C11 (`-std=gnu11`), with `-g -Wall -Wextra`.
- **Libraries (link flags):**
  - POSIX threads — `-pthread`
  - OpenSSL ≥ 1.1.1 (3.0 preferred) — `-lssl -lcrypto` (package `libssl-dev`)
  - OpenGL + GLUT — `-lGL -lGLU -lglut` (package `freeglut3-dev`)
  - math — `-lm`
- **Install:** `sudo apt-get install build-essential libssl-dev freeglut3-dev`

OpenSSL 1.1.1+/3.0 self-initializes; do not call the deprecated `SSL_library_init()` family.

---

## 2. Coding conventions (apply to all modules)

1. **Language/standard:** C11. One module = one `.c` in `src/...` plus one `.h` in `include/`.
2. **Include guards:** `#ifndef MODULE_H` / `#define MODULE_H` / `#endif`. No `#pragma once`.
3. **Naming:** `snake_case`. Each module prefixes its public symbols: `cfg_`, `conn_`, `net_`, `proto_`, `tls_`, `auth_`, `sha256_`, `ver_`, `logger_`/`log_`, `tpq_`, `stats_`, `vm_`, `pkg_`, plus the spec-mandated `CheckForUpdate` and `getCurrentVersion` (kept verbatim).
4. **Return convention (the default for all functions):**
   - Functions returning `int`: **`0` = success, `-1` = error**, unless a richer set is explicitly defined for that function (e.g. `proto_recv`).
   - Functions returning a pointer: **`NULL` = error**.
   - Functions returning a count/size: a **negative** value = error.
5. **Error reporting:** on error, the function logs via `log_msg(LOG_ERROR, ...)` (or `LOG_WARN` for recoverable/expected conditions) and returns the error value. Callers do not double-log the same event.
6. **Memory ownership:** the allocator frees, unless a function's doc says it transfers ownership. No global heap state. Per-connection buffers live on the worker's stack or are heap-allocated and freed within the same function.
7. **Thread-safety annotations:** every public function header comment states one of `THREAD-SAFE`, `NOT THREAD-SAFE (call from one thread)`, or `THREAD-SAFE via <lock>`.
8. **No `exit()` in library modules.** Only `main()` may terminate the process. Modules return errors up the stack.
9. **All syscalls and OpenSSL calls are checked.** `read`/`write`/`recv`/`send` partial results are handled by the channel layer (§4). `EINTR` is retried unless shutting down.
10. **`const`-correctness:** read-only parameters are `const`.

---

## 3. Global constants — `include/common.h`

```c
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>

#define MAX_KEY_LEN          64
#define MAX_VALUE_LEN        256
#define MAX_PATH_LEN         256
#define MAX_ID_LEN           64
#define MAX_VERSION_STR      32
#define SHA256_HEX_LEN       64           /* 32 bytes -> 64 lowercase hex chars */
#define SHA256_HEX_BUF       (SHA256_HEX_LEN + 1)

#define FRAME_MAGIC0         0x55         /* 'U' */
#define FRAME_MAGIC1         0x50         /* 'P' */
#define FRAME_MAGIC2         0x44         /* 'D' */
#define FRAME_MAGIC3         0x54         /* 'T' */
#define FRAME_HEADER_SIZE    12
#define PROTO_VERSION        1

#define DEFAULT_CHUNK_SIZE   65536
#define MIN_CHUNK_SIZE       4096
#define MAX_CHUNK_SIZE       (1 << 20)    /* 1 MiB hard cap */
#define MAX_CONTROL_PAYLOAD  1024         /* max bytes for any non-DATA payload */

#define LOG_LINE_MAX         512
#define GUI_LOG_RING         64           /* lines kept in memory for the GUI */
#define LOG_QUEUE_CAP        4096

#endif /* COMMON_H */
```

`common.h` contains constants only — no functions, no includes of other project headers.

---

## 4. The connection channel — `conn_t` (defined in `netutil.h`)

All protocol I/O goes through a channel abstraction backed by TLS. **Modules above the channel never call `read`/`write`/`SSL_read`/`SSL_write` directly.**

```c
typedef struct {
    int   fd;     /* underlying socket; always valid                */
    void *ssl;    /* SSL* when TLS is active, NULL for plaintext     */
} conn_t;
```

I/O contract (implemented in `netutil.c`, see COMMON doc):

- `int conn_write_all(conn_t *c, const void *buf, size_t len);`
  Writes **exactly** `len` bytes (looping over partial writes). Returns `0` on success, `-1` on error. On `ssl != NULL` uses `SSL_write`; else `send`.
- `int conn_read_all(conn_t *c, void *buf, size_t len);`
  Reads **exactly** `len` bytes (looping over partial reads). Returns `0` on success, `-1` on error **or premature EOF**. On `ssl != NULL` uses `SSL_read`; else `recv`.

For blocking sockets, `SSL_read`/`SSL_write` return `> 0` on progress; a return `<= 0` is classified with `SSL_get_error` — treat `SSL_ERROR_WANT_READ`/`WANT_WRITE` as "retry the same call", `SSL_ERROR_ZERO_RETURN` and `SSL_ERROR_SYSCALL` as EOF/error.

---

## 5. Wire protocol reference (normative)

Everything below is carried inside the channel (encrypted when TLS is on).

### 5.1 Frame layout — exactly 12 header bytes, then payload

| Offset | Size | Field | Value |
|-------:|-----:|-------|-------|
| 0 | 1 | magic[0] | `0x55` |
| 1 | 1 | magic[1] | `0x50` |
| 2 | 1 | magic[2] | `0x44` |
| 3 | 1 | magic[3] | `0x54` |
| 4 | 1 | proto_version | `1` |
| 5 | 1 | msg_type | see §5.2 |
| 6 | 1 | flags | bitmask, §5.3 |
| 7 | 1 | reserved | `0` |
| 8 | 4 | payload_len | **big-endian** `uint32`, bytes that follow the header |

`payload_len` MUST be `≤ MAX_CONTROL_PAYLOAD` for every message except `MSG_DATA`, whose `payload_len` MUST be `≤ chunk_size` (and `chunk_size ≤ MAX_CHUNK_SIZE`). A receiver that sees `payload_len` exceeding its buffer capacity treats it as a protocol error and closes.

### 5.2 Message types — `msg_type_t` (in `protocol.h`)

```c
typedef enum {
    MSG_HELLO            = 1,
    MSG_AUTH             = 2,
    MSG_AUTH_OK          = 3,
    MSG_VERSION_REQ      = 4,
    MSG_UPTODATE         = 5,
    MSG_UPDATE_AVAILABLE = 6,
    MSG_DOWNLOAD_REQ     = 7,
    MSG_DATA             = 8,
    MSG_TRANSFER_DONE    = 9,
    MSG_RESULT           = 10,
    MSG_ERROR            = 11,
    MSG_BYE              = 12,
    MSG_PAUSE            = 13,
    MSG_RESUME           = 14
} msg_type_t;
```

### 5.3 Flags

```c
#define FLAG_LAST_CHUNK 0x01   /* set on the final MSG_DATA frame (informational) */
```

### 5.4 Payload grammar

Non-`DATA` payloads are ASCII **key=value pairs joined by `;`**, no trailing `;` required, order-insensitive. Keys and values contain no `;` or `=`. `MSG_DATA` payload is raw binary file bytes. `MSG_AUTH_OK`, `MSG_BYE` carry empty payload (`payload_len = 0`).

| Message | Direction | Payload | Field rules |
|---------|-----------|---------|-------------|
| `MSG_HELLO` | S→C | `srv=updctl;proto=1` | constant |
| `MSG_AUTH` | C->S | `id=<id>` | `id` is the normalized client id and must match the TLS client certificate common name |
| `MSG_AUTH_OK` | S→C | *(empty)* | |
| `MSG_VERSION_REQ` | C→S | `cur=<M.m.p>` | semantic version |
| `MSG_UPTODATE` | S→C | `latest=<M.m.p>` | |
| `MSG_UPDATE_AVAILABLE` | S→C | `ver=<M.m.p>;size=<bytes>;chunk=<bytes>;chunks=<n>;sha256=<hex64>` | `size`,`chunk`,`chunks` decimal uint64 |
| `MSG_DOWNLOAD_REQ` | C→S | `offset=<bytes>` | decimal uint64, `0 ≤ offset ≤ size` |
| `MSG_DATA` | S→C | *(binary)* | length = frame `payload_len` |
| `MSG_TRANSFER_DONE` | S→C | `sha256=<hex64>` | authoritative whole-file digest |
| `MSG_RESULT` | C→S | `status=ok` \| `status=checksum_fail` | |
| `MSG_ERROR` | S↔C | `code=<CODE>;msg=<text>` | `CODE` from §5.5 |
| `MSG_BYE` | S↔C | *(empty)* | graceful close marker |
| `MSG_PAUSE` | C→S | `offset=<bytes>` | GUI pause request during DATA streaming |
| `MSG_RESUME` | C→S | `status=resume` | resume a held stream before server timeout |

### 5.5 Error codes (the `code=` value in `MSG_ERROR`)

`BUSY`, `AUTH_FAIL`, `TLS_FAIL`, `BAD_REQUEST`, `BAD_VERSION`, `NO_PACKAGE`, `CHECKSUM_FAIL`, `TIMEOUT`, `IO_ERROR`. Meanings and handling are in DESIGN.md §11.5.

### 5.6 Canonical message order (happy path)

```
S:HELLO  →  C:AUTH  →  S:AUTH_OK  →  C:VERSION_REQ  →
   S:UPTODATE  →  S:BYE                                        (up to date)
   | OR |
   S:UPDATE_AVAILABLE  →  C:DOWNLOAD_REQ  →  S:DATA…DATA(LAST)
   →  S:TRANSFER_DONE  →  C:RESULT  →  S:BYE                   (update)
```

The server always sends `HELLO` first. `BUSY` is **not** a `MSG_ERROR` on the wire — the acceptor closes a rejected TCP connection before any TLS handshake (see §10 and SERVER doc §4); the client treats a connection closed before/at handshake as a transient busy condition and retries.

---

## 6. Configuration files

### 6.1 Parsing rules (implemented by `cfg_load`, normative)

- File is UTF-8 text, one directive per line.
- Trim leading and trailing ASCII whitespace from each line.
- A line that is empty, or whose first non-whitespace char is `#`, is a **comment** and is skipped. (Only full-line `#` comments. No inline comments. Values must not contain `#`.)
- Otherwise split on the **first** `=`: text before is the key (trim), text after is the value (trim). A line without `=` is a parse error → `cfg_load` returns `-1`.
- Duplicate keys: last one wins.
- Unknown keys are ignored (forward compatible). Missing keys fall back to the caller's default.

### 6.2 `server.conf` keys

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `PORT` | int | 5500 | listen port |
| `MAX_WORKERS` | int | 16 | fixed worker pool size |
| `ACCEPT_BACKLOG` | int | 64 | `listen()` backlog |
| `QUEUE_CAPACITY` | int | 128 | bounded work-queue capacity |
| `MANIFEST_PATH` | path | ./config/manifest.conf | version manifest |
| `PACKAGE_DIR` | path | ./packages | package directory |
| `LOG_FILE` | path | ./logs/server.log | log output |
| `LOG_LEVEL` | enum | INFO | DEBUG/INFO/WARN/ERROR |
| `CHUNK_SIZE` | int | 65536 | clamp to [MIN_CHUNK_SIZE, MAX_CHUNK_SIZE] |
| `CLIENT_TIMEOUT_SEC` | int | 30 | socket recv/send timeout |
| `PAUSE_HOLD_TIMEOUT_SEC` | int | 5 | server-side hold time for a paused download connection |
| `TLS_CERT` | path | ./config/server.crt | PEM cert |
| `TLS_KEY` | path | ./config/server.key | PEM private key |
| `CA_CERT` | path | ./config/ca.crt | CA certificate used to verify client certificates |
| `CLIENT_REGISTRY` | path | ./config/clients.keys | authorized client-id table after mTLS authentication |

### 6.3 `client.conf` keys

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `SERVER_HOST` | string | 127.0.0.1 | server host/IP |
| `SERVER_PORT` | int | 5500 | server port |
| `CLIENT_VERSION_FILE` | path | ./config/client_versions.txt | shared table of `client_id=installed_version` rows |
| `CLIENT_REGISTRY` | path | ./config/clients.keys | authorized client-id table |
| `CLIENT_DATA_DIR` | path | ./clients | base directory for per-client downloads/logs |
| `CURRENT_VERSION` | string | *(unset)* | inline override of installed version |
| `DOWNLOAD_DIR` | path | *(derived)* | where packages land |
| `RETRY_MAX` | int | 5 | max connection attempts |
| `RETRY_BACKOFF_MS` | int | 500 | base backoff |
| `TIMEOUT_SEC` | int | 30 | socket timeout |
| `PAUSE_TIMEOUT_SEC` | int | 30 | max time a GUI pause may wait for Resume |
| `LOG_FILE` | path | ./logs/client.log | log output |
| `SERVER_CA` | path | ./config/ca.crt | CA certificate used to verify the server certificate |
| `SERVER_TLS_NAME` | string | update-server | expected server certificate identity |
| `CLIENT_ID` | string | prompt | numeric input such as `1`, `01`, or `001`; normalized to `client_001` |
| `CLIENT_CERT` | path | ./config/clients/<id>.crt | client certificate sent during TLS handshake |
| `CLIENT_KEY` | path | ./config/clients/<id>.key | client private key, never transmitted |

### 6.4 `manifest.conf` keys

| Key | Type | Meaning |
|-----|------|---------|
| `LATEST_VERSION` | string | semantic version |
| `PACKAGE_FILE` | string | filename inside `PACKAGE_DIR` |
| `CHECKSUM_SHA256` | hex64 | whole-file digest of the package |
| `MIN_SUPPORTED` | string | minimum upgradeable version |
| `RELEASE_NOTES` | string | free text |

---

## 7. Security primitives (normative details)

### 7.1 Mutual TLS provisioning

The project uses SSL/TLS for both encryption and peer authentication.

- `ca.key` signs certificates and stays offline/secret.
- `ca.crt` is the public trust anchor installed on server and clients.
- `server.key` stays on the server; `server.crt` is sent to clients during TLS.
- `client_###.key` stays on that client; `client_###.crt` is sent to the server during TLS.
- `clients.keys` is not a token store. It is an authorization registry checked after TLS authenticates the certificate identity.

Certificate common names are used as identities:

```text
server.crt      CN=update-server
client_001.crt  CN=client_001
```

### 7.2 TLS context setup

Server:
```c
SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM);   // check ret
SSL_CTX_use_PrivateKey_file (ctx, key_path,  SSL_FILETYPE_PEM);   // check ret
SSL_CTX_check_private_key(ctx);                                   // check ret
SSL_CTX_load_verify_locations(ctx, ca_path, NULL);                 // check ret
SSL_CTX_set_verify(ctx,
    SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
    NULL);
```
Client:
```c
SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
SSL_CTX_load_verify_locations(ctx, ca_path, NULL);                  // check ret
SSL_CTX_use_certificate_file(ctx, client_cert, SSL_FILETYPE_PEM);   // check ret
SSL_CTX_use_PrivateKey_file (ctx, client_key,  SSL_FILETYPE_PEM);   // check ret
SSL_CTX_check_private_key(ctx);                                     // check ret
SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
```

Per-connection client setup also calls:

```c
SSL_set_tlsext_host_name(ssl, "update-server");
SSL_set1_host(ssl, "update-server");
```

That means the client verifies both the CA chain and the expected server identity. Per-connection handshake: `SSL_new(ctx)` -> `SSL_set_fd(ssl, fd)` -> `SSL_accept`/`SSL_connect` (return `1` on success for blocking sockets). Teardown: `SSL_shutdown(ssl)` then `SSL_free(ssl)`.

### 7.3 Application authentication after TLS

After mutual TLS succeeds, the server extracts the client certificate common name with `SSL_get_peer_certificate()` and `X509_NAME_get_text_by_NID(..., NID_commonName, ...)`.

The client then sends:

```text
MSG_AUTH id=client_001
```

The server accepts only if:

1. The TLS client certificate CN is `client_001`.
2. The `MSG_AUTH` id is also `client_001`.
3. `client_001` appears in `CLIENT_REGISTRY`.

This keeps authentication in TLS and authorization in the registry.

---

## 8. Logging (format + API contract)

### 8.1 Exact line format

```
YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [thread-tag] [client-info] message
```
- Timestamp from `clock_gettime(CLOCK_REALTIME)`, formatted via `localtime_r` + `.mmm` milliseconds.
- `LEVEL` is space-padded to 5 chars: `DEBUG`, `INFO `, `WARN `, `ERROR`.
- `thread-tag` examples: `main`, `acceptor`, `worker-03`, `logger`, `client`.
- `client-info` is `IP:PORT fd=N`, or `-` when not applicable.
- `message` is the caller's `printf`-formatted text.

### 8.2 API (in `logger.h`)

```c
typedef enum { LOG_DEBUG=0, LOG_INFO=1, LOG_WARN=2, LOG_ERROR=3 } log_level_t;

int  logger_start(const char *path, log_level_t min_level);   /* spawns logger thread */
void logger_stop(void);                                       /* drain queue, join thread */
void log_msg(log_level_t lvl, const char *thread_tag,
             const char *client_info, const char *fmt, ...);  /* THREAD-SAFE; formats + enqueues */
int  logger_recent(char dst[][LOG_LINE_MAX], int max);        /* THREAD-SAFE; copies last lines for GUI, returns count */
```
Behavior: `log_msg` builds the full line, drops it if its level `< min_level`, otherwise enqueues onto a bounded queue (`LOG_QUEUE_CAP`). The logger thread pops lines, writes+flushes to the file, and stores them in a mutex-guarded ring of `GUI_LOG_RING` lines for `logger_recent`. If the queue is full, `log_msg` drops the line and increments an internal `dropped` counter (flushed as a `WARN` when space returns). `logger_stop` sets a stop flag, signals, drains remaining lines, joins.

---

## 9. GUI snapshot contracts (defined in `stats.h` / `gui.h`)

The render thread (always the process **main** thread) only ever **reads** a snapshot; producers only **write**. No GL call is ever made off the main thread.

Server side — atomic counters (`stats.h`):
```c
#include <stdatomic.h>
typedef struct {
    atomic_ulong       connections;
    atomic_ulong       active_transfers;
    atomic_ulong       updates_served;
    atomic_ulong       uptodate_responses;
    atomic_ulong       auth_failures;
    atomic_ulong       errors;
    atomic_ullong      bytes_sent;
    atomic_int         busy_workers;
    int                max_workers;      /* set once at init */
} stats_t;

void stats_init(stats_t *s, int max_workers);
```
The dashboard reads these atomics directly each frame and calls `logger_recent` for the log ticker; throughput is derived in the GUI by differencing `bytes_sent` between frames.

Client side — mutex-guarded snapshot (`gui.h`):
```c
#include <pthread.h>
typedef struct {
    pthread_mutex_t lock;
    char            status[32];          /* "connecting","authenticating","downloading","up to date","done","failed" */
    char            current_version[MAX_VERSION_STR];
    char            latest_version[MAX_VERSION_STR];
    unsigned long long bytes_done;
    unsigned long long bytes_total;
    double          speed_bps;
    char            log_lines[8][160];
    int             log_count;
} client_gui_state_t;

void client_gui_state_init(client_gui_state_t *g);
void client_gui_set_status(client_gui_state_t *g, const char *status);     /* THREAD-SAFE */
void client_gui_set_progress(client_gui_state_t *g, unsigned long long done,
                             unsigned long long total, double speed_bps);  /* THREAD-SAFE */
```
Each setter locks, updates, unlocks. The render callback locks, copies fields to locals, unlocks, then draws.

---

## 10. Threading model & lock inventory

**Server process** (one process):
| Thread | Count | Runs |
|--------|------:|------|
| main | 1 | startup, then mandatory GLUT loop (dashboard) |
| acceptor | 1 | `accept()` loop → `tpq_push` |
| worker | `MAX_WORKERS` | `tpq_pop` → TLS handshake → auth → session |
| logger | 1 | drains log queue to file + ring |

**Client process:**
| Thread | Count | Runs |
|--------|------:|------|
| main | 1 | mandatory GLUT loop (progress window) |
| update | 1 | `CheckForUpdate` (only when GUI is on, so the main thread can render) |
| logger | 1 | client log |

**Lock inventory** (no lock is ever held across network/disk I/O; no two locks are ever held simultaneously):
- work-queue mutex + 1 condvar (`not_empty`) — threadpool
- version-manager `pthread_rwlock_t`
- logger queue mutex + condvar; logger ring mutex
- client GUI state mutex
- stats: lock-free atomics

---

## 11. Recommended implementation order (milestones)

Build and test in this sequence so you are never debugging two hard things at once. TLS and GUI are mandatory in the final build, but early throwaway local experiments may stub transport while the protocol is being shaped.

1. `common.h`, `config`, `version`, `sha256` — plus a tiny `main` that loads a config and prints values, hashes a file.
2. `netutil` (`conn_t` plaintext path), `protocol` (pack/unpack, `proto_send`/`proto_recv`, `kv_get`) — validate by sending a `HELLO` between two trivial test programs.
3. `threadpool` queue, `logger` (async + ring).
4. `version_manager`, `package_service`.
5. Server `worker` session + `acceptor` + `server_main`; client `downloader` + `CheckForUpdate` + `client_main` — full happy path.
6. Add resume (offset) and the retry/backoff loop; wire the error taxonomy.
7. Add `tls` module and generate CA/server/client certificates.
8. Add mTLS authentication: verify server name on the client, require client certs on the server, compare certificate CN to `MSG_AUTH id`, then check `CLIENT_REGISTRY`.
9. Add `dashboard` (server GUI) and `client_gui`.
10. Wire `stats` everywhere; run the `tests/run_tests.sh` scenarios.

---

## 12. Build system & one-time setup

### 12.1 `Makefile`

```makefile
CC       := gcc
CFLAGS   := -std=gnu11 -g -Wall -Wextra -O2 -Iinclude -pthread
LDFLAGS  := -pthread -lm
LIBS_GL  := -lGL -lGLU -lglut
LIBS_SSL := -lssl -lcrypto

COMMON_SRC := $(wildcard src/common/*.c)
SERVER_SRC := $(wildcard src/server/*.c)
CLIENT_SRC := $(wildcard src/client/*.c)

COMMON_OBJ := $(COMMON_SRC:.c=.o)
SERVER_OBJ := $(SERVER_SRC:.c=.o)
CLIENT_OBJ := $(CLIENT_SRC:.c=.o)

all: server client

server: $(SERVER_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS_GL) $(LIBS_SSL)

client: $(CLIENT_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS_GL) $(LIBS_SSL)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(COMMON_OBJ) $(SERVER_OBJ) $(CLIENT_OBJ) server client

.PHONY: all clean
```

### 12.2 CA + server/client certificate provisioning

```bash
mkdir -p config/clients logs packages downloads

# Project CA. ca.key signs certificates and must stay secret.
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout config/ca.key -out config/ca.crt -subj "/CN=Project-3-CA"

# Server certificate signed by the project CA.
openssl req -newkey rsa:2048 -nodes \
    -keyout config/server.key -out config/server.csr -subj "/CN=update-server"
openssl x509 -req -in config/server.csr -CA config/ca.crt -CAkey config/ca.key \
    -CAcreateserial -out config/server.crt -days 365 -sha256

# One client certificate/key pair. Repeat for client_002, etc.
openssl req -newkey rsa:2048 -nodes \
    -keyout config/clients/client_001.key \
    -out config/clients/client_001.csr -subj "/CN=client_001"
openssl x509 -req -in config/clients/client_001.csr \
    -CA config/ca.crt -CAkey config/ca.key -CAcreateserial \
    -out config/clients/client_001.crt -days 365 -sha256

# a fake package to serve
head -c 10485760 /dev/urandom > packages/app-1.4.0.bin
SHA=$(sha256sum packages/app-1.4.0.bin | awk '{print $1}')
echo "CHECKSUM_SHA256 = $SHA"                 # copy into manifest.conf
```

### 12.3 Run

```bash
./server config/server.conf      # terminal 1
./client config/client.conf      # terminal 2 (one per client)
```

---

## 13. Directory layout (target)

```
software-update-framework/
├── Makefile
├── include/   common.h config.h netutil.h tls.h protocol.h auth.h sha256.h
│              version.h logger.h threadpool.h stats.h gui.h
│              version_manager.h package_service.h
├── src/
│   ├── common/  config.c netutil.c tls.c protocol.c auth.c sha256.c
│   │            version.c logger.c threadpool.c stats.c
│   ├── server/  server_main.c acceptor.c worker.c version_manager.c
│   │            package_service.c dashboard.c
│   └── client/  client_main.c downloader.c client_gui.c
├── config/   server.conf client.conf manifest.conf ca.crt ca.key server.crt server.key
│             clients/client_001.crt clients/client_001.key ...
├── packages/ app-1.4.0.bin
├── downloads/
├── logs/
└── tests/    run_tests.sh
```

Proceed to `IMPLEMENTATION_COMMON.md`.
