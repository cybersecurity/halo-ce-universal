# Browser-to-native Halo relay

This service connects the browser network transport to **one native Halo host
identified by a valid invite**. GitHub Pages serves the game; this separate
process runs on a Linux host with outbound TCP/UDP access. It is not an HTTP
proxy, arbitrary UDP tunnel, game server, or game-data download service.

The current native game speaks its own encrypted UDP/KCP protocol. A WebSocket
alone cannot talk to it. Each accepted browser session owns a native
`port/relay/halo-relay-worker` process, which joins through the existing native
P2P implementation and only forwards packets to the authenticated invite host.
Browser-only WebRTC rooms remain a separate transport.

## Local development

Requirements: Node 22+; Linux x86 with a 32-bit C toolchain for the worker.

```sh
make -C port/relay
cd tools/browser-relay
npm ci --ignore-scripts
npm test
RELAY_ALLOWED_ORIGINS=http://localhost:8780 npm start
```

The default listener is `127.0.0.1:8789`; the WebSocket endpoint is `/join`,
and `GET /health` reports service liveness. The health endpoint does not prove
that a native invite is live or that a multiplayer game has connected.
Localhost may omit the access token. A non-loopback listener refuses startup
without a token of at least 32 bytes, even if it is behind a reverse proxy.

`npm test` exercises the real WebSocket server using a **mock native worker**:
framing, packet restrictions, authentication, worker lifecycle, connection and
traffic limits, and stalled-worker backpressure. These tests do not establish
native game interoperability; the native worker has its own integration tests.

For the combined transport test, build `make -C port/relay halo-relay-worker-test`
and run:

```sh
NATIVE_WORKER="$PWD/port/relay/halo-relay-worker-test" npm --prefix tools/browser-relay run test:native
```

That test starts its own local MQTT broker and native echo host, then uses the
real frontend and native worker to check encrypted UDP/discovery, KCP stream
ordering, repeated connections, and cleanup. It never joins a public host.
It validates the transport, **not a Halo multiplayer match**. The test-only
binary permits local broker/candidate addresses and must never be deployed.

## Container and HTTPS

Build from the repository root. The 32-bit native ABI requires an amd64 Linux
image, including when building on an ARM computer:

```sh
docker build --platform linux/amd64 -f tools/browser-relay/Dockerfile -t halo-browser-relay .
```

Create a private operator environment file (do not commit it), containing:

```text
RELAY_ACCESS_TOKEN=<random secret, at least 32 bytes>
RELAY_ALLOWED_ORIGINS=https://fqlx.github.io,https://abwburns.com
RELAY_MAX_SESSIONS=8
```

Generate the secret with `openssl rand -hex 32`. It is a capability for a small
trusted group, not a public account system. Give it to intended players
separately; **never put it in GitHub Pages source, a Discord invite URL, logs,
or a tracked environment file**. Public access needs a separate short-lived
credential issuer and usage controls; this service does not provide one.

```sh
docker run --init --read-only --cap-drop=ALL --security-opt=no-new-privileges \
  --pids-limit=64 --memory=512m --tmpfs /tmp:size=16m,noexec,nosuid \
  --env-file /private/path/halo-relay.env \
  -p 127.0.0.1:8789:8789 --name halo-browser-relay halo-browser-relay
```

Terminate HTTPS on the operator's reverse proxy. For example, a Caddy site
can forward WebSocket upgrades with:

```caddyfile
relay.example.com {
    reverse_proxy 127.0.0.1:8789
}
```

Configure the browser's trusted site configuration to use
`wss://relay.example.com/join`, and enter the capability in its relay access
field. The browser must not accept an arbitrary relay host from an invite URL.
The relay uses only the real TCP peer address for rate accounting, not client
supplied forwarding headers. A reverse proxy therefore shares a connection
limit across its clients. Extend this deliberately if deploying multiple
trusted proxies.

No extra host port is needed for the game worker's loopback sockets. Its UDP
tunnel uses outbound traffic and STUN. A VPS with suitable public networking
is preferable; restrictive NAT can still prevent a native host connection.
The WebSocket endpoint must stay private to the intended players and the
operator must keep the Node runtime and `ws` dependency updated.

## Protocol and boundaries

1. Connect to `/join` using an allowed exact `Origin`. Tokens are not URL
   parameters; unknown paths or any query string are rejected.
2. Send a text frame:
   `{"type":"join","invite":"<44 hexadecimal digits>","accessToken":"<capability>"}`.
3. The worker returns `{"type":"ready","protocol":1,"identifier":"<12 hex>","address":N}`.
   The address is the native-generated IPv4 integer in the web engine's byte
   representation, inside `100.64.0.0/10`; it is not a browser-selected address.
4. A `peer` event reports the invite host's identifier, virtual address, and
   `connected` boolean. The browser enables joining only after connection.
5. Binary messages are the existing `web_packet_header` plus payload, with
   four-byte padding. Size/kind/length are little-endian; IPv4 addresses and
   ports retain the native packet bytes. Header size is 24 bytes.

The frontend and worker both validate packet sizes, addresses and ports.
Datagram payloads are at most 1,395 bytes and use game ports 5150/5151.
Stream chunks are at most 16,000 bytes; a stream opens on native port 5150
from an unprivileged source port. There are at most 16 stream tuples per
session. Only the assigned browser address, authenticated host address, and
game discovery broadcasts are accepted. No browser field chooses a DNS name,
socket destination, process argument, or relay command.

Default bounds are eight sessions, two MiB/second and 2,000 packets/second
per session (two-second bursts), one MiB queues in either direction, a
10-second initial handshake deadline, a 100-second host connection deadline,
two-minute idle expiry, and a six-hour session lifetime. Both directions apply
backpressure; if either side remains stalled for 15 seconds, the whole session
closes. Reliable stream bytes are never intentionally dropped while keeping
the connection alive. The worker terminates when its WebSocket closes and is
force-killed if necessary. Native stderr is consumed without logging because
the underlying P2P implementation may include invite secrets in diagnostics.

The worker's stdin/stdout use length-prefixed records: `u32le length`,
`u8 type`, then payload, with length including the type byte and at most
65,536 bytes. Input type 1 is the one initial bare invite; type 2 carries a
web packet. Output type 3 is a JSON event, and type 2 carries a web packet.
Stdout must contain only these records. The relay does not pass its access
token to the worker's arguments or environment.
