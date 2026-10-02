// Protocol fixture only. This is NOT evidence that native multiplayer works.
import { writeFileSync } from 'node:fs';
import { RecordDecoder, record } from '../protocol.mjs';
const mode = process.argv[2] || 'echo';
if (process.argv[3]) writeFileSync(process.argv[3], String(process.pid));
const decoder = new RecordDecoder();
if (mode === 'slow') setInterval(() => {}, 1000); // a deliberately stalled live process
let invited = false;
process.stdin.on('data', (bytes) => {
  decoder.push(bytes, (type, data) => {
    if (type === 1 && !invited && /^[a-f0-9]{44}$/.test(data.toString())) {
      invited = true;
      if (mode === 'exit') return process.exit(4);
      if (mode === 'silent') return;
      const ready = record(3, JSON.stringify({ type: 'ready', protocol: 1, identifier: '112233445566', address: 0x02004064 }));
      // Deliberately fragmented header and body exercise framing across pipe reads.
      process.stdout.write(ready.subarray(0, 2));
      setTimeout(() => {
        process.stdout.write(ready.subarray(2));
        process.stdout.write(record(3, JSON.stringify({ type: 'peer', identifier: data.toString().slice(0, 12),
          address: 0x03004064, connected: true })));
      }, 5);
    } else if (type === 2 && invited) {
      if (mode === 'malformed') { process.stdout.write(Buffer.from([0xff, 0xff, 0xff, 0xff])); return; }
      if (mode === 'slow') { process.stdin.pause(); return; }
      const response = Buffer.from(data);
      response.writeUInt32LE(0x03004064, 8);
      response.writeUInt32LE(0x02004064, 12);
      response.writeUInt16BE(data.readUInt16BE(18), 16);
      response.writeUInt16BE(data.readUInt16BE(16), 18);
      process.stdout.write(record(2, response));
    } else process.exit(5);
  });
});
process.stdin.on('end', () => process.exit(0));
