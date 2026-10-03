# Apple Silicon runtime foundation (draft)

This is a native arm64 macOS **memory probe**, not a playable port. It
establishes a small host-side memory contract before adding a game loader,
ABI bridges or graphics. It builds independently of the existing platforms
and needs only Python 3 and Xcode Command Line Tools, with no downloaded
dependencies or game assets.

From the repository root:

```sh
python3 tools/macos_memory_probe.py
```

The script builds `build/macos-runtime/memory_probe` with warnings as errors
and executes it. It rejects other platforms and Python running under Rosetta.
It does not change the default Ninja target or claim a `ninja macos` game build.

## Memory contract

The Android implementation is the reference for the game's 32-bit address
layout (`port/android/include/halo_android_abi.h`). The Xbox window starts at
`0x80000000`, spans 128 MiB, and the Android guest image starts at `0x88000000`.
The probe imports those constants rather than maintaining a second copy.

On macOS, this experiment leaves low process addresses alone. Mach allocates
a separate 4 GiB-aligned, 4 GiB virtual region without replacing any existing
mapping. Guest addresses become offsets into that region:

```text
host pointer = region base + unsigned 32-bit Xbox address
```

The reservation starts inaccessible and is backed on demand when pages are
made writable and touched; reserving it does not touch 4 GiB of physical RAM.
Guest address zero represents NULL. Translation rejects empty or overflowing
ranges and pointers outside the reservation. It checks address bounds, not
whether pages have been made readable. Reverse translation must be used
before storing a host pointer in a 32-bit game field.

Protection calls require exact host-page boundaries and allow only read/write
permissions. They deliberately reject subpage requests rather than silently
altering protection of neighbouring Xbox blocks. The runtime has no executable
memory loader. A memory instance has one owner and must outlive every user.

## Validation

Locally verified on Apple Silicon, macOS 27.0.1, Apple clang 21.0.0:

- Native arm64 host, 8-byte pointers and 16,384-byte host pages.
- Two distinct simultaneous 4 GiB-aligned reservations.
- Access and round-trip translation at the existing Xbox window offset.
- Guest NULL, empty ranges, 32-bit boundary and 64-bit size overflow rejection.
- Read/write, read-only and inaccessible transitions, queried through Mach.
- Adjacent pages and the guest-image address remain inaccessible.
- Misaligned and executable protection requests rejected.
- Reservation destruction, cleared state and recreation.

These are host-runtime checks. No game execution, guest ABI interoperability,
save compatibility, renderer, multiplayer or older macOS version was tested.

## Remaining decisions

1. Select the guest execution strategy. Android's ILP32 instructions cannot
   simply dereference these rebased addresses: every relevant memory access
   and indirect code pointer needs translation. Alternatively, an LP64 source
   conversion must retain explicit 32-bit fields in Xbox data structures.
2. Implement and validate the image loader and guest/host calls, including
   function pointers, stack addresses, TLS, varargs and callback lifetimes.
3. Define 4 KiB guest-block bookkeeping on 16 KiB host pages. The current
   strict protection API does not implement allocator or texture write tracking.
4. Add the platform services and graphics backend, then test actual game data.

## Related work

This branch starts from `cybersecurity/halo-ce-universal/main` and does not
include the browser PR's commits. Relevant existing work includes:

- [#12: browser runtime](https://github.com/cybersecurity/halo-ce-universal/pull/12).
- [#16: experimental Apple Silicon target](https://github.com/cybersecurity/halo-ce-universal/pull/16).
- [#20: ILP32 macOS port with address rebasing](https://github.com/cybersecurity/halo-ce-universal/pull/20).
- [#22: macOS port with 64-bit source conversion](https://github.com/cybersecurity/halo-ce-universal/pull/22).

This draft is a small independently runnable foundation for discussion alongside
those ports, not a claim to supersede their loaders or working game builds.
