const test = require('node:test');
const assert = require('node:assert/strict');
const { network } = require('./web-quick-play-fixture.cjs');

const ORIGINAL = 'ffffffffffffffff', REPLACEMENT = 'fffffffffffffffe';
const ADDRESS = 0x0301010a;
const control = (sequence, quick) => 'halo-room-quick-v1:' + JSON.stringify({ sequence, quick });
const presence = (epoch = 1, matchId = 500) => ({ role: 'host', hostId: REPLACEMENT,
  phase: 'launched', gamePhase: 'playing', epoch, failover: true,
  migration: true, matchId, checkpointTick: 125 });

async function threePlayer() {
  const fixture = await network(), replacements = [], statuses = [];
  const original = await fixture.hostPresence(); original.channels[0].onopen();
  const replacement = await fixture.peerPresence({ id: REPLACEMENT, address: ADDRESS, sequence: 1,
    quick: { ...presence(0), role: 'join', hostId: ORIGINAL, checkpointTick: 90 } });
  replacement.channels[0].onopen();
  const attempt = fixture.net.quickPlay({ onFailover: value => replacements.push(value),
    onStatus: value => statuses.push(value) });
  fixture.tick(1500); await attempt;
  fixture.net.quickPlayPhase('playing'); fixture.checkpoint();
  return { fixture, original, replacement, replacements, statuses };
}

test('third player discovers the replacement over its surviving RTC connection when brokers disappear', async () => {
  const { fixture, original, replacement, replacements, statuses } = await threePlayer();
  try {
    fixture.disconnectBrokers();
    original.connectionState = 'failed'; original.onconnectionstatechange();
    fixture.net.quickPlayLost(); fixture.tick(2000);
    assert.equal(replacements.length, 0);
    replacement.channels[1].onmessage({ data: control(2, presence()) });
    fixture.tick(2100); fixture.tick(3700);
    assert.equal(replacements.length, 1, 'the third player must leave election and target the reachable replacement');
    assert.equal(replacements[0].role, 'join');
    assert.equal(replacements[0].hostId, REPLACEMENT);
    assert.equal(replacements[0].hostAddress, ADDRESS);
    assert.equal(replacements[0].epoch, 1);
    assert.equal(fixture.latest().quick.matchId, 500);
    assert.ok(!statuses.some(s => s.state === 'playing' && s.hold === false), 'native ACK must still gate resume');
    fixture.net.quickPlayPhase('playing'); fixture.checkpoint(150, 1); fixture.tick(3800);
    assert.equal(statuses.at(-1).hold, false);
    fixture.tick(60000);
    assert.equal(replacements.length, 1, 'delayed ticks must not deliver the same migration twice');
  } finally { await fixture.close(); }
});

test('RTC authority announcements are periodic and bounded by their authenticated peer and sequence', async () => {
  const { fixture, replacement } = await threePlayer();
  try {
    fixture.disconnectBrokers(); fixture.tick(1600);
    const sent = replacement.channels[1].sent.filter(s => typeof s === 'string' && s.startsWith('halo-room-quick-v1:'));
    assert.ok(sent.length, 'live peers receive authority state independently of brokers');
    assert.equal(JSON.parse(sent.at(-1).slice('halo-room-quick-v1:'.length)).quick.matchId, 500);
    replacement.channels[1].onmessage({ data: control(10, presence()) }); fixture.tick(1700);
    const sequence = fixture.latest().quick.epoch;
    for (const value of [control(9, presence(7)), control(11, { ...presence(7), hostId: ORIGINAL }),
      control(-1, presence(7)), 'halo-room-quick-v1:{', 'halo-room-quick-v1:' + 'x'.repeat(2000)]) {
      replacement.channels[1].onmessage({ data: value }); fixture.tick(1800);
      assert.equal(fixture.latest().quick.epoch, sequence, 'bad or stale control cannot replace peer authority');
    }
    await fixture.peerPresence({ id: REPLACEMENT, address: ADDRESS, sequence: 9, quick: presence(7) });
    fixture.tick(1900);
    assert.equal(fixture.latest().quick.epoch, sequence, 'older broker delivery cannot overwrite newer RTC authority');
    replacement.connectionState = 'failed'; replacement.onconnectionstatechange();
    replacement.channels[1].onmessage({ data: control(12, presence(7)) }); fixture.tick(2000);
    assert.equal(fixture.latest().quick.epoch, sequence, 'an obsolete connection cannot inject a newer authority');
  } finally { await fixture.close(); }
});

test('a known replacement that is unreachable reports connection progress instead of an election', async () => {
  const { fixture, original, replacement, statuses, replacements } = await threePlayer();
  try {
    original.connectionState = 'failed'; original.onconnectionstatechange();
    replacement.connectionState = 'failed'; replacement.onconnectionstatechange();
    fixture.net.quickPlayLost();
    await fixture.peerPresence({ id: REPLACEMENT, address: ADDRESS, sequence: 2, quick: presence() });
    fixture.tick(2000); fixture.tick(12000); fixture.tick(17000);
    assert.equal(replacements.length, 0, 'an unopened RTC connection cannot become the native target');
    assert.equal(statuses.at(-1).hold, true);
    assert.match(statuses.at(-1).message, /Connecting to the replacement host/);
  } finally { await fixture.close(); }
});

test('a replacement disappearing during reservation does not cancel the preserved match', async () => {
  const { fixture, original, replacement, replacements, statuses } = await threePlayer();
  try {
    fixture.disconnectBrokers(); original.connectionState = 'failed'; original.onconnectionstatechange();
    fixture.net.quickPlayLost();
    replacement.channels[1].onmessage({ data: control(2, presence()) }); fixture.tick(2000);
    assert.equal(replacements.length, 0, 'the replacement is still being reserved');
    assert.equal(fixture.latest().quick?.role, 'join', 'the test must enter the replacement reservation');
    replacement.connectionState = 'failed'; replacement.onconnectionstatechange(); fixture.tick(2100);
    assert.equal(fixture.latest().quick?.epoch, 1);
    assert.equal(fixture.latest().quick?.matchId, 500);
    assert.equal(fixture.latest().quick?.role, 'candidate');
    assert.ok(!statuses.some(s => s.state === 'error'));
    const returned = await fixture.peerPresence({ id: REPLACEMENT, address: ADDRESS, sequence: 3, quick: presence() });
    returned.channels[0].onopen(); fixture.tick(3000); fixture.tick(4600);
    assert.equal(replacements.length, 1);
    assert.equal(replacements[0].hostId, REPLACEMENT);
    assert.equal(replacements[0].epoch, 1);
  } finally { await fixture.close(); }
});
