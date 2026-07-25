# Distributed Software Update Framework

This is a complete C11 client/server update service for the ENCS4330 sockets and POSIX threads project. It implements the design in `DESIGN.md` and `IMPLEMENTATION_GUIDE.md`: TCP sockets, a bounded pthread worker pool, framed messages, mandatory mutual TLS authentication/encryption, SHA-256 verification, resume from partial downloads, async logging, runtime config files, statistics, and mandatory OpenGL dashboards.

## Build

On Ubuntu 22.04/24.04:

```bash
sudo apt-get update
sudo apt-get install build-essential libssl-dev freeglut3-dev
make
```

Mutual TLS authentication/encryption and GUI are always compiled in and always enabled. The Makefile intentionally links OpenSSL and OpenGL every time.

## Run

The repository includes an 8 MiB demo package, demo TLS certificate/key, and demo client identities. In one terminal:

```bash
make run_server
```

In another:

```bash
make run_client
```

The default client config asks for a client ID at startup. Try:

```text
001
```

`make run_client` is a friendly demo target: invalid input, unauthorized IDs, and Ctrl+C are reported by the client and then summarized without Make's generic `Error 1` line. Run `./client config/client.conf` directly when you want the raw process exit code for scripts.

The targets accept config overrides. These sample configs skip the prompt by providing a default ID:

```bash
make run_server SERVER_CONF=config/server_large.conf
make run_client CLIENT_CONF=config/clients/client_002.conf
```

The first `001` run normalizes to registered ID `client_001`, starts from the `client_001=1.2.0` row in `config/client_versions.txt`, downloads the package selected by `config/manifest.conf` (default: `packages/app-1.5.0-medium.bin`), verifies its SHA-256, atomically writes `clients/client_001/downloads/app-1.5.0.bin`, and updates only the `client_001` row to `1.5.0`. The server streams the package as 32 KiB framed DATA packets with a short delay between packets, so the download takes visible time and the GUI progress/throughput changes in real time. The next run should report up-to-date without showing a progress bar.

The server dashboard shows currently active clients by authenticated ID after login, not the historical total number of clients served.

## Security Setup

The committed certificates are for local demonstration only. Regenerate them before a real demo:

```bash
mkdir -p config/clients

openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout config/ca.key -out config/ca.crt -subj "/CN=Project-3-CA"

openssl req -newkey rsa:2048 -nodes \
  -keyout config/server.key -out config/server.csr -subj "/CN=update-server"
openssl x509 -req -in config/server.csr -CA config/ca.crt -CAkey config/ca.key \
  -CAcreateserial -out config/server.crt -days 365 -sha256

openssl req -newkey rsa:2048 -nodes \
  -keyout config/clients/client_001.key \
  -out config/clients/client_001.csr -subj "/CN=client_001"
openssl x509 -req -in config/clients/client_001.csr \
  -CA config/ca.crt -CAkey config/ca.key -CAcreateserial \
  -out config/clients/client_001.crt -days 365 -sha256
```

If you regenerate the package, update the manifest checksum:

```bash
SHA=$(sha256sum packages/app-1.5.0-medium.bin | awk '{print $1}')
sed -i "s/^CHECKSUM_SHA256=.*/CHECKSUM_SHA256=$SHA/" config/manifest.conf
```

## Configuration

All operational tunables live in `config/server.conf`, `config/client.conf`, and `config/manifest.conf`. GUI and mutual TLS are not tunables; they are always active. On Windows, build/run inside WSL or a real Linux environment; MinGW/UCRT does not provide the POSIX socket headers used by this Linux project.

## Client IDs & Authentication

Client IDs are authorized in [config/clients.keys](config/clients.keys), one row per allowed client:

```text
client_001:enabled
client_002:enabled
```

Authentication itself is done by mutual TLS. The server verifies the client certificate using `config/ca.crt`, extracts the certificate identity such as `client_001`, and then checks that the ID is authorized in `clients.keys`.

So the checks are:

1. the client owns a private key matching a CA-signed client certificate
2. the certificate identity matches the normalized client ID sent in `MSG_AUTH`
3. the client ID exists in `config/clients.keys`

The default client prompts for a numeric ID only. Inputs like `1`, `01`, and `001` all normalize to `client_001`; words such as `client_001` are rejected at the prompt. The client loads `config/clients/client_001.crt` and `config/clients/client_001.key`, verifies the server certificate name `update-server`, and then proceeds only after the TLS handshake succeeds.

When the client is running interactively, invalid client input is not fatal. The prompt repeats for non-numeric values, out-of-range numbers, disabled clients, or unregistered client numbers. A malformed or duplicated registry still fails immediately because retrying user input cannot repair the registry file.

Duplicate keys in config files and duplicate client rows in `clients.keys` or `client_versions.txt` are treated as configuration errors. The program fails closed instead of choosing one duplicated value silently.

Only one active process may use a given client ID at a time. The client takes a per-ID OS lock under `clients/<client_id>/client.lock`, and the server also reserves authenticated client IDs during active TLS sessions. A second session for the same ID is rejected with a clear duplicate-client error.

## Tests

```bash
tests/run_tests.sh
```

The test script requires a graphical Linux session because both server and client GUIs are mandatory. It runs a single outdated client, an up-to-date client, and multiple concurrent outdated clients with separate version/download folders.

For one focused real-time download demonstration:

```bash
tests/realtime_demo.sh
```

That demo resets the `client_001` row in `config/client_versions.txt` to `1.2.0`, downloads the full 8 MiB package through TLS-authenticated framed packets, and verifies the resulting file checksum.

## Signals & Resume

Both server and client handle `SIGINT`/`SIGTERM`. The server stops accepting new clients, wakes workers, waits for active sessions to finish or fail, drains logs, and frees TLS/thread resources.

On the client, `Ctrl+C` pauses an active download. The client keeps the partially downloaded `.part` file, writes a `.resume` metadata file with the saved offset, closes the connection, drains logs, and exits cleanly. Running the same client again resumes from the `.part` file offset.

The client GUI also has interactive `Pause` and `Resume` controls during downloads. `Pause` saves the current offset, sends `MSG_PAUSE`, and keeps the TLS connection open while the server waits up to `PAUSE_HOLD_TIMEOUT_SEC` (default: 5 seconds). If `Resume` is clicked before that server hold expires, the client sends `MSG_RESUME` and streaming continues on the same TLS connection. If the hold expires, clicking `Resume` reconnects immediately and continues from the saved `.part` offset. `PAUSE_TIMEOUT_SEC` controls how long the client GUI may stay paused before it exits cleanly with resume data saved.

If the server is terminated while a client is downloading, the client saves the current offset in the `.resume` file, keeps the `.part` file, closes the broken TLS connection, shows that it is trying to reconnect for 10 seconds, and then retries with a fresh TLS connection. If the server comes back before `RETRY_MAX` is exhausted, the same client resumes from the saved `.part` offset.

When a client is already up to date, the GUI remains visible for 10 seconds with a clear up-to-date message before closing. After a successful install, the client also keeps the GUI open for 10 seconds and shows that the update was installed and SHA-256 verified.

## Multi-Client Simulation

Client versions are stored in one shared table: [config/client_versions.txt](config/client_versions.txt).

```text
client_001=1.2.0
client_002=1.2.0
client_003=1.4.0
```

The client asks for a numeric ID, normalizes it to the canonical `client_###` ID, reads that ID's row, and updates only that row after a successful update. The table is protected by a lock file during writes, so parallel client processes do not corrupt it.

Ready-made examples are included:

```bash
make run_client CLIENT_CONF=config/clients/client_001.conf
make run_client CLIENT_CONF=config/clients/client_002.conf
make run_client CLIENT_CONF=config/clients/client_003.conf
make run_client CLIENT_CONF=config/clients/client_004.conf
```

Each one uses the shared `CLIENT_VERSION_FILE` and `CLIENT_REGISTRY`, while downloads/logs are automatically placed under `clients/<client_id>/`.

Run different IDs for true parallel simulation. Starting the same ID twice is intentionally blocked so two processes cannot write the same download state or impersonate the same registered client concurrently.

## Package Scenarios

Three package sizes are included:

```text
config/server_small.conf   -> 2 MiB package, latest 1.4.0
config/server_medium.conf  -> 8 MiB package, latest 1.5.0
config/server_large.conf   -> 24 MiB package, latest 2.0.0
```

Run one with:

```bash
make run_server SERVER_CONF=config/server_large.conf
```
