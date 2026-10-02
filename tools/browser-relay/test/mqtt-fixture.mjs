// Local MQTT 3.1.1 fixture for native transport tests; never deploy this broker.
import assert from 'node:assert/strict';
import net from 'node:net';
import { pathToFileURL } from 'node:url';

function mqtt(kind, body) {
  const header = [kind];
  let size = body.length;
  do { const digit = size % 128; size = Math.floor(size / 128); header.push(digit | (size ? 128 : 0)); } while (size);
  return Buffer.concat([Buffer.from(header), body]);
}

export function broker() {
  const sockets = new Set(), topics = new Map();
  const server = net.createServer((socket) => {
    sockets.add(socket);
    let pending = Buffer.alloc(0);
    socket.on('close', () => { sockets.delete(socket); for (const peers of topics.values()) peers.delete(socket); });
    socket.on('error', () => {});
    socket.on('data', (chunk) => {
      pending = Buffer.concat([pending, chunk]);
      try {
        while (pending.length > 1) {
          let size = 0, index = 1, multiplier = 1, byte;
          do {
            if (index >= pending.length) return;
            byte = pending[index++];
            size += (byte & 127) * multiplier; multiplier *= 128;
            assert.ok(index <= 5 && size < 65536);
          } while (byte & 128);
          if (pending.length < index + size) return;
          const kind = pending[0], body = pending.subarray(index, index + size);
          pending = pending.subarray(index + size);
          if (kind === 0x10) socket.write(Buffer.from([0x20, 0x02, 0, 0]));
          else if (kind === 0x82) {
            const topic = body.subarray(4, 4 + body.readUInt16BE(2)).toString('hex');
            if (!topics.has(topic)) topics.set(topic, new Set());
            topics.get(topic).add(socket);
            socket.write(Buffer.from([0x90, 3, body[0], body[1], 0]));
          } else if (kind === 0x30) {
            const topic = body.subarray(2, 2 + body.readUInt16BE(0)).toString('hex');
            for (const subscriber of topics.get(topic) || []) subscriber.write(mqtt(0x30, body));
          } else if (kind === 0xc0) socket.write(Buffer.from([0xd0, 0]));
        }
      } catch { socket.destroy(); }
    });
  });
  return { server, close: () => { for (const socket of sockets) socket.destroy(); server.close(); } };
}


if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const fixture = broker();
  fixture.server.listen(Number(process.argv[2] || 18884), '127.0.0.1', () => {
    console.log('Local native test MQTT broker ready on ' + fixture.server.address().port);
  });
  for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, fixture.close);
}
