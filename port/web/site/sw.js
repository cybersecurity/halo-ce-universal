/*
SW.JS

The site's service worker:

- it serves every page and script with the headers that make the site
  cross-origin isolated (Cross-Origin-Opener-Policy and
  Cross-Origin-Embedder-Policy), which SharedArrayBuffer, and so the game's
  threads, need; static hosts such as GitHub Pages cannot send them;
- it keeps the site's files for use offline, so the installed web app
  starts without a network. version.json names the build: the files of one
  build are kept together, and a new build replaces them only as a whole
  (when the page asks, after the player agrees to update).
*/

'use strict';

const CACHE_PREFIX = 'halo-web-';
const META_CACHE = 'halo-web-meta';
const SHELL = [
  './',
  'index.html',
  'app.js',
  'input.js',
  'net.js',
  'cache.js',
  'vendor/sha256.js',
  'site-config.js',
  'native-invite.js',
  'gateway.js',
  'style.css',
  'xiso-worker.js',
  'audio-worklet.js',
  'halo.js',
  'halo.wasm',
  'manifest.webmanifest',
  'icons/icon-180.png',
  'icons/icon-192.png',
  'icons/icon-512.png',
  'version.json',
];

async function networkVersion() {
  const response = await fetch('version.json', { cache: 'no-store' });
  return (await response.json()).version;
}

async function activeVersion() {
  const meta = await caches.open(META_CACHE);
  const response = await meta.match('active');
  return response ? (await response.json()).version : null;
}

async function setActiveVersion(version) {
  const meta = await caches.open(META_CACHE);
  await meta.put('active', new Response(JSON.stringify({ version })));
}

// downloads a build's files into a cache of their own, then makes it the
// one served and drops the others
async function installVersion(version) {
  const name = CACHE_PREFIX + version;
  const cache = await caches.open(name);
  await cache.addAll(SHELL.map((path) => new Request(path, { cache: 'no-store' })));
  await setActiveVersion(version);
  for (const other of await caches.keys()) {
    if (other.startsWith(CACHE_PREFIX) && other !== name && other !== META_CACHE) await caches.delete(other);
  }
}

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    try {
      if (!(await activeVersion())) await installVersion(await networkVersion());
    } catch {
      // offline or a partial deployment: files come from the network until
      // a later start caches them
    }
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil(self.clients.claim());
});

function isolated(response) {
  if (!response || response.status === 0 || response.type === 'opaque') return response;
  const headers = new Headers(response.headers);
  headers.set('Cross-Origin-Opener-Policy', 'same-origin');
  headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
  headers.set('Cross-Origin-Resource-Policy', 'same-origin');
  return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
}

async function cachedResponse(request) {
  const version = await activeVersion();
  if (!version) return null;
  const cache = await caches.open(CACHE_PREFIX + version);
  const url = new URL(request.url);
  let response = await cache.match(request, { ignoreSearch: true });
  if (!response && request.mode === 'navigate' && url.pathname.endsWith('/')) {
    response = await cache.match('index.html');
  }
  return response;
}

async function respond(request) {
  const url = new URL(request.url);
  if (url.pathname.endsWith('/version.json') && url.searchParams.has('latest')) {
    // the network's, so the page can tell a new build is out
    return isolated(await fetch('version.json', { cache: 'no-store' }));
  }
  const cached = await cachedResponse(request);
  if (cached) return isolated(cached);
  try {
    return isolated(await fetch(request));
  } catch (error) {
    if (request.mode === 'navigate') {
      const shell = await cachedResponse(new Request('index.html'));
      if (shell) return isolated(shell);
    }
    throw error;
  }
}

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET') return;
  if (new URL(request.url).origin !== self.location.origin) return;
  event.respondWith(respond(request));
});

self.addEventListener('message', (event) => {
  if (event.data === 'update') {
    event.waitUntil((async () => {
      let message = 'updated';
      try {
        await installVersion(await networkVersion());
      } catch {
        message = 'update-failed';
      }
      if (event.source) event.source.postMessage(message);
    })());
  }
});
