import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { network } from './tests/web-quick-play-fixture.cjs';
const { HaloQuickCoordinator: Coordinator } = createRequire(import.meta.url)('../port/web/site/net.js');

const A = '1111111111111111', B = '2222222222222222', C = '0000000000000000';
const ADDRESS_A = 0x0101010a, ADDRESS_B = 0x0201010a, ADDRESS_C = 0x0301010a;
const peer = (coordinator, open = true) => ({ id: coordinator.id, address: coordinator.address, open,
  quick: { ...coordinator.presence } });
const checkpoint = (coordinator, tick = 90, matchId = 500) => coordinator.checkpoint(coordinator.epoch, tick, matchId);

function runTogether(coordinators, end = 10000) {
  const results = new Map();
  for (let now = 0; now <= end; now += 100) {
    const snapshot = coordinators.map(coordinator => peer(coordinator));
    for (const coordinator of coordinators) {
      const status = coordinator.tick(now, snapshot.filter(p => p.id !== coordinator.id), true);
      if (status.result) results.set(coordinator.id, status.result);
    }
  }
  return results;
}

test('idle and download-only room peers are never elected', () => {
  const active = new Coordinator(B, ADDRESS_B, 0);
  const idle = { id: C, address: ADDRESS_C, open: true, quick: null };
  assert.equal(active.tick(0, [idle], true).result, undefined);
  assert.equal(active.tick(5999, [idle], true).result, undefined);
  assert.equal(active.tick(6000, [idle], true).result, undefined); // host reservation is not yet committed
  assert.equal(active.tick(7499, [idle], true).result, undefined);
  assert.deepEqual(active.tick(7500, [idle], true).result,
    { role: 'host', hostId: B, hostAddress: ADDRESS_B });
});

test('simultaneous connected quick entrants agree on one host and its actual address', () => {
  const a = new Coordinator(A, ADDRESS_A, 0), b = new Coordinator(B, ADDRESS_B, 0);
  const results = runTogether([b, a]); // iteration order must not decide the host
  assert.equal(results.size, 2);
  assert.deepEqual(results.get(A), { role: 'host', hostId: A, hostAddress: ADDRESS_A });
  assert.deepEqual(results.get(B), { role: 'join', hostId: A, hostAddress: ADDRESS_A });
});

test('host waits for connected candidates to acknowledge its reservation', () => {
  const a = new Coordinator(A, ADDRESS_A, 0), b = new Coordinator(B, ADDRESS_B, 0);
  a.tick(0, [peer(b)], true);
  a.tick(6000, [peer(b)], true);
  assert.equal(a.presence.role, 'candidate'); // B has not observed an election yet
  b.tick(6100, [peer(a)], true);
  a.tick(6200, [peer(b)], true);
  assert.equal(a.presence.role, 'host');
  assert.equal(a.tick(7699, [peer(b)], true).result, undefined);
  assert.equal(a.tick(7700, [peer(b)], true).result.role, 'host');
});

test('a running host survives a later participant with a smaller ID', () => {
  const host = new Coordinator(B, ADDRESS_B, 0);
  runTogether([host]);
  host.launched('playing');
  const newcomer = new Coordinator(C, ADDRESS_C, 12000);
  newcomer.tick(12000, [peer(host)], true);
  const result = newcomer.tick(13500, [peer(host)], true).result;
  assert.deepEqual(result, { role: 'join', hostId: B, hostAddress: ADDRESS_B });
  assert.equal(host.tick(13500, [peer(newcomer)], true).result.role, 'host');
});

test('known active but unreachable peers time out instead of making a second host', () => {
  const a = new Coordinator(A, ADDRESS_A, 0), b = new Coordinator(B, ADDRESS_B, 0);
  for (let now = 0; now < 35000; now += 1000) {
    assert.equal(a.tick(now, [peer(b, false)], true).result, undefined);
    assert.equal(a.presence.role, 'candidate');
  }
  assert.match(a.tick(35000, [peer(b, false)], true).error, /reachable host/);
});

test('an existing host must be connected before quick join resolves', () => {
  const host = new Coordinator(A, ADDRESS_A, 0);
  runTogether([host]);
  const joiner = new Coordinator(B, ADDRESS_B, 10000);
  assert.equal(joiner.tick(10000, [peer(host, false)], true).result, undefined);
  assert.equal(joiner.tick(20000, [peer(host, false)], true).result, undefined);
  assert.equal(joiner.tick(21000, [peer(host)], true).result, undefined);
  assert.equal(joiner.tick(22500, [peer(host)], true).result.role, 'join');
});

test('a missing host reported by an active joiner is not replaced by another game', () => {
  const a = new Coordinator(A, ADDRESS_A, 0);
  const joined = { id: B, address: ADDRESS_B, open: true,
    quick: { role: 'join', hostId: C, phase: 'launched', gamePhase: 'playing' } };
  assert.equal(a.tick(0, [joined], true).result, undefined);
  assert.equal(a.tick(10000, [joined], true).result, undefined);
  assert.equal(a.presence.role, 'candidate');
});

test('new hosting never proceeds without a working signalling broker', () => {
  const a = new Coordinator(A, ADDRESS_A, 0);
  a.tick(0, [], false);
  assert.equal(a.tick(10000, [], false).result, undefined);
  a.tick(11000, [], true);
  assert.equal(a.tick(16000, [], true).result, undefined);
  a.tick(17000, [], true);
  assert.equal(a.tick(18500, [], true).result.role, 'host');
});

test('a host disappearing during reservation produces a retryable error', () => {
  const host = new Coordinator(A, ADDRESS_A, 0);
  runTogether([host]);
  const joiner = new Coordinator(B, ADDRESS_B, 10000);
  joiner.tick(10000, [peer(host)], true);
  assert.match(joiner.tick(10500, [], true).error, /host left/);
});

test('surviving players elect one replacement and converge after the old host returns', () => {
  const host = new Coordinator(A, ADDRESS_A, 0);
  const b = new Coordinator(B, ADDRESS_B, 0), d = new Coordinator('3333333333333333', ADDRESS_C, 0);
  runTogether([host, b, d]);
  for (const coordinator of [host, b, d]) { coordinator.launched('playing'); checkpoint(coordinator); }
  for (let now = 11000; now <= 31000; now += 100) {
    const snapshot = [b, d].map(coordinator => peer(coordinator));
    for (const coordinator of [b, d]) coordinator.tick(now, snapshot.filter(p => p.id !== coordinator.id), true);
  }
  assert.equal(b.epoch, 1); assert.equal(d.epoch, 1);
  assert.equal(b.result.role, 'host'); assert.equal(d.result.role, 'join');
  assert.equal(d.result.hostAddress, ADDRESS_B);
  b.launched('playing'); d.launched('playing');
  assert.equal(host.tick(32000, [peer(b), peer(d)], true).result, undefined);
  assert.equal(host.tick(33500, [peer(b), peer(d)], true).result.hostId, B);
  assert.equal(host.result.role, 'join', 'a returning former host yields to the newer room epoch');
});

test('a brief lost host connection recovers without restarting the match', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), joiner = new Coordinator(B, ADDRESS_B, 0);
  runTogether([host, joiner]); host.launched('playing'); joiner.launched('playing');
  assert.equal(joiner.tick(11000, [], true).state, 'reconnecting');
  assert.equal(joiner.tick(20999, [], true).result, undefined);
  assert.equal(joiner.tick(21000, [peer(host)], true).result.hostId, A);
  assert.equal(joiner.epoch, 0); assert.equal(joiner.recovering, false);
});

test('an older match in the same public room cannot migrate a healthy loaded cohort', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), joiner = new Coordinator(B, ADDRESS_B, 0);
  runTogether([host, joiner]);
  for (const coordinator of [host, joiner]) { coordinator.launched('playing'); checkpoint(coordinator); }
  const unrelated = new Coordinator(C, ADDRESS_C, 0);
  unrelated.epoch = unrelated.presence.epoch = 7;
  unrelated.launched('playing'); checkpoint(unrelated, 500, 999);
  for (const coordinator of [host, joiner]) {
    const other = coordinator === host ? joiner : host;
    const status = coordinator.tick(11000, [peer(other), peer(unrelated)], true);
    assert.equal(coordinator.epoch, 0, 'epochs belong to the preserved match');
    assert.equal(coordinator.recovering, false);
    assert.equal(status.result.hostId, A);
  }
});

test('one client election cannot interrupt a healthy host or another healthy client', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), b = new Coordinator(B, ADDRESS_B, 0);
  const isolated = new Coordinator('3333333333333333', ADDRESS_C, 0);
  runTogether([host, b, isolated]);
  for (const c of [host, b, isolated]) { c.launched('playing'); checkpoint(c, 125); }
  b.recover(11000);
  for (const c of [host, isolated]) {
    const other = c === host ? isolated : host;
    const status = c.tick(11000, [peer(other), peer(b)], true);
    assert.equal(c.epoch, 0); assert.equal(c.recovering, false);
    assert.equal(status.result.hostId, A, 'the running authority remains in place');
  }
});

test('a suspected host returning before replacement commitment repairs only the isolated client', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), client = new Coordinator(B, ADDRESS_B, 0);
  runTogether([host, client]);
  for (const c of [host, client]) { c.launched('playing'); checkpoint(c); }
  client.tick(11000, [], true); client.tick(21000, [], true);
  assert.equal(client.epoch, 1); assert.equal(client.presence.role, 'candidate');
  const result = client.tick(22000, [peer(host)], true);
  assert.equal(result.reconnect, true); assert.equal(result.result.hostId, A);
  assert.equal(client.epoch, 0); assert.equal(client.presence.matchId, 500);
  assert.equal(host.tick(22000, [peer(client)], true).result.role, 'host');
});

test('RTC control activity cannot hide a host whose match checkpoints stopped advancing', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), client = new Coordinator(B, ADDRESS_B, 0);
  runTogether([host, client]);
  for (const c of [host, client]) { c.launched('playing'); checkpoint(c); }
  const stalled = { ...peer(host), lastProgressAt: 10000, lastPacketAt: 35000 };
  assert.equal(client.tick(35000, [stalled], true).state, 'reconnecting');
  stalled.lastPacketAt = 45000;
  client.tick(45000, [stalled], true);
  assert.equal(client.epoch, 1); assert.equal(client.recovering, true);
});

test('a healthy old host still yields to a launched and checkpointed replacement authority', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), replacement = new Coordinator(B, ADDRESS_B, 0);
  runTogether([host, replacement]);
  for (const c of [host, replacement]) { c.launched('playing'); checkpoint(c); }
  replacement.recover(11000);
  replacement.tick(11000, [], true); replacement.tick(17000, [], true); replacement.tick(18500, [], true);
  replacement.launched('playing'); checkpoint(replacement, 150);
  host.tick(19000, [peer(replacement)], true);
  assert.equal(host.epoch, 1); assert.equal(host.tick(20500, [peer(replacement)], true).result.hostId, B);
});

test('an unrelated joiner at the same epoch cannot block a preserved match election', () => {
  const a = new Coordinator(A, ADDRESS_A, 0); runTogether([a]);
  a.launched('playing'); checkpoint(a); a.recover(11000);
  const unrelated = { id: B, address: ADDRESS_B, open: true,
    quick: { role: 'join', hostId: C, phase: 'launched', gamePhase: 'playing',
      epoch: 1, migration: true, matchId: 999, checkpointTick: 90 } };
  a.tick(11000, [unrelated], true); a.tick(17000, [unrelated], true);
  const status = a.tick(18500, [unrelated], true);
  assert.equal(status.result?.hostId, A);
  assert.equal(a.presence.matchId, 500);
});

test('replacement election prefers the newest checkpoint and preserves the same match', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), b = new Coordinator(B, ADDRESS_B, 0);
  const d = new Coordinator('3333333333333333', ADDRESS_C, 0);
  runTogether([host, b, d]);
  for (const coordinator of [host, b, d]) coordinator.launched('playing');
  checkpoint(host, 100); checkpoint(b, 100); checkpoint(d, 125);
  for (let now = 11000; now <= 31000; now += 100) {
    const snapshot = [b, d].map(coordinator => peer(coordinator));
    for (const coordinator of [b, d]) coordinator.tick(now, snapshot.filter(p => p.id !== coordinator.id), true);
  }
  assert.equal(d.result.role, 'host'); assert.equal(b.result.hostId, d.id);
  assert.equal(d.presence.matchId, 500); assert.equal(b.presence.matchId, 500);
  assert.equal(d.presence.checkpointTick, 125);
});

test('connected survivors agree on one preserved host when one or all brokers are offline', () => {
  for (const brokerAvailability of [[true, false], [false, false]]) {
    const host = new Coordinator(A, ADDRESS_A, 0), b = new Coordinator(B, ADDRESS_B, 0);
    const d = new Coordinator('3333333333333333', ADDRESS_C, 0);
    runTogether([host, b, d]);
    for (const coordinator of [host, b, d]) coordinator.launched('playing');
    checkpoint(host, 125); checkpoint(b, 125); checkpoint(d, 100);
    for (let now = 11000; now <= 31000; now += 100) {
      const snapshot = [b, d].map(coordinator => peer(coordinator));
      [b, d].forEach((coordinator, index) =>
        coordinator.tick(now, snapshot.filter(p => p.id !== coordinator.id), brokerAvailability[index]));
    }
    assert.equal(b.result?.role, 'host');
    assert.equal(d.result?.role, 'join');
    assert.equal(d.result?.hostId, B);
    for (const coordinator of [b, d]) {
      assert.equal(coordinator.epoch, 1);
      assert.equal(coordinator.presence.matchId, 500);
    }
  }
});

test('a broker outage cannot start a fresh room through RTC candidates alone', () => {
  const a = new Coordinator(A, ADDRESS_A, 0), b = new Coordinator(B, ADDRESS_B, 0);
  for (const now of [0, 6000, 10000, 30000]) {
    assert.equal(a.tick(now, [peer(b)], false).result, undefined);
    assert.equal(b.tick(now, [peer(a)], false).result, undefined);
  }
});

test('an unsnapshotted loading newcomer cannot replace a loaded survivor', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), survivor = new Coordinator(B, ADDRESS_B, 0);
  runTogether([host, survivor]);
  for (const coordinator of [host, survivor]) { coordinator.launched('playing'); checkpoint(coordinator); }
  const newcomer = new Coordinator(C, ADDRESS_C, 10000);
  newcomer.tick(10000, [peer(host), peer(survivor)], true);
  newcomer.tick(11500, [peer(host), peer(survivor)], true);
  newcomer.launched('loading');
  for (let now = 12000; now <= 33000; now += 100) {
    const snapshot = [survivor, newcomer].map(coordinator => peer(coordinator));
    for (const coordinator of [survivor, newcomer]) coordinator.tick(now, snapshot.filter(p => p.id !== coordinator.id), true);
  }
  assert.equal(survivor.result.role, 'host');
  assert.notEqual(newcomer.result?.role, 'host', 'a lower ID does not make an unloaded engine authoritative');
});

test('checkpoint admission rejects older ticks, different matches and stale epochs', () => {
  const a = new Coordinator(A, ADDRESS_A, 0); runTogether([a]); a.launched('playing');
  checkpoint(a, 125);
  a.checkpoint(0, 100, 500); a.checkpoint(0, 150, 999);
  assert.equal(a.presence.checkpointTick, 125); assert.equal(a.presence.matchId, 500);
  a.recover(10000);
  a.checkpoint(0, 200, 500);
  assert.equal(a.presence.checkpointTick, 125); assert.equal(a.presence.matchId, 500);
});

test('host loss without a verified checkpoint holds the match instead of creating a fresh game', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), joiner = new Coordinator(B, ADDRESS_B, 0);
  runTogether([host, joiner]); host.launched('playing'); joiner.launched('playing');
  joiner.tick(11000, [], true); joiner.tick(21000, [], true);
  for (let now = 22000; now <= 55000; now += 1000)
    assert.equal(joiner.tick(now, [], true).result, undefined);
  assert.equal(joiner.presence.role, 'candidate');
  assert.equal(joiner.result, null);
});

test('recovery cannot follow a host lacking a checkpoint of the same running match', () => {
  for (const metadata of [
    { migration: true, matchId: 999, checkpointTick: 125 },
    { migration: true, matchId: 500, checkpointTick: -1 },
    { migration: false, matchId: 500, checkpointTick: 125 },
  ]) {
    const host = new Coordinator(A, ADDRESS_A, 0), survivor = new Coordinator(B, ADDRESS_B, 0);
    runTogether([host, survivor]); survivor.launched('playing'); checkpoint(survivor);
    survivor.recover(11000);
    const incompatible = { id: C, address: ADDRESS_C, open: true,
      quick: { role: 'host', hostId: C, phase: 'launched', gamePhase: 'playing',
        epoch: 1, failover: true, ...metadata } };
    for (const now of [11000, 13000, 20000]) {
      const status = survivor.tick(now, [incompatible], true);
      assert.notEqual(status.result?.hostId, C);
      assert.notEqual(survivor.presence.hostId, C, 'host role alone does not authorize resetting onto another match');
    }
  }
});

test('a silent open host channel eventually fails over despite healthy signalling', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), joiner = new Coordinator(B, ADDRESS_B, 0);
  runTogether([host, joiner]);
  for (const coordinator of [host, joiner]) { coordinator.launched('playing'); checkpoint(coordinator); }
  const silent = { ...peer(host), lastPacketAt: 10000 };
  assert.equal(joiner.tick(34999, [silent], true).result.hostId, A);
  assert.equal(joiner.tick(35000, [silent], true).state, 'reconnecting');
  joiner.tick(45000, [silent], true);
  assert.equal(joiner.epoch, 1); assert.equal(joiner.result, null);
  joiner.tick(51000, [silent], true);
  assert.equal(joiner.tick(52500, [silent], true).result.role, 'host');
});

test('an isolated survivor cannot elect a host while signalling brokers are unavailable', () => {
  const host = new Coordinator(A, ADDRESS_A, 0), joiner = new Coordinator(B, ADDRESS_B, 0);
  runTogether([host, joiner]);
  for (const coordinator of [host, joiner]) { coordinator.launched('playing'); checkpoint(coordinator); }
  joiner.tick(10000, [], false); joiner.tick(20000, [], false);
  assert.equal(joiner.tick(30000, [], false).result, undefined);
  assert.equal(joiner.presence.role, 'candidate');
});

test('public API monitors after launch and calls failover once with the new host', async () => {
  const fixture = await network(), replacements = [], statuses = [];
  try {
    const pc = await fixture.hostPresence();
    // A signalling hello creates the real production peer callbacks.
    // Complete its channel opening, then lose that established connection.
    pc.channels[0].onopen();
    const attempt = fixture.net.quickPlay({ onFailover: value => replacements.push(value),
      onStatus: value => statuses.push(value) });
    fixture.tick(1500); const selected = await attempt;
    assert.equal(selected.role, 'join'); assert.equal(selected.epoch, 0);
    fixture.net.quickPlayStarted(); fixture.net.quickPlayPhase('playing');
    fixture.checkpoint();
    statuses.length = 0;
    pc.connectionState = 'failed'; pc.onconnectionstatechange();
    fixture.tick(2000); fixture.tick(12000); fixture.tick(18000); fixture.tick(19500);
    assert.equal(replacements.length, 1); assert.equal(replacements[0].role, 'host');
    assert.equal(replacements[0].room, 'TEST42'); assert.equal(replacements[0].hostAddress, fixture.net.address);
    assert.equal(replacements[0].epoch, 1, 'the engine receives the replacement authority epoch');
    fixture.tick(21000); assert.equal(replacements.length, 1);
    assert.ok(statuses.some(status => status.state === 'reconnecting' && status.hold === true),
      'simulation is held as soon as the host connection is lost');
    assert.ok(statuses.some(status => status.state === 'recovering' && status.hold === true));
    assert.ok(statuses.filter(status => status.hold === true).every(status => !/restart|reset/i.test(status.message)));
    assert.ok(!statuses.some(status => status.state === 'playing' && status.hold === false),
      'electing a replacement does not release the engine before migration finishes');
  } finally { await fixture.close(); }
});

test('public API repairs a brief reconnect on the same epoch and waits for the native ACK', async () => {
  const fixture = await network(), replacements = [], statuses = [], repairs = [];
  try {
    const host = await fixture.hostPresence(); host.channels[0].onopen();
    const attempt = fixture.net.quickPlay({ onFailover: value => replacements.push(value),
      onStatus: value => statuses.push(value), onReconnect: value => repairs.push(value) });
    fixture.tick(1500); const selected = await attempt;
    fixture.net.quickPlayStarted(); fixture.net.quickPlayPhase('playing');
    fixture.checkpoint();
    host.connectionState = 'failed'; host.onconnectionstatechange();
    fixture.tick(2000);
    assert.equal(statuses.at(-1).state, 'reconnecting'); assert.equal(statuses.at(-1).hold, true);
    const returned = await fixture.hostPresence(); returned.channels[0].onopen();
    fixture.tick(2100);
    assert.equal(repairs.length, 1); assert.equal(repairs[0].epoch, 0);
    assert.equal(statuses.at(-1).hold, true, 'RTC opening alone cannot resume a retired native stream');
    fixture.net.quickPlayPhase('playing'); fixture.tick(2200);
    assert.equal(statuses.at(-1).state, 'playing'); assert.equal(statuses.at(-1).hold, false);
    assert.equal(replacements.length, 0, 'the original match keeps its authority after a short outage');
    assert.equal(selected.epoch, 0); assert.equal(fixture.latest().quick.epoch, 0);
  } finally { await fixture.close(); }
});

test('native loss receipts during a loaded match election are consumed without advancing its epoch again', async () => {
  const fixture = await network(), replacements = [];
  try {
    const host = await fixture.hostPresence(); host.channels[0].onopen();
    const attempt = fixture.net.quickPlay({ onFailover: value => replacements.push(value) });
    fixture.tick(1500); await attempt;
    fixture.net.quickPlayPhase('playing'); fixture.checkpoint();
    host.connectionState = 'failed'; host.onconnectionstatechange();
    fixture.tick(2000); fixture.tick(12000);
    assert.equal(fixture.latest().quick.role, 'candidate', 'signalling is electing with no selected result');
    assert.equal(fixture.latest().quick.epoch, 1);
    assert.equal(fixture.net.quickPlayLost(), true);
    assert.equal(fixture.net.quickPlayLost(), true, 'duplicate native EOF is also part of this recovery');
    assert.equal(fixture.latest().quick.epoch, 1);
    assert.equal(fixture.latest().quick.matchId, 500);
    fixture.tick(18000); fixture.tick(19500);
    assert.equal(replacements.length, 1); assert.equal(replacements[0].epoch, 1);
  } finally { await fixture.close(); }
});

test('the first native client loss repairs its player once without changing host authority', async () => {
  const fixture = await network(), repairs = [];
  try {
    const host = await fixture.hostPresence(); host.channels[0].onopen();
    const attempt = fixture.net.quickPlay({ onReconnect: value => repairs.push(value) }); fixture.tick(1500); await attempt;
    fixture.net.quickPlayPhase('playing'); fixture.checkpoint();
    assert.equal(fixture.net.quickPlayLost(), true);
    assert.equal(fixture.latest().quick.epoch, 0);
    assert.equal(fixture.net.quickPlayLost(), true);
    assert.equal(fixture.latest().quick.epoch, 0);
    assert.equal(repairs.length, 1); assert.equal(repairs[0].hostAddress, ADDRESS_B);
  } finally { await fixture.close(); }
});

test('a client disconnect immediately after its reconnect ACK starts another repair', async () => {
  const fixture = await network(), repairs = [];
  try {
    const host = await fixture.hostPresence(); host.channels[0].onopen();
    const attempt = fixture.net.quickPlay({ onReconnect: value => repairs.push(value) });
    fixture.tick(1500); await attempt;
    fixture.net.quickPlayPhase('playing'); fixture.checkpoint();
    fixture.net.quickPlayLost();
    assert.equal(repairs.length, 1);
    fixture.net.quickPlayPhase('playing');
    // The ACK has completed native recovery, but the 100ms page timer has
    // not cleared its held flag. A new EOF belongs to the resumed connection.
    assert.equal(fixture.net.quickPlayLost(), true);
    assert.equal(repairs.length, 2, 'a lingering UI hold must not swallow a fresh native failure');
    assert.equal(repairs[1].epoch, 0);
    assert.equal(repairs[1].hostAddress, ADDRESS_B);
  } finally { await fixture.close(); }
});

test('the third player retries a replacement host that disconnects immediately after its ACK', async () => {
  const fixture = await network(), replacements = [], repairs = [];
  try {
    const host = await fixture.hostPresence(); host.channels[0].onopen();
    const attempt = fixture.net.quickPlay({ onFailover: value => replacements.push(value),
      onReconnect: value => repairs.push(value) });
    fixture.tick(1500); await attempt;
    fixture.net.quickPlayPhase('playing'); fixture.checkpoint();
    await fixture.hostPresence(1); fixture.tick(2000); fixture.tick(3500);
    assert.equal(replacements.length, 1); assert.equal(replacements[0].role, 'join');
    fixture.net.quickPlayPhase('playing');
    assert.equal(fixture.net.quickPlayLost(), true);
    assert.equal(repairs.length, 1, 'recovery must retry the third player after a new failure');
    assert.equal(repairs[0].epoch, 1);
    assert.equal(replacements.length, 1, 'this repair preserves the replacement authority');
    assert.equal(fixture.latest().quick.matchId, 500);
  } finally { await fixture.close(); }
});

for (const role of ['host', 'join']) {
  test(`a delayed native loss after selecting a replacement ${role} preserves that pending epoch`, async () => {
    const fixture = await network(), replacements = [];
    try {
      const host = await fixture.hostPresence(); host.channels[0].onopen();
      const attempt = fixture.net.quickPlay({ onFailover: value => replacements.push(value) });
      fixture.tick(1500); await attempt;
      fixture.net.quickPlayPhase('playing'); fixture.checkpoint();
      if (role === 'host') {
        host.connectionState = 'failed'; host.onconnectionstatechange();
        fixture.tick(2000); fixture.tick(12000); fixture.tick(18000); fixture.tick(19500);
      } else {
        await fixture.hostPresence(1); fixture.tick(2000); fixture.tick(3500);
      }
      assert.equal(replacements.length, 1); assert.equal(replacements[0].role, role);
      assert.equal(fixture.net.quickPlayLost(), true);
      assert.equal(fixture.net.quickPlayLost(), true);
      fixture.tick(20000);
      assert.equal(replacements.length, 1, 'old disconnects cannot initiate another failover');
      assert.equal(fixture.latest().quick.epoch, 1);
      assert.equal(fixture.latest().quick.matchId, 500);
      fixture.net.cancelQuickPlay();
      assert.equal(fixture.net.quickPlayLost(), false, 'explicit cancellation restores normal routing');
    } finally { await fixture.close(); }
  });
}

test('a connection failure after host takeover withdraws that host while preserving its match for another survivor', async () => {
  const fixture = await network(), replacements = [];
  try {
    const host = await fixture.hostPresence(); host.channels[0].onopen();
    const attempt = fixture.net.quickPlay({ onFailover: value => replacements.push(value) });
    fixture.tick(1500); await attempt;
    fixture.net.quickPlayPhase('playing'); fixture.checkpoint();
    host.connectionState = 'failed'; host.onconnectionstatechange();
    fixture.tick(2000); fixture.tick(12000); fixture.tick(18000); fixture.tick(19500);
    assert.equal(replacements[0].role, 'host');
    fixture.net.quickPlayPhase('playing'); fixture.checkpoint(150, 1);
    fixture.tick(20000); // Native playing acknowledgement releases the recovery hold.
    assert.equal(fixture.net.quickPlayLost(), true);
    assert.equal(fixture.latest().quick.epoch, 2);
    assert.equal(fixture.latest().quick.matchId, 500);
    assert.equal(fixture.latest().quick.gamePhase, 'migration-failed');
    assert.equal(fixture.net.quickPlayLost(), true, 'duplicate error cannot advance the authority again');
    fixture.tick(26000); fixture.tick(60000);
    assert.equal(replacements.length, 1, 'a failed host cannot immediately elect itself again');
    const replacement = await fixture.hostPresence(2); replacement.channels[0].onopen();
    fixture.tick(61000); fixture.tick(62500);
    assert.equal(replacements.length, 2); assert.equal(replacements[1].role, 'join');
    assert.equal(replacements[1].epoch, 2);
    fixture.net.quickPlayPhase('playing'); fixture.checkpoint(180, 2);
    assert.equal(fixture.latest().quick.matchId, 500);
    assert.equal(fixture.latest().quick.gamePhase, 'playing');
  } finally { await fixture.close(); }
});

test('unresolved setup and initial host loss retain their existing native status routing', async () => {
  const fixture = await network();
  try {
    assert.equal(fixture.net.quickPlayLost(), false);
    const attempt = fixture.net.quickPlay();
    assert.equal(fixture.net.quickPlayLost(), false);
    fixture.tick(6000); fixture.tick(7500); await attempt;
    assert.equal(fixture.net.quickPlayLost(), false, 'an initial hosting runtime is not recovering a loaded match');
    assert.equal(fixture.latest().quick.epoch, 0);
  } finally { await fixture.close(); }
});

test('a newer room epoch reaches the engine once and cannot be replaced by stale presence', async () => {
  const fixture = await network(), replacements = [], statuses = [];
  try {
    const host = await fixture.hostPresence(); host.channels[0].onopen();
    const attempt = fixture.net.quickPlay({ onFailover: value => replacements.push(value),
      onStatus: value => statuses.push(value) });
    fixture.tick(1500); await attempt;
    fixture.net.quickPlayStarted(); fixture.net.quickPlayPhase('playing');
    fixture.checkpoint();
    await fixture.hostPresence(4); fixture.tick(2000); fixture.tick(3500);
    assert.equal(replacements.length, 1); assert.equal(replacements[0].epoch, 4);
    assert.equal(replacements[0].role, 'join');
    assert.ok(statuses.some(status => status.state === 'recovering' && status.hold === true));
    await fixture.hostPresence(0); fixture.tick(4000);
    assert.equal(replacements.length, 1); assert.equal(fixture.latest().quick.epoch, 4);
  } finally { await fixture.close(); }
});

test('a fresh entrant follows a checkpointed migrated host without becoming eligible to replace it', async () => {
  const fixture = await network();
  try {
    const host = await fixture.hostPresence(4); host.channels[0].onopen();
    const attempt = fixture.net.quickPlay();
    fixture.tick(1500); const selected = await attempt;
    assert.equal(selected.role, 'join'); assert.equal(selected.epoch, 4);
    assert.equal(selected.hostAddress, ADDRESS_B);
    fixture.net.quickPlayStarted();
    assert.equal(fixture.latest().quick.checkpointTick, -1, 'joining a host is not a verified local snapshot');
    host.connectionState = 'failed'; host.onconnectionstatechange();
    fixture.tick(2000); fixture.tick(12000); fixture.tick(18000); fixture.tick(19500);
    assert.equal(fixture.latest().quick.role, 'candidate', 'an entrant still loading cannot create a fresh replacement match');
  } finally { await fixture.close(); }
});

test('a native migration failure withdraws host authority while preserving later same-match recovery', async () => {
  const fixture = await network(), replacements = [];
  try {
    const attempt = fixture.net.quickPlay({ onFailover: value => replacements.push(value) });
    fixture.tick(6000); fixture.tick(7500); await attempt;
    fixture.net.quickPlayStarted(); fixture.net.quickPlayPhase('playing'); fixture.checkpoint();
    fixture.net.quickPlayPhase('migration-failed');
    assert.equal(fixture.latest().quick.role, 'candidate');
    assert.equal(fixture.latest().quick.hostId, null);
    assert.equal(fixture.latest().quick.gamePhase, 'migration-failed');
    assert.equal(fixture.latest().quick.checkpointTick, -1);
    assert.equal(fixture.latest().quick.matchId, 500);
    fixture.checkpoint(125); // A delayed transfer cannot override the native failure receipt.
    fixture.tick(13500); fixture.tick(15000); fixture.tick(42501);
    assert.equal(fixture.latest().quick.role, 'candidate'); assert.equal(replacements.length, 0);
    const host = await fixture.hostPresence(1); host.channels[0].onopen();
    fixture.tick(43000); fixture.tick(44500);
    assert.equal(replacements.length, 1); assert.equal(replacements[0].role, 'join');
    assert.equal(replacements[0].epoch, 1); assert.equal(replacements[0].hostAddress, ADDRESS_B);
    assert.equal(fixture.latest().quick.matchId, 500, 'failure retains the preserved match identity');
    fixture.net.quickPlayPhase('playing'); fixture.checkpoint(150, 1);
    assert.equal(fixture.latest().quick.gamePhase, 'playing'); assert.equal(fixture.latest().quick.checkpointTick, 150);
  } finally { await fixture.close(); }
});

test('an entrant still loading keeps its election timeout even after inheriting a match identity', () => {
  const host = new Coordinator(A, ADDRESS_A, 0); runTogether([host]); host.launched('playing');
  host.epoch = host.presence.epoch = 4; checkpoint(host);
  const entrant = new Coordinator(B, ADDRESS_B, 10000);
  entrant.tick(10000, [peer(host)], true); entrant.tick(11500, [peer(host)], true); entrant.launched('loading');
  assert.equal(entrant.presence.matchId, 500);
  entrant.tick(12000, [], true); entrant.tick(22000, [], true);
  assert.match(entrant.tick(57000, [], true).error, /reachable host/,
    'a selected room identity alone does not establish a loaded match that must wait indefinitely');
});

test('a failed client follows a later host epoch despite invalidated replacement eligibility', async () => {
  const fixture = await network(), replacements = [];
  try {
    const host = await fixture.hostPresence(); host.channels[0].onopen();
    const attempt = fixture.net.quickPlay({ onFailover: value => replacements.push(value) });
    fixture.tick(1500); await attempt;
    fixture.net.quickPlayStarted(); fixture.net.quickPlayPhase('playing'); fixture.checkpoint();
    fixture.net.quickPlayPhase('migration-failed');
    assert.equal(fixture.latest().quick.role, 'join'); assert.equal(fixture.latest().quick.checkpointTick, -1);
    host.connectionState = 'failed'; host.onconnectionstatechange();
    fixture.tick(2000); fixture.tick(12000); fixture.tick(18000); fixture.tick(19500);
    assert.equal(replacements.length, 0, 'a paused failed client cannot promote its old snapshot');
    const replacement = await fixture.hostPresence(1); replacement.channels[0].onopen();
    fixture.tick(20000); fixture.tick(21500);
    assert.equal(replacements.length, 1); assert.equal(replacements[0].role, 'join');
    assert.equal(replacements[0].epoch, 1); assert.equal(fixture.latest().quick.matchId, 500);
    assert.equal(fixture.latest().quick.checkpointTick, -1, 'following a new host does not invent a verified snapshot');
  } finally { await fixture.close(); }
});

test('public quick-play API withdraws cancelled eligibility and allows a fresh attempt', async () => {
  const fixture = await network();
  try {
    assert.equal(fixture.latest().quick, null, 'being in a room is not playing');
    const controller = new AbortController(), statuses = [];
    const attempt = fixture.net.quickPlay({ signal: controller.signal, onStatus: value => statuses.push(value) });
    const rejection = assert.rejects(attempt, { name: 'AbortError' });
    const activeSequence = fixture.latest().quickSequence;
    assert.equal(fixture.latest().quick.role, 'candidate');
    controller.abort();
    await rejection;
    assert.equal(fixture.latest().quick, null);
    assert.ok(fixture.latest().quickSequence > activeSequence);
    assert.ok(statuses.every(value => typeof value.state === 'string' && typeof value.message === 'string'));
    const retry = fixture.net.quickPlay();
    fixture.tick(6000); fixture.tick(7500);
    const result = await retry;
    assert.equal(result.role, 'host'); assert.equal(result.room, 'TEST42');
    assert.equal(result.hostAddress, fixture.net.address);
    assert.equal(result.epoch, 0);
  } finally { await fixture.close(); }
});

test('abort stays attached after resolution and clears the launched host claim', async () => {
  const fixture = await network();
  try {
    const controller = new AbortController();
    const attempt = fixture.net.quickPlay({ signal: controller.signal });
    fixture.tick(6000); fixture.tick(7500);
    await attempt;
    fixture.net.quickPlayStarted();
    assert.equal(fixture.latest().quick.phase, 'launched');
    assert.equal(fixture.latest().quick.gamePhase, 'loading');
    fixture.net.quickPlayPhase('playing');
    assert.equal(fixture.latest().quick.gamePhase, 'playing');
    controller.abort();
    assert.equal(fixture.latest().quick, null);
    fixture.net.quickPlayPhase('playing');
    assert.equal(fixture.latest().quick, null, 'late game phase cannot resurrect cancellation');
  } finally { await fixture.close(); }
});

test('returning to menu or leaving the room withdraws quick-play eligibility', async () => {
  const fixture = await network();
  try {
    let attempt = fixture.net.quickPlay();
    fixture.tick(6000); fixture.tick(7500); await attempt;
    fixture.net.quickPlayPhase('menu');
    assert.equal(fixture.latest().quick, null);
    attempt = fixture.net.quickPlay();
    const rejection = assert.rejects(attempt, { name: 'AbortError' });
    await fixture.net.leave(); await rejection;
    assert.equal(fixture.net.status().room, null);
    await assert.rejects(fixture.net.quickPlay(), /Join a browser room/);
  } finally { await fixture.close(); }
});

test('native invite transport joins only its connected host and never elects one', async () => {
  const fixture = await network();
  try {
    const transport = { connected: true, address: 0x01404064, hostAddress: 0x02404064, close() {} };
    await fixture.net.useTransport(transport);
    const result = await fixture.net.quickPlay();
    assert.equal(result.role, 'join'); assert.equal(result.room, null);
    assert.equal(result.hostAddress, transport.hostAddress);
    transport.connected = false;
    await assert.rejects(fixture.net.quickPlay(), /host is not connected/);
  } finally { await fixture.close(); }
});
