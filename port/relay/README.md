# Native invite relay worker (prototype)

This Linux worker joins one native `halo://join/<44 hexadecimal digits>` invite
and bridges its socket traffic to one browser session. It links the existing
native P2P signalling, cryptography and KCP transport. It does not run Halo,
load maps, host a match, or change the native peer's protocol.

A browser must run a source-built engine with the same network version as the
host. The published Apollo binary cannot use this worker. The WebSocket frontend
is in `tools/browser-relay`; this executable itself speaks only to stdin/stdout.

## Build and test

On a Linux amd64 machine with GCC multilib, make and Python 3:

```sh
sudo apt-get install gcc-multilib make python3
make -C port/relay
make -C port/relay test
```

The program deliberately builds with `-m32`: the native P2P types have the game's
32-bit ABI. No XDK, graphics libraries, ISO, SDL or game assets are needed. The
runtime needs the 32-bit glibc loader (`libc6-i386` on Debian/Ubuntu).

`test_relay.py` starts a private MQTT test broker and two native P2P processes.
One is a socket echo host, and the other bridges browser frames through the real
encrypted native UDP/KCP tunnel. This checks signalling, datagram/stream framing
and malformed input handling; it is not a multiplayer game or public-host test.
The test executable alone defines `HALO_RELAY_TEST`, permits the local test broker
and offers `--echo-host`. The production executable has neither behavior and
ignores the test environment variable. `policy-test` checks production address
filtering separately.

## Parent-process protocol

All records on stdin and stdout are:

```
u32 little-endian length, u8 type, payload[length - 1]
```

Length is 1 through 65,536, counting the type byte. Reads may be fragmented.
There are no newline separators. Stdout contains only framed records.

Input types:

- `1`: exactly 44 ASCII hexadecimal bytes, sent once. This is the invite code,
  without `halo://join/`. Never put this secret in argv, URLs, logs or errors.
- `2`: one Web network packet, as defined in `port/web/src/web_shared.h`.

Output types:

- `2`: one Web network packet.
- `3`: UTF-8 JSON event, with `type` equal to `ready`, `peer` or `error`.

After receiving a valid invite, the worker emits:

```json
{"type":"ready","protocol":1,"identifier":"0123456789ab","address":1234567890}
```

`identifier` is the worker's newly generated six-byte native identity. `address`
is an unsigned 32-bit IPv4 value in the native network-byte-order representation,
derived from the identity in `100.64.0.0/10`. The browser must use this identity in
`XNADDR.abEnet` and this address as its local network address before game startup.
Numbers above illustrate the JSON shape and are not a real identity/address pair.

When the native host's authenticated tunnel connects:

```json
{"type":"peer","identifier":"fedcba987654","address":1234567891,"connected":true}
```

The browser installs that identity/address mapping for `XNetXnAddrToInAddr`.
No game packets may be submitted before this event. Loss of the host emits the
same event with `connected:false`, then an error, and closes the session.
Errors contain a fixed `code` without addresses, invite contents or exception text.
EOF exits the worker. The frontend must terminate the worker when its socket closes.

The 24-byte Web packet header contains `size`, `kind`, `source_ip`,
`destination_ip` (four little-endian uint32 values), source/destination port
(two network-byte-order uint16 values), and payload `length` (little-endian uint32).
The complete packet is padded to four bytes. Kinds: datagram=1, open=2, data=3,
close=4, refuse=5. Padding is not payload.

## Scope and limits

- Join-only: one native host per worker. Browser hosting and inbound TCP open
  requests are not implemented.
- UDP source/destination ports are limited to Halo system-link ports 5150/5151.
  UDP payloads are at most 1,395 bytes, matching the native tunnel's 1,400-byte
  inner frame minus its five-byte header. Broadcasts go only to the joined host.
- TCP streams must target native host port 5150. At most 16 stream tuples are
  open, payloads are at most 16,000 bytes, and each has a 128 KiB pending budget.
- Only the assigned source identity and the connected host destination are
  accepted. The public worker rejects private/reserved native peer candidates.
- Idle sessions expire after five minutes, unconnected invites after 95 seconds,
  and all sessions after four hours. Stalled output terminates after one second;
  reliable bytes are never silently dropped.
- One worker process per browser session isolates the native stack's global state.
  Its game sockets bind an assigned loopback address. Public P2P uses a separate
  ephemeral UDP socket; hosting infrastructure must allow UDP in both directions.
- Native invitations are never written to logs. The worker suppresses native
  logging because the native hosting path may print invitation secrets.

This is not a public service by itself. The frontend must enforce authentication,
allowed origins, per-client/global session limits and byte-rate/queue limits,
terminate failed workers, and expose WSS behind TLS. An invite grants access to
its host; it is not service authentication. GitHub Pages cannot run this worker.
