import http from 'node:http';
import { spawn } from 'node:child_process';
import { timingSafeEqual } from 'node:crypto';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { WebSocketServer, WebSocket } from 'ws';
import { MAX_PACKET, RecordDecoder, SessionPackets, record, isVirtualAddress } from './protocol.mjs';

const LOOPBACK = new Set(['127.0.0.1', '::1', 'localhost']);
const DEFAULT_WORKER = fileURLToPath(new URL('../../port/relay/halo-relay-worker', import.meta.url));
const DEFAULTS = {
  host: '127.0.0.1', port: 8789, worker: DEFAULT_WORKER, workerArgs: [],
  allowedOrigins: ['http://localhost:8780', 'http://127.0.0.1:8780'], accessToken: '',
  maxSessions: 8, maxSessionsPerIP: 8, joinTimeoutMs: 10000, peerTimeoutMs: 100000,
  idleTimeoutMs: 120000, maxSessionMs: 6 * 60 * 60 * 1000,
  maxBufferedBytes: 1024 * 1024, backpressureTimeoutMs: 15000,
  bytesPerSecond: 2 * 1024 * 1024, packetsPerSecond: 2000, heartbeatMs: 30000,
};

function tokenMatches(value, expected) {
  if (!expected) return value === undefined || value === '';
  if (typeof value !== 'string') return false;
  const a = Buffer.from(value), b = Buffer.from(expected);
  return a.length === b.length && timingSafeEqual(a, b);
}

export function validateConfig(input = {}) {
  const config = { ...DEFAULTS, ...input };
  if (typeof config.host !== 'string' || !Array.isArray(config.allowedOrigins) || !config.allowedOrigins.length)
    throw new Error('Configure a bind address and at least one allowed Origin');
  for (const origin of config.allowedOrigins) {
    const url = new URL(origin);
    if (url.origin !== origin || !['http:', 'https:'].includes(url.protocol) ||
        (url.protocol !== 'https:' && !LOOPBACK.has(url.hostname)))
      throw new Error('Allowed Origins must be exact HTTPS origins (HTTP is allowed on loopback)');
  }
  if (typeof config.accessToken !== 'string' ||
      (!LOOPBACK.has(config.host) && Buffer.byteLength(config.accessToken) < 32))
    throw new Error('Non-loopback binding requires RELAY_ACCESS_TOKEN of at least 32 bytes');
  for (const key of Object.keys(DEFAULTS).filter((key) => typeof DEFAULTS[key] === 'number')) {
    if (!Number.isSafeInteger(config[key]) || config[key] < (key === 'port' ? 0 : 1))
      throw new Error(`Invalid ${key}`);
  }
  if (config.port > 65535) throw new Error('Invalid port');
  return config;
}

class Budget {
  constructor(rate) { this.rate = rate; this.left = rate * 2; this.at = Date.now(); }
  take(amount) {
    const now = Date.now();
    this.left = Math.min(this.rate * 2, this.left + (now - this.at) * this.rate / 1000);
    this.at = now;
    if (amount > this.left) return false;
    this.left -= amount;
    return true;
  }
}

export function createRelay(options = {}) {
  const config = validateConfig(options);
  const clients = new Set();
  const attempts = new Map();
  let closing = false;
  const server = http.createServer((request, response) => {
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    if (request.method === 'GET' && request.url === '/health') {
      response.writeHead(closing ? 503 : 200, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify({ service: 'halo-browser-relay', protocol: 1, status: closing ? 'closing' : 'ok' }));
    } else { response.writeHead(404); response.end(); }
  });
  server.headersTimeout = 10000;
  server.requestTimeout = 10000;
  const wss = new WebSocketServer({ noServer: true, maxPayload: MAX_PACKET, perMessageDeflate: false });
  server.on('upgrade', (request, socket, head) => {
    const reject = (status) => {
      socket.end(`HTTP/1.1 ${status}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
    };
    const ip = request.socket.remoteAddress || '';
    if (closing || request.method !== 'GET' || request.url !== '/join') return reject('404 Not Found');
    if (!config.allowedOrigins.includes(request.headers.origin)) return reject('403 Forbidden');
    if (clients.size >= config.maxSessions || [...clients].filter((c) => c.ip === ip).length >= config.maxSessionsPerIP)
      return reject('503 Service Unavailable');
    const now = Date.now();
    for (const [key, attempt] of attempts) if (now - attempt.at > 60000) attempts.delete(key);
    let attempt = attempts.get(ip);
    if (!attempt) {
      if (attempts.size >= 1024) return reject('429 Too Many Requests');
      attempt = { at: now, count: 0 }; attempts.set(ip, attempt);
    }
    if (++attempt.count > 20) return reject('429 Too Many Requests');
    wss.handleUpgrade(request, socket, head, (ws) => accept(ws, ip));
  });

  function accept(ws, ip) {
    let worker, packets, joined = false, stopped = false, alive = true, peerConnected = false;
    let workerPausedAt = 0, socketPausedAt = 0;
    const created = Date.now();
    let lastPacket = created;
    const decoder = new RecordDecoder();
    const byteBudget = new Budget(config.bytesPerSecond), packetBudget = new Budget(config.packetsPerSecond);
    const client = { ip, stop };
    clients.add(client);
    const timer = setInterval(check, Math.min(1000, config.joinTimeoutMs));
    const ping = setInterval(() => {
      if (!alive) return stop(1001, 'Connection timed out');
      alive = false;
      ws.ping();
    }, config.heartbeatMs);
    ws.on('pong', () => { alive = true; });
    ws.on('close', () => stop());
    ws.on('error', () => stop());
    ws.on('message', (data, binary) => {
      if (stopped) return;
      try {
        if (!packetBudget.take(1) || !byteBudget.take(data.length)) throw new Error('Session rate limit');
        if (!joined) {
          if (binary || data.length > 1024) throw new Error('Expected join request');
          const request = JSON.parse(data.toString());
          if (!request || request.type !== 'join' || typeof request.invite !== 'string' ||
              !/^[a-f0-9]{44}$/i.test(request.invite) ||
              Object.keys(request).some((key) => !['type', 'invite', 'accessToken'].includes(key)) ||
              !tokenMatches(request.accessToken, config.accessToken)) throw new Error('Invalid join request');
          joined = true;
          launchWorker(request.invite.toLowerCase());
        } else {
          if (!binary || !packets) throw new Error('Game session is not ready');
          if (packets.validate(data, true).discard) return;
          writeWorker(record(2, data));
          lastPacket = Date.now();
        }
      } catch { stop(1008, 'Invalid request or session limit'); }
    });

    function check() {
      const now = Date.now();
      if (!packets && now - created > config.joinTimeoutMs) return stop(1008, 'Join timed out');
      if (!peerConnected && now - created > config.peerTimeoutMs) return stop(1008, 'Native host did not connect');
      if (now - lastPacket > config.idleTimeoutMs || now - created > config.maxSessionMs)
        return stop(1001, 'Session expired');
      if ((workerPausedAt && now - workerPausedAt > config.backpressureTimeoutMs) ||
          (socketPausedAt && now - socketPausedAt > config.backpressureTimeoutMs))
        return stop(1013, 'Connection too slow');
    }

    function launchWorker(invite) {
      worker = spawn(config.worker, config.workerArgs, {
        stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true,
        // Never give the worker the relay's capability token or operator config.
        env: { PATH: process.env.PATH || '/usr/bin:/bin', LANG: 'C', ...(config.workerEnv || {}) },
      });
      const hostIdentifier = invite.slice(0, 12);
      worker.on('error', () => stop(1011, 'Native worker unavailable'));
      worker.on('exit', () => stop(1011, 'Native worker stopped'));
      worker.on('close', () => clients.delete(client));
      worker.stdin.on('error', () => stop(1011, 'Native worker input closed'));
      worker.stdin.on('drain', () => { workerPausedAt = 0; if (!stopped) ws.resume(); });
      worker.stdout.on('error', () => stop(1011, 'Native worker output closed'));
      worker.stderr.on('error', () => {});
      // Native diagnostics may contain an invite. Consume them without logging.
      worker.stderr.resume();
      worker.stdout.on('data', (chunk) => {
        if (stopped) return;
        try {
          decoder.push(chunk, (type, payload) => {
            if (stopped) return;
            if (type === 2) {
              if (!packets) throw new Error('Worker not ready');
              if (packets.validate(payload, false).discard) return;
              lastPacket = Date.now();
              send(payload, true);
            } else if (type === 3 && payload.length <= 4096) {
              const event = JSON.parse(payload.toString());
              if (event.type === 'ready' && !packets && event.protocol === 1 &&
                  /^[a-f0-9]{12}$/i.test(event.identifier) && isVirtualAddress(event.address)) {
                packets = new SessionPackets(event.address);
                send(JSON.stringify({ type: 'ready', protocol: 1, identifier: event.identifier, address: event.address }));
              } else if (event.type === 'peer' && packets && event.identifier === hostIdentifier &&
                         typeof event.connected === 'boolean') {
                packets.peer(event.address, event.connected);
                peerConnected = event.connected;
                send(JSON.stringify({ type: 'peer', identifier: event.identifier, address: event.address,
                  connected: event.connected }));
              } else if (event.type === 'error' || event.type === 'closed') {
                stop(1011, 'Native session ended');
              } else throw new Error('Invalid worker event');
            } else throw new Error('Invalid worker record');
          });
        } catch { stop(1011, 'Invalid native session output'); }
      });
      writeWorker(record(1, invite));
    }

    function writeWorker(bytes) {
      if (!worker || stopped || worker.stdin.destroyed) return;
      if (worker.stdin.writableLength + bytes.length > config.maxBufferedBytes)
        return stop(1013, 'Native worker is too slow');
      if (!worker.stdin.write(bytes)) {
        workerPausedAt ||= Date.now();
        ws.pause();
      }
    }

    function send(bytes, binary = false) {
      if (stopped || ws.readyState !== WebSocket.OPEN) return;
      if (ws.bufferedAmount + Buffer.byteLength(bytes) > config.maxBufferedBytes)
        return stop(1013, 'Browser connection is too slow');
      ws.send(bytes, { binary }, (error) => {
        if (error) return stop();
        if (!stopped && ws.bufferedAmount < config.maxBufferedBytes / 4 && socketPausedAt) {
          socketPausedAt = 0;
          worker?.stdout.resume();
        }
      });
      if (ws.bufferedAmount > config.maxBufferedBytes / 2 && worker) {
        socketPausedAt ||= Date.now();
        worker.stdout.pause();
      }
    }

    function stop(code = 1000, reason = 'Session closed') {
      if (stopped) return;
      stopped = true;
      clearInterval(timer); clearInterval(ping);
      if (!worker || worker.exitCode !== null || worker.signalCode !== null) clients.delete(client);
      if (worker) {
        worker.stdin.destroy();
        worker.stdout.destroy();
        worker.kill('SIGTERM');
        const kill = setTimeout(() => { if (worker.exitCode === null && worker.signalCode === null) worker.kill('SIGKILL'); }, 1500);
        kill.unref();
        worker.once('exit', () => clearTimeout(kill));
      }
      if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
        if (code >= 1008 && ws.readyState === WebSocket.OPEN && ws.bufferedAmount < config.maxBufferedBytes / 2)
          ws.send(JSON.stringify({ type: 'error', message: reason }));
        ws.close(code, reason);
        const terminate = setTimeout(() => ws.terminate(), 1000);
        terminate.unref();
        ws.once('close', () => clearTimeout(terminate));
      }
    }
  }

  return {
    server, config,
    listen: () => new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(config.port, config.host, () => { server.off('error', reject); resolve(server.address()); });
    }),
    async close() {
      closing = true;
      for (const client of clients) client.stop(1001, 'Relay shutting down');
      await new Promise((resolve) => wss.close(resolve));
      await new Promise((resolve) => server.close(resolve));
      server.closeAllConnections();
    },
    get sessions() { return clients.size; },
  };
}

export function configFromEnvironment(env = process.env) {
  return {
    host: env.RELAY_HOST || DEFAULTS.host,
    port: Number(env.PORT || env.RELAY_PORT || DEFAULTS.port),
    worker: env.RELAY_WORKER || DEFAULTS.worker,
    allowedOrigins: env.RELAY_ALLOWED_ORIGINS ? env.RELAY_ALLOWED_ORIGINS.split(',').map((s) => s.trim()) : DEFAULTS.allowedOrigins,
    accessToken: env.RELAY_ACCESS_TOKEN || '',
    maxSessions: Number(env.RELAY_MAX_SESSIONS || DEFAULTS.maxSessions),
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const relay = createRelay(configFromEnvironment());
    const address = await relay.listen();
    console.log(`Halo browser relay listening on ${address.address}:${address.port}`);
    for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, () => { relay.close().then(() => process.exit(0)); });
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
