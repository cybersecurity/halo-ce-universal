#!/usr/bin/env python3
"""Real native P2P roundtrip against a local MQTT broker and socket echo host.

This tests encrypted signalling/UDP/KCP and framing; it does not simulate Halo,
connect to a public broker, join a real match, or prove game protocol compatibility.
"""
import asyncio
import json
import os
from pathlib import Path
import shlex
import struct
import sys


def packet(kind, source, destination, source_port, destination_port, payload=b''):
    size = (24 + len(payload) + 3) & ~3
    return struct.pack('<IIII', size, kind, source, destination) + struct.pack('!HH', source_port, destination_port) + struct.pack('<I', len(payload)) + payload + bytes(size - 24 - len(payload))


def mqtt_packet(kind, payload):
    length = len(payload)
    encoded = bytearray([kind])
    while True:
        digit = length % 128
        length //= 128
        encoded.append(digit | (128 if length else 0))
        if not length:
            return bytes(encoded) + payload


class Broker:
    def __init__(self):
        self.subscribers = {}
        self.connections = set()

    async def connection(self, reader, writer):
        self.connections.add(writer)
        try:
            while True:
                kind = (await reader.readexactly(1))[0]
                size, shift = 0, 0
                while True:
                    digit = (await reader.readexactly(1))[0]
                    size |= (digit & 127) << shift
                    shift += 7
                    if not digit & 128:
                        break
                    assert shift <= 28
                assert size < 65536
                body = await reader.readexactly(size)
                if kind == 0x10:
                    writer.write(b'\x20\x02\x00\x00')
                elif kind == 0x82:
                    topic_size = int.from_bytes(body[2:4], 'big')
                    topic = body[4:4 + topic_size]
                    self.subscribers.setdefault(topic, set()).add(writer)
                    writer.write(b'\x90\x03' + body[:2] + b'\x00')
                elif kind == 0x30:
                    topic_size = int.from_bytes(body[:2], 'big')
                    topic = body[2:2 + topic_size]
                    for client in self.subscribers.get(topic, set()).copy():
                        if not client.is_closing():
                            client.write(mqtt_packet(0x30, body))
                elif kind == 0xc0:
                    writer.write(b'\xd0\x00')
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            self.connections.discard(writer)
            for clients in self.subscribers.values():
                clients.discard(writer)
            writer.close()


class Worker:
    @classmethod
    async def start(cls, executable, broker, host=False):
        self = cls()
        # A loader prefix supports testing extracted 32-bit toolchains without
        # changing the host/container's installed libraries.
        command = shlex.split(os.environ.get('HALO_RELAY_TEST_EXEC_PREFIX', '')) + [executable]
        if host:
            command.append('--echo-host')
        self.process = await asyncio.create_subprocess_exec(*command,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            env={**os.environ, 'HALO_RELAY_TEST_BROKER': broker})
        return self

    async def send(self, kind, data):
        self.process.stdin.write(struct.pack('<I', len(data) + 1) + bytes([kind]) + data)
        await self.process.stdin.drain()

    async def receive(self):
        size = struct.unpack('<I', await self.process.stdout.readexactly(4))[0]
        assert 0 < size <= 65536
        data = await self.process.stdout.readexactly(size)
        return data[0], json.loads(data[1:]) if data[0] == 3 else data[1:]

    async def event(self, name):
        while True:
            kind, data = await asyncio.wait_for(self.receive(), timeout=15)
            if kind == 3 and data['type'] == name:
                return data
            assert kind != 3 or data['type'] != 'error', data

    async def stop(self):
        if self.process.returncode is None:
            self.process.terminate()
        await self.process.wait()
        error = await self.process.stderr.read()
        assert not error, error.decode(errors='replace')


async def main():
    executable = str(Path(sys.argv[1]).resolve())
    broker = Broker()
    server = await asyncio.start_server(broker.connection, '127.0.0.1', 0)
    address = '127.0.0.1:' + str(server.sockets[0].getsockname()[1])
    workers = []
    try:
        host = await Worker.start(executable, address, host=True)
        workers.append(host)
        await host.event('ready')
        invitation = await host.event('test_invite')
        code = invitation['invite'].removeprefix('halo://join/')
        client = await Worker.start(executable, address)
        workers.append(client)
        await client.send(1, code.encode())
        ready = await client.event('ready')
        peer = await client.event('peer')
        assert len(ready['identifier']) == 12 and peer['connected'] is True
        assert peer['identifier'] == code[:12]
        source, destination = ready['address'], peer['address']
        for target in (destination, 0xffffffff):
            payload = b'udp native tunnel\x00\xff' + bytes(range(256))
            await client.send(2, packet(1, source, target, 5151, 5150, payload))
            kind, response = await asyncio.wait_for(client.receive(), timeout=10)
            assert kind == 2 and struct.unpack_from('<I', response, 4)[0] == 1
            assert response[24:24 + len(payload)] == payload
            assert struct.unpack_from('<II', response, 8) == (destination, source)
            assert struct.unpack_from('!HH', response, 16) == (5150, 5151)
        print('PASS: native encrypted UDP unicast and discovery-broadcast roundtrip')
        await client.send(2, packet(2, source, destination, 49157, 5150))
        expected = bytes(range(256)) * 100  # exceeds both Web and KCP frames
        for offset in range(0, len(expected), 8000):
            await client.send(2, packet(3, source, destination, 49157, 5150, expected[offset:offset+8000]))
        received = bytearray()
        while len(received) < len(expected):
            kind, response = await asyncio.wait_for(client.receive(), timeout=15)
            assert kind == 2 and struct.unpack_from('<I', response, 4)[0] == 3
            length = struct.unpack_from('<I', response, 20)[0]
            received += response[24:24+length]
        assert received == expected
        print('PASS: native KCP/TCP stream preserves 25,600 bytes across frames')
        await client.send(2, packet(4, source, destination, 49157, 5150))
        assert (await host.event('test_stream_closed'))['bytes'] == len(expected)
        # The next native test-host stream is a sink: close immediately after
        # sending a burst, without waiting for acknowledgements or echoed data.
        await client.send(2, packet(2, source, destination, 49158, 5150))
        final_burst = bytes(range(256)) * 256
        checksum = 0
        for byte in final_burst:
            checksum = ((checksum * 16777619) ^ byte) & 0xffffffff
        for offset in range(0, len(final_burst), 8000):
            await client.send(2, packet(3, source, destination, 49158, 5150, final_burst[offset:offset+8000]))
        await client.send(2, packet(4, source, destination, 49158, 5150))
        closed = await host.event('test_stream_closed')
        assert closed['bytes'] == len(final_burst) and closed['checksum'] == checksum, closed
        print('PASS: immediate CLOSE drains all 65,536 queued bytes through native KCP')
        # Destination spoofing must fail closed rather than opening arbitrary sockets.
        await client.send(2, packet(1, source, 0x0100007f, 5151, 5150, b'blocked'))
        error = await client.event('error')
        assert error['code'] == 'invalid_packet'
        await asyncio.wait_for(client.process.wait(), timeout=3)
        assert client.process.returncode == 1
        print('PASS: non-peer destination is rejected and session closes')
        invalid = await Worker.start(executable, address)
        workers.append(invalid)
        await invalid.send(1, b'x' * 44)
        assert (await invalid.event('error'))['code'] == 'invalid_invite'
        print('PASS: invalid invite rejected before networking starts')
        overlong = await Worker.start(executable, address)
        workers.append(overlong)
        overlong.process.stdin.write(struct.pack('<I', 65537))
        await overlong.process.stdin.drain()
        assert (await overlong.event('error'))['code'] == 'invalid_record'
        print('PASS: oversized framing rejected without allocation')
    finally:
        for worker in workers:
            await worker.stop()
        for connection in broker.connections.copy():
            connection.close()
        server.close()
        await server.wait_closed()


if __name__ == '__main__':
    asyncio.run(main())
