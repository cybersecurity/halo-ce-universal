# Browser match preservation

A quick-play host departure or transport outage must never call the new-match startup path. The
map, live player/object datums and game type stay loaded. The game thread pauses
the simulation while the browser elects a replacement, then reconnects the
original roster over newly established transports.

The design borrows the trading platform's separation of authoritative ordering
from observation delivery: one host assigns authority, checkpoints identify the
last applied sequence, and recovery validates identity before applying state.
The game implements these boundaries in-process; it does not need a separate
sequencer or snapshotter service.

## Ordering and checkpoints

The room election epoch identifies the current host generation; the engine's
game tick orders simulation state. An eight-byte epoch envelope around room
packets rejects messages from previous hosts, including reliable packets queued
behind native ring backpressure. Reattach and final resume handshakes also verify
session seed, epoch, original machine slot and its recorded virtual address.

Every 15 host ticks, reliable fragments carry a bounded, checksummed checkpoint.
A partial, corrupt or invalid typed snapshot never replaces the last complete
one. The checkpoint supplements the existing canonical replica with random
state, respawn/death timers, game-type authority state, machine roster and routing, damage cooldowns,
weapon firing/reload state and projectile state. Checkpoint metadata identifies
which surviving player can safely adopt authority. Idle or still-loading clients
cannot become a replacement host.

## Resume

The new server copies the same network roster and adopts the player's loaded
world instead of invoking normal server creation, map selection or countdown.
Survivors reconnect their existing machine/player slots; no player is added a
second time. Only transport/input queues are rebuilt. The replacement fences
the departed host, waits for the remaining machines to reattach, and broadcasts
a final resume acknowledgement at the retained authority tick. After 12 seconds,
reachable machines may resume while missing player slots and their recorded
owners remain eligible to reattach for another 120 seconds. Validated disconnects
also retain this identity during the original host's first, epoch-zero outage;
transport failure alone must not remove the roster before recovery begins. Clients stay paused
until that acknowledgement, rather than simulating ahead while the host waits.
Late joins update roster membership and slot ownership, including when they reuse
a departed machine's slot. Historical score datums remain on the scoreboard;
their old machine input ownership is removed.

If the original host is still present and is elected again, it renews its listener
and client transports around the existing authoritative world. It keeps its own
player and does not promote a replica or roll the world back. Reliable output
queues are bounded per peer so one blocked recipient cannot stop every other
player's keepalives. Queued reliable input is serviced before applying browser
connection timeouts. A timed-out reconnect retires its stale transport and retries
the same authority epoch; the former 45-second permanent recovery failure is
removed. Room epoch synchronization follows the loaded match's identity.

Authority announcements travel over both MQTT and each surviving RTC connection.
The direct control message is bounded, tied to the current peer identity and
shares a sequence gate with broker delivery, so delayed announcements cannot
replace newer authority state. Changes, connection opens and periodic beacons
repeat the announcement. During recovery, connected checkpoint owners of the
same match can acknowledge and finish an election even if one or all brokers
are offline. Initial room creation and an isolated survivor still require broker
discovery; new or lost RTC connections need signaling for their ICE exchange.
If a replacement disappears during reservation, recovery returns to candidate
selection in the same epoch while retaining the loaded world. Progress messages
distinguish room signaling, host selection and connection to a known replacement.

A generic replay of player inputs is not used: the distributed client prediction
path is not a deterministic copy of the authoritative simulation. Existing
replication reconciles the resumed world, while sequenced checkpoints recover
host-only state. Short checkpoint rollback and reconciliation are possible;
zero interruption or exact historic collision replay is not claimed.
A partitioned former host can reattach as its original player, using the recorded
owner address, within the same 120-second grace. It follows the new host's epoch
and cannot reclaim authority. The departed host does not delay the survivor cohort's
resume; a truly absent former player expires after the grace.

A client's native connection failure first repairs that player's stream to the
current host in the same epoch, including epoch zero. The original slot, score,
inventory and loaded world remain; other players keep running. A repaired stream
replaces a half-open connection only after session, epoch, slot and recorded-owner
validation. Admission and failed unvalidated attempts never invalidate a retained
roster slot. The client stays paused until its native acknowledgement arrives.
If the host disappears or its checkpoints stop advancing, bounded loss detection
still permits failover. A candidate's higher proposed epoch cannot interrupt a
healthy host or healthy clients. A launched, checkpointed replacement authority
still fences older hosts after a real takeover. If the current host returns before
a replacement commits, the isolated client retracts its proposal and reattaches.

## Verification

The tests exercise atomic checkpoint assembly, typed state validation and
restoration, actual manager adoption/reattach/resume helpers, epoch and ownership
rejection, current-roster retention, repeated handoff, and the production
quick-play state machine. Browser tests fence delayed packets and ensure
canceled sessions cannot receive recovery callbacks. A full WebAssembly build
is required alongside these tests. Live multiplayer evidence must separately
compare scores, clocks, player slots, positions and inventory across a host
close; passing these tests alone is not public deployment or sustained play.

The September 30 three-player Chrome check forced a 20-second host output outage
while keeping that host running, and held one client's output for 60 seconds.
All three resumed match 58499 with the original host at epoch 1 and retained
player slots and accumulated scores. Closing that host afterward preserved the
same match on both survivors at epoch 2. These are local injected-fault checks;
they do not establish arbitrary WAN partition or browser-eviction recovery.

A further three-player Chrome check disconnected player 3 from all signaling
brokers before closing player 1's host tab. Player 2 became host; both survivors
resumed match 26575 at epoch 1 while player 3 still had zero broker connections.
The replacement announcement was recorded over RTC. Original player slots and
accumulated scores remained in the same match. After signaling returned, a late
joiner entered that match; closing player 2 then resumed both survivors at epoch 2.
Regression checks also cover elections with all brokers unavailable, isolation,
foreign-match joiners and a replacement dropping during reservation. The public
package excludes the local fault injector and observe-mode launch arguments.

The same-epoch repair check resumed one client at epoch zero while the host and
third player kept playing and checkpoints kept advancing. It first reproduced
admission invalidating the retained machine; the corrected admission and pending
cleanup paths pass manager regressions whose original-code controls fail.
In the final three-player Chrome run, match 21315 survived a single-client repair,
then a native host failure: all three resumed at epoch one, including the former
host's original player. Closing the replacement host then resumed both remaining
players at epoch two, retaining original slots, accumulated scores and the loaded
world. This release uses network version 6; older loaded runtimes are not hot-patched.
