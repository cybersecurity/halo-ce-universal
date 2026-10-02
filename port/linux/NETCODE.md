# Distributed netcode

The Xbox game plays system link in lockstep: clients send their input to
the host, the host sends every machine every player's input for each 30 Hz
tick, and every machine simulates the whole game from them, waiting for
each tick's update. A client therefore sees its own movement and shots a
full round trip late, and any machine whose simulation differs in the last
bit goes out of sync.

The native builds replace that (they no longer have the lockstep netcode)
with the model of later Halo engines (the "distributed" simulation of the
MonkeyNuts/Ares source)
with ideas from VALORANT's netcode articles, keeping the 30 Hz tick:

- **Every machine ticks on its own clock.** Nobody waits for anybody: a
  client no longer runs only the ticks the host has sent. A client's clock
  starts with the host's first game update and takes the host's time from
  it (at a game's start as at a late join), so every machine reads the
  game's timers alike.
- **Own player predicted.** A client drives its own player (and the
  vehicle it drives) from its local input at once. Remote players are
  driven by the inputs the host relays every tick, the latest one held
  until a newer arrives.
- **Host authoritative.** The host alone decides damage, deaths, spawns,
  pickups, scores and the game's objects; clients do not decide them but
  apply what the host sends.
- **Corrections.** The host sends each client the authoritative state of
  the players' units and the game's moving objects; a client moves its
  copies toward it, a small error half of the way each tick, a larger one
  at once, drawn gliding from where they were. A client's own unit and
  vehicle are only corrected past a tolerance, so prediction does not
  rubber-band: the host tells the client which of the client's ticks it
  has its player at (its prediction come back), and the client compares
  that with where it had the player at that tick, not now, and moves the
  player by the difference, keeping what they did since. The host takes a
  client's player where the client says within a tolerance of its own,
  and no further from an anchor (one it took, a newer once a second) than
  a player moves in the client's ticks since, those no more than its own
  since and a little jitter (so the tolerance and the jitter are not gained
  again each tick); a player on foot moves across and up (and the host
  takes their velocity) no faster than twice as fast as they run and jump,
  or as fast as the host's own ticks sent its copy of them in the last few
  seconds or the flight they began then (an explosion's throw, which the
  client learns of a round trip late), whichever is more; down no faster
  than that and a fall from the highest the host has had them since they
  were on the ground; no more than 2 world units a tick, with a twentieth
  of a world unit a tick more for where they are; and no higher above
  where the host last had them on the ground than a jump takes them (as
  fast up as a jump or a run, the legs drawn up, and a world unit more)
  and a throw its ticks gave them, and in the air no higher than a thing
  thrown up at that speed falls to since (the ticks since the host's copy
  left the ground, less the client's round trip, half a second at most,
  and jitter): past it, the
  host's copy falls as its own ticks have it, so no one hovers, walks on
  air or comes down slowly. What the host's ticks sent its copy is
  its speed less as much as the velocity it took of the client was faster
  than the player goes of their own (and up, what its tick added but a
  jump's), so a client that says it goes faster (a copy said to hover and
  fall, gaining the host's gravity each tick) gains nothing by it. A
  teleporter, which moves the host's own copy too, starts afresh.
- **Shooter's hits.** A client reports what its own players hit; the host
  checks the report (the player's, a weapon they carry, fired from within
  its reach, the target where the host had it when the shooter saw it, no
  faster than weapons fire) and deals the damage. What the shooter saw
  hit, hits.

A host's game advertisement says that it plays this netcode. A client does
not join a host that does not (one of network version 4 built before the
lockstep netcode was removed, set to play it): it tells the player why.

## Versions

The native builds' network code has a version, an unsigned 16-bit number
(`HALO_PORT_NETWORK_VERSION` in `port/linux/include/halo_port_limits.h`),
raised with any change to what the machines send each other. A host puts it
in its game's advertisement (reserved bytes that hosts built before there
was a version send as zeros, so they are version 0). A client does not join
a host of another version: it shows a message box that says which of the
two is newer, with both versions ("update the game" or "ask the host to
update"), and stays in the list of games. Version 1 was the first of this
netcode; version 2 lets a machine join a game in progress; version 3 puts
each player in its slot of the host's player list on every machine;
version 4 plays the European (PAL) maps as the North American ones
(`port/linux/game/pal_tags.c`); version 5 corrects a client's own player
against where it had them at the tick the host has them at, and checks
what the machines send each other more closely; version 6 sends a unit's
state and a player's input without the parts that are their defaults, fills
each datagram, sends damage and pickups only to the machines they concern,
stamps a hit with the host's tick the client had heard of, and checks hits
and a client's own player's moves more closely; version 7 sends with a
killing blow its killer's score after it, and with a body's state that it
is dead; version 8 is the first whose clients play by the host's rules
(below), so a build without them joins no host of it; version 9 tells
every machine of a player the host dropped for cheating, each client
tells the host its Discord user, and a machine's join request carries its
hardware id; version 10 sends every player's ping for the scoreboard;
version 11 adds browser match-preserving host migration, typed checkpoints,
and original-roster reattachment.

A client plays by its host's rules: in another's game (searching for it,
in its lobby, or playing it) the developer console, the telnet console
and the cheat buttons run only commands that change nothing of the game
(what the machine shows and how its controls feel: `hs_compile_and_evaluate`),
and each frame and tick the game's cheats, speed, autoaim, magnetism and
rider ejection are put back to the host's, and what it draws of the world
to what everyone's draws (no wireframe, debug drawing mode, environment
left out, fog, grass or water off), as set before joining too
(`cheats_network_client_enforce`); its camera stays the player's own
(no flying or following camera). The host keeps its own.

The host also finds a client whose game runs faster than time (a speed
hack, which speeds up the machine's own clock, so that nothing on it can
tell): every two seconds it measures how fast the ticks the client's
messages are stamped with go by against its own clock's time, and how far
ahead of its own time they are. A client's clock starts at the host's and
only ever jumps forward to it when behind, so an honest one is never ahead
while going faster (one that caught up, or a host that stalled, is one or
the other, not both). One more than a tenth faster and half a second ahead
has its players' predictions refused at once (the host's copies go as its
own ticks have them), and after ten seconds of it is dropped, its address
kept out of the host's games while the host runs, and every machine is
told who, in red on its console and in its `debug.txt`
(`distributed_note_client_clock`, `network_game_server_kick_machine`,
`_distributed_message_notice`). The host also adds a line to
`cheaters.txt` beside its `debug.txt`: when, the player's address (an
internet play peer's real one), their Discord user and their players'
names, and why. A client tells the host its Discord user as the Discord
client signed in on its machine says (its id and name, none without one:
not running, or internet play off), once it is in the game and again
when it changes; it says what it likes, so the host keeps of it only digits
in the id and letters, digits, "_", "." and "-" in the name, 23 and 39 of
them at most (`p2p_discord_sanitize`), and names it as the player said.
A joining machine tells the host its hardware id: a keyed hash (HMAC-SHA-256,
16 bytes as hex) of what its machine is known by (Windows' SMBIOS UUID, else
its MachineGuid; Linux's `/etc/machine-id`; Android's `ANDROID_ID`, which the
launcher writes to `hardware_id.txt`: `p2p_hardware_id`), kept by the host
as hex only. A player dropped for cheating, and one the host bans with the
console's `ban <player name>` (Tab completes the name; the host's alone), is
added to `bans.txt` beside `debug.txt` (a line each, as in `cheaters.txt`,
with `ip=` and `hwid=`): the host refuses a machine joining whose address or
hardware id is in it (a line taken out unbans). Both are as the player's
machine tells them: anyone with administrator or root access can change
them, and players behind one address share it.
A speed hack of less than a tenth is let be: the host's bounds on how far
and how fast a client's player moves and fires hold it to the host's time
anyway.

## Joining a game in progress

A game stays open when it starts (on the Xbox it closed, since every
machine had to simulate it from its first tick), and the game list shows
it. A machine that joins it is accepted as in the
pregame, and its players are added as the game adds a player in game:
every machine in the game spawns them, told by the game's own
`_message_server_add_player_ingame`. Then the host sends that machine alone
the game's settings and its start, with the host's game time (the start
message carries 16 bits of it); the machine loads the game, sets its clock
to that time and the ticks it spent loading, and takes the rest of the time
from the first game update if it is ahead (so that the game's timers read
as the host's). It takes up the host's count of updates where it is. When it has loaded, the distributed netcode
gives it the host's objects (network_objects.c), every player's statistics
and the game type's state, and it plays on as any other client.

The netcode names a player by its datum's index, which must be the same on
every machine, the one that joined too. That machine has not the players
who left (their datums stay until the game ends), nor the order in which
the others added players. So each player's datum is
its slot in the host's player list: every machine makes it there, and the
host gives a player added to the game in progress a slot whose datum is
free (`network_game_manager.c`). A player added to the game in progress
also gets its team and the game type's data, as the players at the start
do (in free for all, a team of its own).

The game is closed to joins while its machines load and after it ends.
A machine must join within 10 seconds of connecting; in a game, the host
drops a machine it has heard nothing from for 15 seconds, and one that
joined the game in progress and has not loaded in two minutes.

Until it has loaded, the machine hears none of the game's messages (which a
machine in the pregame refuses, and which the others no longer need), only
a pregame keep-alive every five seconds from the host
(`network_server_manager.c`, `network_server_message_handler.c`).

## Stages

1. (Done) Decoupled ticks: clients tick on their own clock with local input
   for local players and the latest relayed input for remote ones; the host
   no longer waits for clients; taps are accumulated so a quick button
   press is never lost; out-of-sync checks off. The lockstep netcode is
   gone: the host's per-tick game update carries no actions, and only
   keeps the clients' count of its ticks.
2. (Done) Authority: clients skip damage, deaths, spawns, pickups, item
   spawns, and scoring, and apply the host's state for them
   (`port/linux/game/network_distributed.c`):
   - every tick, every player's unit: which it is, alive or not, the seat
     it rides, shields and health (down, recharging, the damage they show),
     its powerups (camouflage and how long each has left), where it is (a
     client's own player's position is its own, within a tolerance, and the
     host takes it), and when dead who killed it;
   - what a client's players pick up, which the host decides: the client
     shows it (the HUD's message, the sound, a powerup's screen flash);
     only that client is told;
   - every two seconds, each player's ping (unreliable): the round trip
     of its machine's messages to the host and back, as the host smooths
     it, 0 for the host's own players, for the scoreboard;
   - twice a second and with every kill, the players' statistics that
     changed (once a second a few more, round them all, as the message is
     unreliable); when it changes (looked at five times a second, sent at
     most every other look unless the game ended; reliably, so never
     again unchanged), the game type's state (scores, the flags, the balls
     and their carriers, the king's hill, the players' speeds) and whether
     the game is over. A machine that has loaded is sent all of both at
     once. It counts the game type's events (captures, grabs and returns
     of the flags, laps, the balls reset, a ball passed to its carrier's
     killer), and a client announces those it has not had,
     as the host does its own; a client runs the rest of the game type's
     update that only shows the game (waypoints, messages, sounds, a
     carrier's speed).

   The messages are a kind of their own (the game's unused "data" message
   type), unreliable per tick, reliable for what must not be lost.
3. (Done) Object identity (`port/linux/game/network_objects.c`): the
   game's units, vehicles, weapons and equipment are the host's, at the same
   datum index (identifier and all) on every machine, so a message names
   one by its index.
   - The host tells its clients (reliably) of each such object it makes
     (what it is, where, how it looks) and each it deletes; clients make
     and delete theirs to match. A client that has loaded asks for all the
     host's objects and is told when it has them; from then on it deletes
     any such object the host has not told it of, and nothing but the
     host's word deletes the host's.
   - The objects placed when the map loads are placed alike everywhere:
     the host's word finds a client's already there. Past loading, a
     client's own objects (projectiles, effects: what only it sees) take
     indices from the upper half of the object array, clear of the host's.
   - Ten times a second, what every unit carries (the host's weapons, slot
     for slot, their ammunition, the weapon in hand, the grenades); a
     client moves the same weapon objects in and out of its units. A
     change of weapons or grenades goes to every client at once; one of
     ammunition only to the unit's player's machine at once, and to the
     others as often as they are sent that player. A client takes its own
     players' ammunition and grenade counts from the host only once a
     round trip has passed since it last fired, reloaded or threw (what
     the host says before then is from before). How old what a unit
     carries is travels in 16 bits.
   - Players take the units the host spawns them with, seats are the
     host's (a client's own player's once it has ridden otherwise for
     longer than a round trip), and the CTF flags and oddballs are the same
     objects everywhere.
4. (Done) Corrections: the host sends each client where its moving objects
   are (vehicles, items, bodies) as often as they are near that client's
   nearest player (every tick within 25 world units, every second within
   60, every third within 120, every fourth further off), once more to
   every client as an object comes to rest, and a few of those at rest,
   round them all. A client puts its copies there, and the difference is
   drawn fading over a few ticks (`render_interpolation.c`) instead of a
   jump. A client drives its own player's vehicle and sends where it is,
   which the host takes within a tolerance, as it does its own player's
   unit, and no further from an anchor (one it took, a newer once a
   second) than the vehicle moves in the client's ticks since, those no
   more than its own since and a little jitter (twice the tag's top speed,
   or as fast as the host's own ticks sent its copy in the last few
   seconds beyond the velocity it took of the client, whichever is more,
   no more than 3 world units a tick, and a tenth more a tick, with the
   blend distance; its velocity no faster, without the tenth, so that the
   client's word does not raise it, so a vehicle falls no faster than
   that and a tick's gravity: a long fall's last moves are the host's; a
   teleporter falls back to the tolerance); a vehicle that does not fly,
   float or stay (not a Banshee, a boat or a turret), in the air, no
   higher above where it left the ground than a thing thrown up as fast
   as it went up then, and a tenth of a world unit a tick, falls to since
   (less the client's round trip, half a second at most, and jitter), with
   2 world units more (as a player on foot); a copy at rest that the
   client's word moves wakes, so that it falls and counts its time in the
   air. The host sends the client its own
   vehicle every third tick, with the client's tick it took the vehicle at
   (`_distributed_object_predicted_bit` and a 16-bit time in the object's
   state); the client compares that with where it had the vehicle at that
   tick, and past 4 world units moves it by the difference, as it does
   its own player's unit.
5. (Done) Hits (`port/linux/game/network_damage.c`):
   - A client deals no damage. What its own players' shots, grenades,
     melee and vehicles hit, it reports to the host (reliably), with the
     host's latest tick it had heard of when it made the report.
   - The host deals a report once it has checked it: from that machine's
     player; damage one of their weapons (a vehicle's a driver's or
     gunner's; now or in the last ten seconds), their grenades (while they
     have them, and for a while after) or the vehicle they drove (in the
     last ten seconds: its collisions) can deal (its projectiles' impacts
     and detonations, followed through the tags), no harder than it can be
     (all of it, but an airborne melee blow's half again); of the shape the
     game gives that damage: what hits at a point (a projectile's impact,
     or its detonation on what it sticks to, and an explosion) there, its
     origin its epicenter; a melee blow from the striker's head (its
     origin) and body (its epicenter), both within a few world units of
     where the host had the player; a collision from the vehicle (its
     epicenter) within a few world units and the vehicle's size of where
     the host had the vehicle the player drove, the target (where the
     client had it) no further from it than their sizes and the push the
     vehicle gives it; the origin at the target in each (an explosion's and
     a melee blow's within its reach); what hits at a point within its
     reach of where the host had the player (their unit, or the vehicle it
     rode, and its size) since it could have been fired: the projectile's
     range, or its speed for as long as its timer runs, what it sets off
     going off from there, with 6 world units more (the host keeps thirteen
     seconds of where each player was, every third tick, as a rocket flies
     and a grenade outlives its thrower, and looks no further back than
     the client's round trip and a second before the report came; not checked for what the tags do
     not bound: a projectile that sticks, which what it sticks to carries,
     one whose timer starts once it bounces or rests, a weapon's own
     detonation; no line of sight); the target within a few world units of
     where the host had it at the tick the report was made at (a player's
     unit or vehicle: the host keeps a second of where they were) or of
     where it is (more for a fast one; also for one that is no longer a
     player's, a body or a vehicle left);
     and no more reports than the weapon that deals them fires (its rate of
     fire and projectiles a shot, with a margin; an explosion's hits count
     as one, when its damage has a reach, and no object is hit twice by
     one, the explosions of each player's reports told apart on their
     own). A report made more than three seconds ago (a burst of them held
     back while the network was out) is refused. A report is paid for
     before the host looks through its history, so a flood of them costs
     the sender its hits. The damage is dealt as a client's own hit can
     be: only the flags such a hit has, area damage as the game deals that
     damage (the report's only for damage dealt both ways), an explosion's
     direction from its epicenter to the target's centre (where the client
     had it) and its scale no more than its fall off over that distance
     gives (unless it does not fall off), any other's direction one long,
     and the host's multiplier and team, not the report's, its owner the
     player's unit (or the vehicle it rides, or a unit of theirs); a
     report with a number that is not finite, or a node, region or
     material the target does not have, is refused. Its own copies of a
     client's projectiles deal nothing (the report does), but once that
     client has left the game they deal what they hit, as its own do (a
     hit the client reported just before it left may so be dealt twice).
   - The host sends its clients the damage it dealt to units, and a client
     replays what it does besides the harm (which the units' states
     bring): the player's screen flash and shake, the unit's flinch, pain
     sound, knockback and stun, the scope it knocks the player out of, and
     who the HUD shows hit them. A killing blow it replays whole, so the
     body falls as the shot had it and the kill is announced with the
     host's killer (and the killer's score after it, as the host's game
     type has it: the game type's state, which brings the scores, may come
     after the blow), and a telefrag's message (a client does not decide a
     telefrag itself: it sees who blocks a teleporter a latency late); an actor's (a biped no player's, alive until then)
     too, counted by no one there (the host's statistics come as they
     are). A killing blow goes to every client; other damage to
     the machines of the unit's player, its riders and the damage's owner,
     and of the clients sent that player this tick (who can see them); a
     player's screen effects to that player's machine alone, but for a
     weapon's own shake of the player firing it (no one's damage), which
     that player's machine shows itself at once. The killing blow is sent
     unreliably: an actor's body the host says is dead (the objects' states
     say so) that is still alive half a second on is killed with nothing
     to show (a player's the units' states kill).
   - A client's own projectiles respond to what they hit as the game has
     them: the host's shields and health, which the client has, say
     whether the shield or the body took the hit, and how much is left of
     it (the host's own copies of a client's projectiles likewise).

## Transport

What reaches the other machines, and how, decides how the game feels over
a real network as much as the model does (compared with Quake III, Source,
Unity's Netcode for Entities, lightyear, netfox and the Ares source):

- **Nothing held back.** The game's connections (the reliable messages:
  objects made and deleted, the game type's state, hits, pickups) send each
  write at once (`TCP_NODELAY`, in `xnet.c` for the game's sockets and in
  `p2p.c` for internet play's): with Nagle's algorithm a small write waited
  for the other end's delayed acknowledgement, up to 200 ms on Windows.
- **Input every tick, unreliably, each tick's buttons several times.** A
  client sends the host its players' input after each tick, with the
  buttons of the three ticks before it, and the host sends every client
  every player's input as its tick ran it, the same way. Each tick's
  buttons are taken once, from whichever message brings them first
  (`player_queues_new.c`), so a press is lost only with four datagrams lost
  in a row, and nothing waits for a lost one to be sent again (the game's
  own per-tick update, reliable and so held up by any loss, now carries no
  input). The host's clock still reaches the clients in it, and the game's
  own client update, whose input the host no longer takes, goes ten times
  a second instead of sixty; the host takes its own players' input at
  each of its ticks.
- **As few datagrams a tick as they fill.** The unreliable messages of a
  tick to a machine go together (`_distributed_message_batch`), saving each
  one's headers (internet play's tunnel adds 36 bytes to every datagram: 15
  of its header, 16 of its tag and 5 of its own inside), each message split
  between two where one is full, so none goes out part empty; the machines'
  datagrams go in a turn that starts one further each tick.
- **Stamped with their tick.** Every message carries the sender's tick
  (its header's game time); an unreliable one (the units, the input, the
  objects, the statistics, what units carry, the damage) that arrives after
  a newer of its kind is dropped, so a late datagram never puts anything
  back.
- **Taken at the tick.** The host takes a client's players' and vehicles'
  positions (the latest of each) at its next tick, not as each arrives.
- **Fewer bytes.** Vectors travel in 16 bits a part, shields and health in
  16 bits; a unit's state goes without the parts that are their defaults
  (the vehicle and seat of one that rides nothing, the position of one that
  rides, its up when straight up, the damage its shields and body show,
  powerups and camouflage when it has none, the tick its own client
  predicted it at to any other client), 35 bytes for a player on foot
  instead of 64; a player's input goes with its tick's update once a
  message, and the buttons of the three ticks before only where they
  differ from the tick after (14 to 20 bytes instead of 24); what a unit
  carries is sent when it changes (and once a second); the moving objects
  as often as they are near the client's players, the objects at rest
  round all of them, four a tick; the players' statistics when
  they change (with every kill, twice a second), with sixteen more
  players' once a second round them all; the game type's state when it
  changes, at most two and a half times a second.
- **The players each client needs, when it needs them.** The host sends a
  client every player's unit and input every tick when they are within 25
  world units of the client's own players, every second tick within 60,
  every third within 120, every fourth further off, and every sixth when no
  cluster of the client's players' can see theirs (the map's potentially
  visible set, which errs on the side of seeing: Halo's are coarse, and
  most of a map sees most of it) or they are dead. A client whose players
  are dead is sent the others as from where its players last were alive.
  None is ever left out, but a player who has left the game, whose unit
  goes only when it changes, and a dead player's input, which drives
  nothing. Whatever changes what a client sees goes at once:
  - a player's death, spawn or seat, and a player coming into sight;
  - a player's input every tick while their buttons or weapon choices
    change (the ticks it carries), so no jump, grenade or melee of theirs
    is missed, even out of sight;
  - a player a client's player aims at within 35 degrees at least every
    second tick, and within 20 degrees through a scope every tick, so a
    sniper sees a far player move as smoothly as a near one (as Ares
    raises the priority of what a player zooms onto).

  A client's own players' units go to it every tick, their input never (it
  has its own).
- **The round trip.** A client's input messages tell the host the latest
  host tick the client has had, which gives the host each client's round
  trip (smoothed as TCP smooths its own, once a message). The host keeps a
  second (32 ticks) of where players' units and vehicles were, and checks a
  client's hit against where the target was from the host tick the client
  had last heard of when it made the report (three ticks more), instead of
  against where it is now with a wide margin: however long the report
  took to come (the reliable channel's resending holds it up), that is
  when the shooter saw the target (as far back as the host keeps). A
  report made more than three seconds ago is refused.

## Testing

`debug.network_test` (`port/linux/game/network_test.c`) hosts or joins a
game without the menus (in a team game the joining player takes the other
team), and `debug.test_input` plays controller 1 with a scripted bot (or,
`look:<seed>`, one that stands still, only turning and looking up and
down); each machine logs every player's position, health and shields,
where they aim and face, their animation state and how hard they move,
weapons, grenades, score, kills and deaths every second, with the objects
made and removed and the hits reported, dealt, rejected and replayed, so
two machines' views of one game can be compared. `debug.network_test_kill`,
`debug.network_test_shoot`, `debug.network_test_vehicle` and
`debug.network_test_pickup` script kills, hits, a vehicle ride and a weapon
swap the bots' wandering does not reach (`debug.network_test_pickup_weapon`
picks the weapon: the first whose tag name has it in it, as "sniper"), and `debug.network_test_score`
shortens the game, to test the next (`host:<map>:<variant>,<variant>...`
plays the variants in turn, the next once a game is over, as the host's
button on the scores does). `debug.network_latency` and
`debug.network_loss` hold back what a machine receives and drop some of its
datagrams, to test as over the internet.

The host logs to `debug.txt` when a player on another machine presses the
action button where the host has nothing for them to pick up, with where it
has them and the nearest item: a client that sees a pickup the host does
not.
