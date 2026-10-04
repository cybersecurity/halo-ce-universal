# Monocypher

A small cryptography library in C, by Loup Vaillant, Michael Savage and
Fabio Scotoni, dual-licensed CC0-1.0 or BSD 2-Clause (see `LICENCE.md`).

Upstream: https://monocypher.org (https://github.com/LoupVaillant/Monocypher),
release `4.0.3`, `monocypher-4.0.3.tar.gz`, SHA-256
`8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66`
(its sources match the repository's `4.0.3` tag but for their first line,
where the release writes its version).

Only `src/monocypher.c`, `src/monocypher.h`, `src/optional/monocypher-ed25519.c`,
`src/optional/monocypher-ed25519.h` and `LICENCE.md` are kept, unchanged.

Internet play (`port/linux/src/p2p_crypto.c`) uses its Ed25519 (with
SHA-512) and its conversion of Ed25519 public keys to X25519: a run's key
signs the listing of a public game in the server browser
(`port/linux/src/p2p_lobby.c`). It builds into the Linux, Windows and
Android platform layers.
