/*
APP.JS

The page of the web port (port/web/README.md): it checks that the browser
can run the game, copies the game data out of the player's disc image
(xiso-worker.js), then starts the game (halo.js and halo.wasm, built by
`ninja web`) and serves it on the main thread:

- each frame the game's thread posts as an ImageBitmap or RGBA pixel buffer
  goes onto the canvas (Module.haloPresent);
- each animation frame advances a counter the game waits on, polls the
  controllers (input.js) and gives the game the canvas's size;
- the game's sound plays through an AudioWorklet (audio-worklet.js).
*/

'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  // the WebAssembly memory the build expects (tools/web_build.py
  // WEB_MEMORY_BYTES): the Xbox memory window ends at 0x88000000
  const MEMORY_PAGES = 0x88000000 / 65536;
  const REQUIRED_BYTES = 2.1e9;
  const DEFAULT_ROOM = window.HALO_BROWSER_CONFIG?.defaultRoom ?? 'FQLX01';
  const QUICK_MAPS = ['ui.map', 'beavercreek.map'];
  const diagnosticOptions = new URLSearchParams(location.search);
  const ios = /iPhone|iPad|iPod/.test(navigator.userAgent) ||
    (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  const pixelFrames = diagnosticOptions.get('frame_transport') === 'rgba' ||
    (diagnosticOptions.get('frame_transport') !== 'bitmap' &&
      /Macintosh/.test(navigator.userAgent) && /Chrome|Chromium|Edg\//.test(navigator.userAgent));

  const state = {
    memory: null,
    audio: null,
    started: false,
    shared: null,
    offsets: null,
    log: [],
    wakeLock: null,
    version: null,
    invite: null,
    gateway: null,
    gatewayInstalled: false,
    inviteConnecting: false,
    inviteAttempt: 0,
    nativeInstalling: false,
    pendingInviteConnect: null,
    roomTask: Promise.resolve(),
    maps: null,
    dataBusy: false,
    dataTransition: false,
    releaseGameLock: null,
    checksReady: false,
    manualMode: new URLSearchParams(location.search).get('menu') === '1',
    manualRequested: false,
    selectedRoom: null,
    quickController: null,
    quickFailed: false,
    quickRole: null,
    quickPhase: null,
    audioStarted: false,
    roomSettingsOpen: false,
  };

  // ---------- settings (this browser's; nothing else depends on them)

  const coarsePointer = matchMedia('(pointer: coarse)').matches;
  const settings = { touch: coarsePointer, touchFps: false, look: 1.4, vsync: true, glDebug: false, renderHeight: ios || pixelFrames ? 480 : 720 };
  try {
    Object.assign(settings, JSON.parse(localStorage.getItem('halo-web-settings') || '{}'));
  } catch { /* private browsing: the defaults */ }
  if (![480, 720, 1080, 1440].includes(Number(settings.renderHeight))) settings.renderHeight = ios || pixelFrames ? 480 : 720;

  function saveSettings() {
    try { localStorage.setItem('halo-web-settings', JSON.stringify(settings)); } catch { /* not kept */ }
  }

  // ---------- log

  function log(line) {
    const text = String(line);
    state.log.push(text);
    if (state.log.length > 2000) state.log.splice(0, state.log.length - 2000);
    console.log(text);
  }

  async function debugFile() {
    try {
      // WasmFS mounts OPFS at /data; reused Apollo maps run from /data/halo/data.
      const directories = (state.maps?.dataRoot || '/data').split('/').filter(Boolean);
      if (directories.shift() !== 'data') return '';
      let root = await navigator.storage.getDirectory();
      for (const directory of directories) root = await root.getDirectoryHandle(directory);
      return await (await root.getFileHandle('debug.txt')).getFile();
    } catch {
      return null;
    }
  }

  async function debugText() {
    const file = await debugFile();
    if (!file) return '';
    if (!file.stream || file.size <= 200000 || file.size === undefined) return file.text();
    // Scan the entire file in bounded chunks. Timeout spam must not erase
    // the earlier disconnect/adoption receipts needed to diagnose a match.
    const reader = file.stream().getReader(), decoder = new TextDecoder();
    let carry = '', head = '', tail = '', important = '', timeouts = 0, firstTimeout = '', lastTimeout = '';
    const collect = line => {
      if (head.length < 5000) head = (head + line + '\n').slice(0, 5000);
      tail = (tail + line + '\n').slice(-50000);
      if (line.includes('timeout in network_connection_idle')) {
        if (!timeouts) firstTimeout = line;
        lastTimeout = line; timeouts++;
      } else if (!line.includes('network_connection_idle() failed in network_game_client_idle_ingame()') &&
          /migration|reattach|adopted the live match|checkpoint|epoch|out.of.sync|connection.*(?:fail|lost|timed|went)|machine.*(?:remov|accept|connect)/i.test(line)) {
        important = (important + line + '\n').slice(-140000);
      }
    };
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        carry += decoder.decode(value, { stream: true });
        let end;
        while ((end = carry.indexOf('\n')) >= 0) { collect(carry.slice(0, end)); carry = carry.slice(end + 1); }
        if (carry.length > 200000) carry = carry.slice(-200000);
      }
      carry += decoder.decode();
      if (carry) collect(carry);
    } finally { reader.releaseLock(); }
    return `Scanned ${file.size} bytes; selected connection events and recent tail below.\n` +
      `--- start ---\n${head}\n--- connection events ---\n${important}` +
      (timeouts ? `\nConnection idle timeouts: ${timeouts}\nFirst: ${firstTimeout}\nLast: ${lastTimeout}\n` : '') +
      `\n--- recent tail ---\n${tail}`;
  }

  async function fullLog() {
    const debug = await debugText();
    return `${navigator.userAgent}\n\n--- page and console ---\n${state.log.join('\n')}` +
      (debug ? `\n\n--- debug.txt ---\n${debug}` : '');
  }

  function toast(text, milliseconds = 3500) {
    const element = $('toast');
    element.textContent = text;
    element.hidden = false;
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => { element.hidden = true; }, milliseconds);
  }

  async function fatal(text) {
    log('fatal: ' + text);
    $('fatal-text').textContent = text;
    $('fatal').hidden = false;
    document.body.classList.remove('playing');
  }

  // ---------- checks

  function addCheck(ok, text, level) {
    const element = document.createElement('div');
    element.className = 'check' + (ok ? '' : level === 'warn' ? ' warn' : ' bad');
    element.textContent = text;
    $('checks').appendChild(element);
    return ok;
  }

  async function ensureIsolation() {
    if (!('serviceWorker' in navigator)) return;
    try {
      await navigator.serviceWorker.register('sw.js');
    } catch (error) {
      log('service worker: ' + error);
      return;
    }
    if (window.crossOriginIsolated) return;
    await navigator.serviceWorker.ready;
    // the service worker serves the page with the isolation headers from
    // its next load on
    let reloaded = false;
    try { reloaded = sessionStorage.getItem('halo-web-isolation') === '1'; } catch { /* none */ }
    if (!reloaded) {
      try { sessionStorage.setItem('halo-web-isolation', '1'); } catch { /* none */ }
      location.reload();
      await new Promise(() => {});
    }
  }

  function webgl2InWorkers() {
    try {
      return typeof OffscreenCanvas !== 'undefined' && !!new OffscreenCanvas(1, 1).getContext('webgl2');
    } catch {
      return false;
    }
  }

  async function checkGameStorage() {
    if (!navigator.storage?.getDirectory) {
      return addCheck(false, 'Game storage is unavailable in this browser. Open this page in an up-to-date Safari or Chrome.');
    }
    try {
      // Safari exposes this API even when it refuses OPFS access (for
      // example in Private Browsing). Test access before reserving memory.
      await navigator.storage.getDirectory();
      return addCheck(true, 'Game file storage (OPFS)');
    } catch (error) {
      log('game storage: ' + (error?.stack || error));
      return addCheck(false, 'Game storage could not be opened. In Safari, open this link in a regular tab ' +
        'instead of Private Browsing. If you are already in a regular tab, close other Halo tabs and restart Safari.');
    }
  }

  async function runChecks() {
    let ok = true;
    ok = addCheck(window.crossOriginIsolated && typeof SharedArrayBuffer !== 'undefined',
      window.crossOriginIsolated ? 'Threads (SharedArrayBuffer)' :
        'Threads: this page is not cross-origin isolated. Reload it; if this stays, the browser is too old.') && ok;
    ok = addCheck(webgl2InWorkers(), 'WebGL 2 from a worker (OffscreenCanvas; iOS 17 or later)') && ok;
    ok = await checkGameStorage() && ok;
    // Reserve the Xbox address window only after Play owns the game lock.
    // Downloads, idle launchers and a second tab need no game-sized memory.
    ok = addCheck(typeof WebAssembly !== 'undefined' && typeof WebAssembly.Memory === 'function', 'WebAssembly memory') && ok;
    if (navigator.storage && navigator.storage.estimate) {
      try {
        const estimate = await navigator.storage.estimate();
        const free = (estimate.quota || 0) - (estimate.usage || 0);
        $('storage-summary').textContent = `Storage: ${(estimate.usage / 1e9).toFixed(2)} GB used of ` +
          `${(estimate.quota / 1e9).toFixed(1)} GB this site may use.`;
        state.freeBytes = free;
      } catch { /* unknown */ }
    }
    return ok;
  }

  // ---------- game data

  function requiredMaps() {
    // Desktop invites can name any map. Only browser quick play fixes the
    // match to Beaver Creek; the full menu retains all supported scenarios.
    return !state.manualMode && state.selectedRoom && !state.invite ? QUICK_MAPS : HaloCache.expected;
  }

  function hasFullMaps() {
    return !!state.maps && HaloCache.expected.every(name => state.maps.files.includes(name));
  }

  async function mapsState() {
    return HaloCache.mapsState({ required: requiredMaps() });
  }

  function updatePlayButton() {
    const automatic = !state.manualMode && !!(state.invite || state.selectedRoom);
    $('play').hidden = automatic;
    $('play').textContent = 'Play main menu';
    $('play').disabled = state.started || (!automatic && (!state.maps || state.dataBusy));
  }

  function showSteps(maps) {
    state.maps = maps;
    $('step-data').hidden = !!maps;
    $('step-play').hidden = !maps;
    if (maps) {
      $('data-summary').textContent = `Game data: ${maps.files.length} maps, ${(maps.bytes / 1e9).toFixed(2)} GB.`;
    }
    updatePlayButton();
    connectPendingInvite();
    maybeQuickPlay();
  }

  function connectPendingInvite() {
    if (hasFullMaps() && !state.dataBusy && state.pendingInviteConnect && !state.dataTransition) {
      const connect = state.pendingInviteConnect;
      state.pendingInviteConnect = null;
      connect();
    }
  }

  function setDataBusy(busy) {
    state.dataBusy = busy;
    $('iso-file').disabled = busy;
    $('delete-data').disabled = busy;
    updatePlayButton();
    connectPendingInvite();
    maybeQuickPlay();
  }

  function extract(file) {
    return new Promise((resolve, reject) => {
      const worker = new Worker('xiso-worker.js');
      const started = Date.now();
      $('progress').hidden = false;
      worker.onmessage = (event) => {
        const message = event.data;
        if (message.type === 'progress') {
          const fraction = message.total ? message.done / message.total : 0;
          $('progress-fill').style.width = (fraction * 100).toFixed(1) + '%';
          const seconds = (Date.now() - started) / 1000;
          const rate = message.done / Math.max(seconds, 0.1);
          const left = rate > 0 ? (message.total - message.done) / rate : 0;
          $('progress-text').textContent = `Copying maps/${message.file}: ` +
            `${(message.done / 1e9).toFixed(2)} of ${(message.total / 1e9).toFixed(2)} GB` +
            (seconds > 3 ? `, about ${Math.ceil(left / 60)} min left` : '');
        } else if (message.type === 'done') {
          worker.terminate();
          resolve(message);
        } else if (message.type === 'error') {
          worker.terminate();
          reject(new Error(message.message));
        }
      };
      worker.onerror = (event) => {
        worker.terminate();
        reject(new Error(event.message || 'The copy stopped.'));
      };
      worker.postMessage({ file });
    });
  }

  async function onImageChosen(event) {
    const file = event.target.files && event.target.files[0];
    event.target.value = '';
    if (!file) return;
    if (state.freeBytes !== undefined && state.freeBytes < REQUIRED_BYTES) {
      toast('There may not be enough free storage for the game data (about 1.8 GB).', 6000);
    }
    if (navigator.storage && navigator.storage.persist) {
      // keep the data when the device runs low on space
      navigator.storage.persist().catch(() => {});
    }
    setDataBusy(true);
    try {
      const result = await HaloCache.withLock(() => extract(file));
      log(`extracted ${result.files} files, ${result.bytes} bytes`);
      showSteps(await mapsState());
      $('progress-text').textContent = state.maps ? 'Done. Maps are ready to play.' :
        'This disc image does not contain the maps needed for this mode.';
    } catch (error) {
      $('progress-text').textContent = error.message;
      log('extraction failed: ' + error.message);
    } finally {
      setDataBusy(false);
    }
  }

  // ---------- saved games: a .zip of the save folder (stored, not compressed)

  const crcTable = (() => {
    const table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      table[n] = c >>> 0;
    }
    return table;
  })();

  function crc32(bytes) {
    let crc = 0xFFFFFFFF;
    for (let i = 0; i < bytes.length; i++) crc = crcTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8);
    return (crc ^ 0xFFFFFFFF) >>> 0;
  }

  function zip(files) {
    const encoder = new TextEncoder();
    const parts = [];
    const central = [];
    let offset = 0;
    for (const { name, bytes } of files) {
      const nameBytes = encoder.encode(name);
      const crc = crc32(bytes);
      const local = new DataView(new ArrayBuffer(30));
      local.setUint32(0, 0x04034b50, true);
      local.setUint16(4, 20, true);
      local.setUint32(14, crc, true);
      local.setUint32(18, bytes.length, true);
      local.setUint32(22, bytes.length, true);
      local.setUint16(26, nameBytes.length, true);
      parts.push(new Uint8Array(local.buffer), nameBytes, bytes);
      const entry = new DataView(new ArrayBuffer(46));
      entry.setUint32(0, 0x02014b50, true);
      entry.setUint16(4, 20, true);
      entry.setUint16(6, 20, true);
      entry.setUint32(16, crc, true);
      entry.setUint32(20, bytes.length, true);
      entry.setUint32(24, bytes.length, true);
      entry.setUint16(28, nameBytes.length, true);
      entry.setUint32(42, offset, true);
      central.push(new Uint8Array(entry.buffer), nameBytes);
      offset += 30 + nameBytes.length + bytes.length;
    }
    const centralSize = central.reduce((sum, part) => sum + part.length, 0);
    const end = new DataView(new ArrayBuffer(22));
    end.setUint32(0, 0x06054b50, true);
    end.setUint16(8, files.length, true);
    end.setUint16(10, files.length, true);
    end.setUint32(12, centralSize, true);
    end.setUint32(16, offset, true);
    return new Blob([...parts, ...central, new Uint8Array(end.buffer)], { type: 'application/zip' });
  }

  async function exportSaves() {
    const files = [];
    async function walk(directory, prefix) {
      for await (const [name, handle] of directory.entries()) {
        if (handle.kind === 'directory') await walk(handle, prefix + name + '/');
        else files.push({ name: prefix + name, bytes: new Uint8Array(await (await handle.getFile()).arrayBuffer()) });
      }
    }
    try {
      const maps = state.maps || await mapsState();
      const savePath = (maps?.saveRoot || '/data/save').split('/').slice(2);
      await walk(await HaloCache.directory(savePath), 'save/');
      try {
        const dataPath = (maps?.dataRoot || '/data').split('/').slice(2);
        const config = await (await (await HaloCache.directory(dataPath)).getFileHandle('config.toml')).getFile();
        files.push({ name: 'config.toml', bytes: new Uint8Array(await config.arrayBuffer()) });
      } catch { /* none yet */ }
    } catch {
      toast('There are no saved games yet.');
      return;
    }
    const link = document.createElement('a');
    link.href = URL.createObjectURL(zip(files));
    link.download = 'halo-saves.zip';
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 60000);
  }

  async function deleteData() {
    if (state.dataBusy || state.started) return;
    if (!confirm('Delete the game data (the maps folder)? Saved games are kept.')) return;
    try {
      await HaloCache.withLock(async () => {
        for (const path of [[], ['halo', 'data']]) {
          let folder;
          try { folder = await HaloCache.directory(path); }
          catch (error) { if (error.name === 'NotFoundError') continue; throw error; }
          for (const name of ['maps', 'maps.json']) {
            try { await folder.removeEntry(name, { recursive: true }); }
            catch (error) { if (error.name !== 'NotFoundError') throw error; }
          }
        }
      });
    } catch (error) { toast(error.message, 6000); return; }
    showSteps(await mapsState());
    setDataBusy(false);
  }

  // ---------- the running game

  function quickStatus(phase, message) {
    state.quickPhase = phase;
    $('quick-status').textContent = message;
    $('quick-game-status').textContent = message;
    $('quick-panel').dataset.state = phase;
    $('quick-panel').hidden = !state.started || phase === 'playing' || phase === 'menu';
    $('quick-retry').hidden = phase !== 'error' || state.started;
  }

  function cancelQuickPlay() {
    state.quickController?.abort();
    state.quickController = null;
    HaloNet.cancelQuickPlay();
  }

  function showRoomSettings(open) {
    state.roomSettingsOpen = open;
    document.body.classList[open ? 'add' : 'remove']('room-settings');
    $('room-toggle').hidden = open || !state.started || !!state.invite;
    $('room-close').hidden = !open;
    HaloInput.setUIActive(open);
    if (open && document.pointerLockElement) document.exitPointerLock();
  }

  async function maybeQuickPlay() {
    if (!state.checksReady || state.started || state.dataBusy || state.dataTransition || !state.maps) return;
    if (state.manualMode) {
      if (state.manualRequested) {
        state.manualRequested = false;
        await play({ userGesture: false });
      }
      return;
    }
    if (state.quickController || state.quickFailed) return;
    if (state.invite) {
      if (!hasFullMaps()) return;
      if (!state.gatewayInstalled || !state.gateway?.connected) {
        quickStatus('waiting', 'Waiting for the invited host…');
        return;
      }
      state.quickRole = 'join';
      quickStatus('joining', 'Joining the invited match…');
      await play({ role: 'join', target: state.gateway.hostAddress });
      return;
    }
    const room = HaloNet.status().room;
    if (!room || room !== state.selectedRoom) return;
    const controller = new AbortController();
    state.quickController = controller;
    quickStatus('waiting', 'Connecting to the room and finding a match…');
    try {
      const selection = await HaloNet.quickPlay({ signal: controller.signal, onStatus(status) {
        if (controller.signal.aborted) return;
        log(`network ${new Date().toISOString()}: ${status.state}: ${status.message}`);
        if (typeof status.hold === 'boolean') window.Module?._web_quick_play_hold?.(status.hold ? 1 : 0);
        if (!controller.signal.aborted && (!state.started || ['recovering', 'reconnecting', 'error'].includes(status.state)))
          quickStatus(status.state, status.message);
      }, onReconnect(selection) {
        if (controller.signal.aborted || state.manualMode || state.invite || !state.started ||
            selection.room !== state.selectedRoom) return;
        const reconnect = window.Module?._web_quick_play_reconnect;
        if (!reconnect) {
          quickStatus('error', 'Player reconnection needs the latest game build. Reload and choose Update.');
          return;
        }
        log(`network ${new Date().toISOString()}: client reattach, epoch ${selection.epoch}, host ${HaloNet.addressText(selection.hostAddress)}`);
        quickStatus('reconnecting', 'Reconnecting your player. The match continues on the current host…');
        reconnect(selection.hostAddress, selection.epoch);
      }, onFailover(selection) {
        if (controller.signal.aborted || state.manualMode || state.invite ||
            !state.started || selection.room !== state.selectedRoom) return;
        log(`network ${new Date().toISOString()}: replacement ${selection.role}, epoch ${selection.epoch}, host ${HaloNet.addressText(selection.hostAddress)}`);
        const migrate = window.Module?._web_quick_play_migrate;
        if (!migrate) {
          quickStatus('error', 'Host recovery needs the latest game build. Reload and choose Update.');
          cancelQuickPlay();
          return;
        }
        state.quickRole = selection.role;
        quickStatus('recovering', selection.role === 'host' ?
          'You are the replacement host. Preserving the match…' : 'Reconnecting to the replacement host. Preserving the match…');
        migrate(selection.role === 'host' ? 1 : 2, selection.hostAddress, selection.epoch);
      } });
      if (controller.signal.aborted || state.manualMode || state.started || state.selectedRoom !== selection.room) return;
      state.quickRole = selection.role;
      state.quickEpoch = selection.epoch;
      quickStatus(selection.role === 'host' ? 'hosting' : 'joining', selection.role === 'host' ?
        'Starting a match. Keep this game open so others can join.' : 'Joining the room’s match…');
      await play({ role: selection.role, target: selection.hostAddress });
    } catch (error) {
      if (controller.signal.aborted) return;
      state.quickFailed = true;
      cancelQuickPlay();
      quickStatus('error', error.message || 'Could not connect to a match. Retry or open the main menu.');
    }
  }

  function openMainMenu() {
    if (state.roomSettingsOpen) showRoomSettings(false);
    const unrecoverable = !$('fatal').hidden;
    state.inviteAttempt++;
    state.pendingInviteConnect = null;
    state.inviteConnecting = false;
    state.manualMode = true;
    state.manualRequested = true;
    state.quickFailed = false;
    cancelQuickPlay();
    quickStatus('menu', 'Main menu selected.');
    updatePlayButton();
    unlockInteraction();
    if (!hasFullMaps()) {
      // The running engine owns the OPFS game lock. Reload before importing
      // maps for the full menu.
      state.dataTransition = true;
      const url = new URL(location.href);
      url.searchParams.set('menu', '1');
      url.hash = '';
      location.href = url.toString();
      return;
    }
    if (state.started || state.nativeInstalling) {
      if (!unrecoverable && !state.nativeInstalling && window.Module?._web_quick_play_cancel) window.Module._web_quick_play_cancel();
      else {
        state.dataTransition = true;
        const url = new URL(location.href);
        url.searchParams.set('menu', '1');
        location.href = url.toString();
      }
    } else if (state.maps && !state.dataBusy) {
      state.manualRequested = false;
      return play({ userGesture: true });
    } else maybeQuickPlay();
  }

  function makeAudioContext() {
    if (state.audio) return;
    try {
      const AudioContextClass = window.AudioContext || window.webkitAudioContext;
      state.audio = new AudioContextClass({ sampleRate: 48000, latencyHint: 'interactive' });
    } catch (error) { log('audio context: ' + error); }
  }

  function unlockInteraction(userGesture = true) {
    makeAudioContext();
    state.audio?.resume().catch(() => {});
    if (state.started && userGesture) {
      const root = document.documentElement;
      if (root.requestFullscreen && !navigator.standalone) root.requestFullscreen({ navigationUI: 'hide' }).catch(() => {});
      if (screen.orientation?.lock) screen.orientation.lock('landscape').catch(() => {});
      const canvas = $('screen');
      if (canvas.requestPointerLock && matchMedia('(pointer: fine)').matches) {
        try { canvas.requestPointerLock()?.catch(() => {}); } catch { /* unsupported */ }
      }
    }
    if (state.shared && !state.audioStarted) startAudio();
  }

  function landscapeSize() {
    const scale = Math.min(window.devicePixelRatio || 1, 2);
    const long = Math.max(window.innerWidth, window.innerHeight);
    const short = Math.min(window.innerWidth, window.innerHeight);
    let width = Math.round(long * scale);
    let height = Math.round(short * scale);
    // Bound render targets and transferred frame surfaces independently of
    // Retina/4K desktop size. Settings and diagnostics can opt into more detail.
    const requestedHeight = Number(diagnosticOptions.get('render_height'));
    const maximumHeight = Number.isFinite(requestedHeight) && requestedHeight > 0 ?
      Math.max(480, Math.min(1440, Math.round(requestedHeight))) : Number(settings.renderHeight);
    if (height > maximumHeight) {
      width = Math.round(width * maximumHeight / height);
      height = maximumHeight;
    }
    return { width: width & ~1, height: height & ~1 };
  }

  function presentationStats(canvas) {
    if (diagnosticOptions.get('fps') !== '1') return null;
    const output = document.createElement('output');
    output.id = 'performance-stats';
    output.style.cssText = 'position:fixed;top:max(8px,env(safe-area-inset-top));' +
      'left:max(8px,env(safe-area-inset-left));z-index:20;padding:4px 8px;' +
      'font:13px monospace;background:#11161ddd;border-radius:6px;pointer-events:none';
    output.textContent = 'FPS —';
    output.title = 'Frames received from the game worker';
    output.dataset.samples = '[]';
    document.body.appendChild(output);
    const samples = [];
    let frames = 0, previousFrames = 0, previousTime = performance.now();
    const resetWindow = () => {
      previousFrames = frames;
      previousTime = performance.now();
    };
    // Neither hidden time nor queued frames delivered while hidden belong
    // in a visible FPS sample.
    document.addEventListener('visibilitychange', resetWindow);
    setInterval(() => {
      if (document.hidden || !frames) { resetWindow(); return; }
      const now = performance.now();
      const windowMs = now - previousTime;
      if (windowMs < 1000) return;
      const fps = (frames - previousFrames) * 1000 / windowMs;
      output.textContent = `${fps.toFixed(1)} FPS`;
      output.title = `${canvas.width} × ${canvas.height}; frames received from the game worker`;
      samples.push({ fps: +fps.toFixed(2), frames, ms: Math.round(now),
        windowMs: +windowMs.toFixed(2), width: canvas.width, height: canvas.height });
      if (samples.length > 120) samples.shift();
      output.dataset.samples = JSON.stringify(samples);
      previousFrames = frames;
      previousTime = now;
    }, 1000);
    return () => {
      if (!frames) previousTime = performance.now();
      frames++;
    };
  }

  function sharedWord(name) {
    return (state.shared + state.offsets[name]) >> 2;
  }

  function readOffsets(module) {
    const pointer = module._web_shared_offsets();
    const words = new Int32Array(state.memory.buffer, pointer, 36);
    const names = ['size', 'eventWrite', 'eventRead', 'events', 'eventSize', 'gamepads', 'gamepadSize',
      'displayWidth', 'displayHeight', 'frameCounter', 'framesPresented', 'vsync', 'audioRate', 'audioOpen',
      'audioWrite', 'audioRead', 'audioUnderruns', 'audioRing', 'audioRingFrames', 'pageHidden', 'gameStarted',
      'eventCapacity', 'gamepadCount', 'netLocalAddress', 'netOutWrite', 'netOutRead', 'netInWrite', 'netInRead',
      'netOut', 'netOutBytes', 'netIn', 'netInBytes', 'gatewayEnabled', 'gatewayIdentifier',
      'gatewayPeers', 'gatewayPeerCount'];
    const offsets = {};
    names.forEach((name, index) => { offsets[name] = words[index]; });
    // Cached older runtimes export only the original 36 offsets. Their shared
    // size must include the appended table before reading its extra offsets.
    const pingStart = offsets.gatewayPeers + offsets.gatewayPeerCount * 12;
    if (offsets.size >= pingStart + 20 + 128 * 8) {
      const pingNames = ['pingSequence', 'pingHost', 'pingEpoch', 'pingUpdated', 'pingCount', 'pingPeers', 'pingPeerCount'];
      const pingWords = new Int32Array(state.memory.buffer, pointer + 36 * 4, pingNames.length);
      pingNames.forEach((name, index) => { offsets[name] = pingWords[index]; });
    }
    return offsets;
  }

  function updateDisplaySize() {
    const i32 = new Int32Array(state.memory.buffer);
    const { width, height } = landscapeSize();
    Atomics.store(i32, sharedWord('displayWidth'), width);
    Atomics.store(i32, sharedWord('displayHeight'), height);
    $('rotate').hidden = !(state.started && window.innerHeight > window.innerWidth);
  }

  function animationFrame() {
    const i32 = new Int32Array(state.memory.buffer);
    Atomics.add(i32, sharedWord('frameCounter'), 1);
    Atomics.notify(i32, sharedWord('frameCounter'));
    HaloInput.pollGamepads();
    requestAnimationFrame(animationFrame);
  }

  function onVisibility() {
    if (!state.shared) return;
    const i32 = new Int32Array(state.memory.buffer);
    const hidden = document.hidden ? 1 : 0;
    Atomics.store(i32, sharedWord('pageHidden'), hidden);
    Atomics.notify(i32, sharedWord('frameCounter'));
    if (state.audio) {
      if (hidden) state.audio.suspend().catch(() => {});
      else state.audio.resume().catch(() => {});
    }
    if (!hidden) requestWakeLock();
  }

  async function requestWakeLock() {
    try {
      if (navigator.wakeLock && !document.hidden) state.wakeLock = await navigator.wakeLock.request('screen');
    } catch { /* not allowed now */ }
  }

  async function startAudio() {
    const context = state.audio;
    if (!context || state.audioStarted) return;
    state.audioStarted = true;
    try {
      await context.audioWorklet.addModule('audio-worklet.js');
      const i32 = new Int32Array(state.memory.buffer);
      const node = new AudioWorkletNode(context, 'halo-audio', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [2],
        processorOptions: {
          buffer: state.memory.buffer,
          ring: state.shared + state.offsets.audioRing,
          ringFrames: state.offsets.audioRingFrames,
          write: state.shared + state.offsets.audioWrite,
          read: state.shared + state.offsets.audioRead,
          underruns: state.shared + state.offsets.audioUnderruns,
          rate: 48000,
        },
      });
      node.connect(context.destination);
      Atomics.store(i32, sharedWord('audioOpen'), 1);
      log(`audio: ${context.sampleRate} Hz`);
    } catch (error) {
      state.audioStarted = false;
      log('audio: ' + error);
      toast('Sound is not available: ' + error.message);
    }
  }

  async function play({ role = null, target = null, userGesture = false } = {}) {
    if (state.started || state.dataBusy || state.dataTransition || !state.maps) return;
    if (role && state.invite && (!state.gatewayInstalled || !state.gateway?.connected)) {
      quickStatus('waiting', 'Waiting for the invited host…');
      return;
    }
    state.started = true;
    unlockInteraction(userGesture);
    try {
      if (navigator.locks) {
        await new Promise((resolve, reject) => {
          navigator.locks.request('halo-game', { ifAvailable: true }, lock => {
            if (!lock) { reject(new Error('Halo is open in another tab. Close it, then press Play.')); return; }
            resolve();
            return new Promise(release => { state.releaseGameLock = release; });
          }).catch(reject);
        });
      }
    } catch (error) {
      state.started = false;
      state.audio?.close().catch(() => {});
      state.audio = null;
      state.quickFailed = true;
      cancelQuickPlay();
      quickStatus('error', error.message);
      updatePlayButton();
      return;
    }
    if (state.dataTransition) {
      state.releaseGameLock?.();
      state.releaseGameLock = null;
      state.started = false;
      return;
    }
    try {
      state.memory = new WebAssembly.Memory({ initial: MEMORY_PAGES, maximum: MEMORY_PAGES, shared: true });
    } catch (error) {
      state.releaseGameLock?.();
      state.releaseGameLock = null;
      state.started = false;
      state.audio?.close().catch(() => {});
      state.audio = null;
      state.quickFailed = true;
      cancelQuickPlay();
      quickStatus('error', 'The browser could not reserve memory for Halo. Close other Halo tabs or apps and retry.');
      log('memory: ' + (error?.message || error));
      updatePlayButton();
      return;
    }
    updatePlayButton();
    document.body.classList.add('playing');
    $('room-toggle').hidden = !!state.invite;
    $('quick-panel').hidden = !role;
    requestWakeLock();
    // the system's back gesture or button (Android) backs out of menus, as
    // the controller's B does, instead of leaving the game
    history.pushState({ playing: true }, '');
    window.addEventListener('popstate', () => {
      HaloInput.pressBack();
      history.pushState({ playing: true }, '');
    });

    const canvas = $('screen');
    const context = canvas.getContext(pixelFrames ? '2d' : 'bitmaprenderer', { alpha: false });
    const countPresent = presentationStats(canvas);
    // (tests pass extra --NAME=value settings in window.__haloArgs)
    const argumentsList = Array.isArray(window.__haloArgs) ? window.__haloArgs.slice() : [];
    argumentsList.push('--HALO_DATA_ROOT=' + state.maps.dataRoot, '--HALO_SAVE_ROOT=' + state.maps.saveRoot);
    argumentsList.push('--HALO_WEB_PRESENT_ACK=1');
    if (pixelFrames) argumentsList.push('--HALO_WEB_PIXEL_FRAMES=1');
    if (role) {
      argumentsList.push('--HALO_QUICK_PLAY=' + role);
      if (role === 'join' && target) argumentsList.push('--HALO_QUICK_PLAY_TARGET=' + HaloNet.addressText(target));
    }
    if (diagnosticOptions.get('batch_streams') === '0') argumentsList.push('--HALO_WEB_BATCH_STREAMS=0');
    if (diagnosticOptions.get('geometry_cache') === '1') argumentsList.push('--HALO_WEB_GEOMETRY_CACHE=1');
    if (!settings.vsync) argumentsList.push('--HALO_NO_VSYNC=1');
    if (settings.glDebug) argumentsList.push('--HALO_GL_DEBUG=1');

    window.Module = {
      wasmMemory: state.memory,
      arguments: argumentsList,
      print: (text) => log(text),
      printErr: (text) => log(text),
      haloPresent: (bitmap, pendingBuffer) => {
        try {
          // A bitmap sent just before the tab was hidden may arrive after
          // rendering stops. Release it without updating the hidden canvas.
          if (document.hidden) return;
          if (canvas.width !== bitmap.width || canvas.height !== bitmap.height) {
            canvas.width = bitmap.width;
            canvas.height = bitmap.height;
          }
          if (bitmap.pixels) {
            context.putImageData(new ImageData(new Uint8ClampedArray(bitmap.pixels), bitmap.width, bitmap.height), 0, 0);
          } else if (pixelFrames) {
            // An older cached runtime can still send ImageBitmaps to a newer
            // launcher until its offered update is applied.
            context.drawImage(bitmap, 0, 0);
          } else context.transferFromImageBitmap(bitmap);
          countPresent?.();
        } finally {
          try { bitmap.close?.(); }
          finally {
            // Older runtimes send only the bitmap; newer ones bound their
            // transfer queue with this shared acknowledgement counter.
            if (pendingBuffer) Atomics.sub(new Int32Array(pendingBuffer), 0, 1);
          }
        }
      },
      haloMessage: (kind, text) => {
        if (kind === 0) log('game: ' + text);
        else if (kind === 6) {
          try {
            const status = JSON.parse(text);
            if (status.phase === 'checkpoint') {
              HaloNet.quickPlayCheckpoint(status);
              return;
            }
            log(`network ${new Date().toISOString()}: native ${status.phase}: ${status.message}`);
            if (status.phase === 'disconnected' && !state.invite && !state.manualMode && HaloNet.quickPlayLost()) {
              quickStatus('recovering', 'Restoring the connection while preserving the match…');
              return;
            }
            const recovering = HaloNet.quickPlayPhase(status.phase);
            if (recovering) {
              quickStatus('recovering', 'Choosing a replacement host and preserving the match…');
              return;
            }
            quickStatus(status.phase, status.message);
            if (status.phase === 'menu' || status.phase === 'error' || status.phase === 'disconnected') {
              state.quickFailed = status.phase === 'error';
              state.manualMode = true;
              cancelQuickPlay();
              if (!hasFullMaps()) {
                if (status.phase === 'menu') openMainMenu();
                else fatal(status.message + ' Reload to retry multiplayer, or choose Main menu and import your disc image.');
              }
            }
          } catch (error) { log('quick play status: ' + error); }
        }
        else if (kind === 1) toast(text, 5000);
        else if (kind === 2) log('clipboard: ' + text);
        else if (kind === 4) log('thread error: ' + text);
        else if (kind === 5) {
          log('game: ' + text);
          $('fatal').querySelector('h2').textContent = 'The game quit';
          fatal(text + ' Reload to start it again.');
        }
        else fatal(text);
      },
      onAbort: (what) => fatal('The game stopped: ' + what),
      onRuntimeInitialized: () => {
        const module = window.Module;
        module._web_quick_play_set_address?.(HaloNet.address);
        module._web_quick_play_set_epoch?.(state.quickEpoch || 0);
        state.shared = module._web_shared_state();
        state.offsets = readOffsets(module);
        onVisibility();
        updateDisplaySize();
        HaloInput.attach({
          memory: state.memory,
          base: state.shared,
          offsets: state.offsets,
          canvas,
          touchRoot: $('touch'),
          touch: settings.touch,
          touchFps: settings.touchFps === true,
        });
        HaloInput.setLookSensitivity(settings.look);
        HaloNet.attach({ memory: state.memory, base: state.shared, offsets: state.offsets });
        if (role && !state.invite) HaloNet.quickPlayStarted();
        $('touch').hidden = !settings.touch;
        requestAnimationFrame(animationFrame);
        startAudio();
        log('runtime ready');
      },
    };

    const script = document.createElement('script');
    script.src = 'halo.js';
    script.onerror = () => fatal('Could not load halo.js.');
    document.body.appendChild(script);
  }

  // ---------- online play (net.js)

  function setUpInvites() {
    if (!$('invite-input')) return;
    const relay = window.HALO_BROWSER_CONFIG?.relayUrl || '';
    const linked = new URLSearchParams(location.hash.slice(1)).get('join');
    state.invite = linked ? HaloInvite.parse(linked) : null;
    if (state.invite) $('invite-input').value = 'halo://join/' + state.invite;
    $('invite-status').textContent = linked && !state.invite ? 'This invite is incomplete or invalid.' :
      relay ? 'Ready for a desktop invite.' : 'Desktop invites need a relay. This preview does not have a relay configured yet.';
    $('relay-access-row').hidden = !relay;
    const connect = async () => {
      if (state.inviteConnecting || state.gatewayInstalled || state.started) return;
      const token = HaloInvite.parse($('invite-input').value);
      if (!token) { $('invite-status').textContent = 'Paste a complete halo://join/ invite.'; return; }
      cancelQuickPlay();
      state.quickFailed = false;
      state.invite = token;
      $('play').disabled = true;
      $('browser-rooms').hidden = true;
      if (!relay) {
        $('invite-status').textContent = 'The browser link is valid, but this site needs a relay before it can join desktop games.';
        return;
      }
      if (!hasFullMaps() || state.dataBusy) {
        state.pendingInviteConnect = connect;
        $('step-data').hidden = false;
        $('invite-status').textContent = 'Import your disc image to join this host. The invite will connect when all maps are ready.';
        connectPendingInvite();
        return;
      }
      $('invite-connect').disabled = true;
      $('invite-input').disabled = true;
      state.inviteConnecting = true;
      const attempt = ++state.inviteAttempt;
      let transport = null;
      try {
        transport = await HaloGateway.connect(relay, token, {
          accessToken: $('relay-access').value,
          onStatus(status) {
            if (attempt !== state.inviteAttempt) return;
            $('invite-status').textContent = status.message;
            if (status.state === 'error') {
              if (state.started && !state.manualMode) fatal(status.message + ' Reload to start a new session.');
              else {
                state.quickFailed = true;
                state.gateway = null;
                state.gatewayInstalled = false;
                $('invite-connect').disabled = false;
                $('invite-input').disabled = false;
                quickStatus('error', status.message);
              }
            }
            updatePlayButton();
            maybeQuickPlay();
          },
        });
        if (attempt !== state.inviteAttempt) { transport.close(); return; }
        await state.roomTask;
        if (attempt !== state.inviteAttempt) { transport.close(); return; }
        state.nativeInstalling = true;
        try { await HaloNet.useTransport(transport); }
        finally { state.nativeInstalling = false; }
        if (attempt !== state.inviteAttempt) { transport.close(); return; }
        // A host failure can arrive while leaving the previous browser room.
        if (transport.closed) throw new Error('The relay session ended. Try connecting again.');
        state.gateway = transport;
        state.gatewayInstalled = true;
        $('online-address').textContent = HaloNet.addressText(state.gateway.address);
        $('invite-input').disabled = true;
        $('relay-access').value = '';
        updatePlayButton();
        maybeQuickPlay();
      } catch (error) {
        transport?.close();
        if (attempt !== state.inviteAttempt) return;
        $('invite-status').textContent = error.message;
        $('invite-connect').disabled = false;
        $('invite-input').disabled = false;
      } finally {
        if (attempt === state.inviteAttempt) {
          state.inviteConnecting = false;
          updatePlayButton();
        }
      }
    };
    $('invite-connect').onclick = connect;
    $('invite-input').onkeydown = (event) => { if (event.key === 'Enter') connect(); };
    $('invite-share').onclick = async () => {
      try {
        const link = HaloInvite.link(location.href, $('invite-input').value);
        await navigator.clipboard.writeText(link);
        toast('Browser invite copied.');
      } catch (error) { $('invite-status').textContent = error.message; }
    };
    if (state.invite) {
      $('browser-rooms').hidden = true;
      if (relay && !state.manualMode) connect();
    }
  }

  function onlineOptions() {
    const options = {};
    if (settings.turnUrl) {
      options.turn = { urls: settings.turnUrl, username: settings.turnUser || '', credential: settings.turnPassword || '' };
    }
    // (tests name their own broker: ?signal=ws://...)
    const signal = new URLSearchParams(location.search).get('signal');
    if (signal) options.brokers = [signal];
    return options;
  }

  function roomLink(code) {
    const url = new URL(location.href);
    url.search = '';
    url.hash = '';
    url.searchParams.set('room', code);
    return url.toString();
  }

  function showOnline(status) {
    const inRoom = !!status.room;
    $('online-join').hidden = false;
    $('online-room').hidden = !inRoom;
    $('online-default').disabled = !DEFAULT_ROOM || status.room === DEFAULT_ROOM;
    maybeQuickPlay();
    if (!inRoom) return;
    $('online-code').textContent = status.room;
    $('room-toggle').textContent = 'Room ' + status.room;
    const players = status.players === 1 ? '1 other player' : `${status.players} other players`;
    $('online-status').textContent = status.brokers ? `Connected: ${players} in the room.` :
      'Looking for the room… (checking the connection)';
    $('online-names').textContent = status.names.length ? status.names.join(', ') : '';
  }

  function roomAction(action) {
    state.roomTask = state.roomTask.then(action).catch(error => toast(error.message));
    return state.roomTask;
  }

  function setRoomURL(code) {
    const url = new URL(location.href);
    if (code) url.searchParams.set('room', code);
    else url.searchParams.delete('room');
    history.replaceState(history.state, '', url);
  }

  function joinRoom(code, { remember = true, updateURL = true } = {}) {
    code = String(code || '').trim().toUpperCase().replace(/[^A-Z0-9]/g, '');
    if (code.length < 4 || code.length > 16) {
      toast('Use a room code with 4–16 letters or digits.');
      return Promise.resolve();
    }
    if (code === state.selectedRoom && code === HaloNet.status().room && !state.manualMode && !state.quickFailed)
      return state.roomTask;
    // Quick play is configured once when the engine starts. Changing only
    // WebRTC rooms would leave its old host/client session running on a new LAN.
    if (state.started || (state.dataBusy && state.manualMode)) {
      state.dataTransition = true;
      try {
        if (remember) localStorage.setItem('halo-web-room', code);
        localStorage.removeItem('halo-web-room-left');
      } catch { /* not kept */ }
      const url = new URL(location.href);
      url.searchParams.set('room', code);
      url.searchParams.delete('menu');
      url.hash = '';
      location.href = url.toString();
      return Promise.resolve();
    }
    cancelQuickPlay();
    state.quickFailed = false;
    state.selectedRoom = code;
    if (updateURL) {
      state.manualMode = false;
      state.manualRequested = false;
      const url = new URL(location.href);
      url.searchParams.delete('menu');
      history.replaceState(history.state, '', url);
    }
    $('online-share').disabled = true;
    return roomAction(async () => {
      const joined = await HaloNet.join(code, onlineOptions());
      try {
        if (remember) localStorage.setItem('halo-web-room', joined);
        localStorage.removeItem('halo-web-room-left');
      } catch { /* not kept */ }
      if (updateURL) setRoomURL(joined);
      $('online-share').disabled = joined !== state.selectedRoom;
      updatePlayButton();
      if (state.checksReady && !state.maps) showSteps(await mapsState());
      else maybeQuickPlay();
    });
  }

  function setUpOnline() {
    HaloNet.on((type, detail) => {
      if (type === 'diagnostic') log(`network ${new Date().toISOString()}: ${JSON.stringify(detail)}`);
      if (type === 'status') showOnline(detail);
      if (type === 'joined') toast(`${detail.name} joined the room.`);
      if (type === 'left') toast(`${detail.name} left the room.`);
    });
    let name = '';
    try { name = localStorage.getItem('halo-web-player-name') || ''; } catch { /* none */ }
    $('online-name').value = name;
    $('online-name').onchange = (event) => {
      try { localStorage.setItem('halo-web-player-name', event.target.value.trim().slice(0, 24)); } catch { /* none */ }
    };
    $('online-create').onclick = () => joinRoom(HaloNet.newRoomCode());
    $('online-default-room').hidden = !DEFAULT_ROOM;
    $('online-default-code').textContent = DEFAULT_ROOM;
    $('online-default').onclick = () => { if (DEFAULT_ROOM) return joinRoom(DEFAULT_ROOM); };
    $('online-enter').onclick = () => joinRoom($('online-input').value);
    $('online-input').onkeydown = (event) => { if (event.key === 'Enter') joinRoom(event.target.value); };
    $('online-leave').onclick = () => {
      if (state.started || !hasFullMaps()) {
        state.dataTransition = true;
        try {
          localStorage.removeItem('halo-web-room');
          localStorage.setItem('halo-web-room-left', '1');
        } catch { /* not kept */ }
        const url = new URL(location.href);
        url.searchParams.delete('room');
        url.searchParams.set('menu', '1');
        url.hash = '';
        location.href = url.toString();
        return Promise.resolve();
      }
      cancelQuickPlay();
      state.selectedRoom = null;
      quickStatus('menu', 'Room left. Choose a room or open the main menu.');
      updatePlayButton();
      return roomAction(async () => {
        await HaloNet.leave();
        try {
          localStorage.removeItem('halo-web-room');
          localStorage.setItem('halo-web-room-left', '1');
        } catch { /* none */ }
        setRoomURL(null);
      });
    };
    $('online-share').onclick = async () => {
      const room = HaloNet.status().room;
      if (!room || room !== state.selectedRoom || $('online-share').disabled) return;
      const link = roomLink(room);
      try {
        if (navigator.share) await navigator.share({ title: 'Halo CE room', text: 'Join my Halo game', url: link });
        else {
          await navigator.clipboard.writeText(link);
          toast('The room link is copied.');
        }
      } catch { /* cancelled */ }
    };
    $('online-address').textContent = HaloNet.addressText(HaloNet.address);
    $('opt-turn-url').value = settings.turnUrl || '';
    $('opt-turn-user').value = settings.turnUser || '';
    $('opt-turn-password').value = settings.turnPassword || '';
    for (const [id, key] of [['opt-turn-url', 'turnUrl'], ['opt-turn-user', 'turnUser'], ['opt-turn-password', 'turnPassword']]) {
      $(id).onchange = (event) => { settings[key] = event.target.value.trim(); saveSettings(); };
    }
    // An explicit link wins. Otherwise remember a chosen room or a decision
    // to leave; new visitors gather in the default public room.
    const linked = new URLSearchParams(location.search).get('room');
    let last = null, left = false;
    try {
      last = localStorage.getItem('halo-web-room');
      left = localStorage.getItem('halo-web-room-left') === '1';
    } catch { /* none */ }
    if (!$('invite-input') || !new URLSearchParams(location.hash.slice(1)).has('join')) {
      if (linked) joinRoom(linked, { updateURL: false });
      else if (!left && (last || DEFAULT_ROOM)) joinRoom(last || DEFAULT_ROOM, { remember: !!last, updateURL: false });
    }
    showOnline(HaloNet.status());
  }

  // ---------- updates

  async function checkForUpdate() {
    try {
      const current = await (await fetch('version.json')).json();
      state.version = current.version;
      $('version').textContent = 'Build ' + current.version;
      const latest = await (await fetch('version.json?latest=1', { cache: 'no-store' })).json();
      if (latest.version && latest.version !== current.version && navigator.serviceWorker.controller) {
        $('update-notice').hidden = false;
      }
    } catch { /* offline */ }
  }

  function updateFailed() {
    $('update-notice').hidden = false;
    $('update-message').textContent = 'The update could not be downloaded. Your current game is still available.';
    $('update-button').disabled = false;
    $('update-button').textContent = 'Retry update';
  }

  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.addEventListener('message', (event) => {
      if (event.data === 'updated') location.reload();
      if (event.data === 'update-failed') updateFailed();
    });
  }

  // ---------- start

  async function main() {
    window.addEventListener('error', (event) => {
      log(`error: ${event.message} (${event.filename}:${event.lineno})`);
      if (state.started) fatal(event.message || 'An error stopped the game.');
    });
    window.addEventListener('unhandledrejection', (event) => log('unhandled: ' + event.reason));
    window.addEventListener('resize', () => { if (state.shared) updateDisplaySize(); });
    document.addEventListener('visibilitychange', onVisibility);

    $('opt-touch').checked = settings.touch;
    $('opt-touch-fps').checked = settings.touchFps === true;
    $('opt-look').value = settings.look;
    $('opt-vsync').checked = settings.vsync;
    $('opt-resolution').value = settings.renderHeight;
    $('opt-resolution').onchange = (event) => {
      const height = Number(event.target.value);
      if (![480, 720, 1080, 1440].includes(height)) return;
      settings.renderHeight = height;
      saveSettings();
      if (state.shared) updateDisplaySize();
    };
    $('opt-touch').onchange = (event) => { settings.touch = event.target.checked; saveSettings(); };
    $('opt-touch-fps').onchange = (event) => { settings.touchFps = event.target.checked; saveSettings(); };
    $('opt-look').oninput = (event) => {
      settings.look = parseFloat(event.target.value);
      saveSettings();
      HaloInput.setLookSensitivity(settings.look);
    };
    $('opt-vsync').onchange = (event) => { settings.vsync = event.target.checked; saveSettings(); };
    $('opt-gldebug').checked = settings.glDebug;
    $('opt-gldebug').onchange = (event) => { settings.glDebug = event.target.checked; saveSettings(); };
    $('iso-file').onchange = onImageChosen;
    $('play').onclick = () => {
      unlockInteraction();
      if (!state.manualMode && (state.invite || state.selectedRoom)) return maybeQuickPlay();
      return play({ userGesture: true });
    };
    $('main-menu').onclick = openMainMenu;
    $('room-toggle').onclick = () => showRoomSettings(true);
    $('room-close').onclick = () => showRoomSettings(false);
    $('quick-menu').onclick = openMainMenu;
    $('fatal-menu').onclick = openMainMenu;
    $('update-button').onclick = () => {
      if ($('update-button').disabled) return;
      $('update-button').disabled = true;
      $('update-button').textContent = 'Updating…';
      $('update-message').textContent = 'Downloading the update. The game will restart when it is ready.';
      try {
        navigator.serviceWorker.controller.postMessage('update');
      } catch { updateFailed(); }
    };
    $('quick-retry').onclick = () => { state.quickFailed = false; state.manualMode = false; return maybeQuickPlay(); };
    for (const type of ['pointerdown', 'keydown', 'touchstart']) {
      window.addEventListener(type, (event) => {
        // Browser autoplay and pointer lock may require a normal game interaction.
        if (state.started && !state.roomSettingsOpen && (state.audio?.state !== 'running' ||
            (type === 'pointerdown' && event.target === $('screen') && !document.pointerLockElement))) {
          unlockInteraction();
        }
      }, { passive: true });
    }
    $('export-saves').onclick = exportSaves;
    $('delete-data').onclick = deleteData;
    const showLog = async () => {
      $('log-text').textContent = await fullLog();
      $('log-view').hidden = false;
    };
    $('show-log').onclick = showLog;
    $('quick-log').onclick = showLog;
    $('log-close').onclick = () => { $('log-view').hidden = true; };
    $('log-download').onclick = async () => {
      const file = await debugFile();
      const contents = [`${navigator.userAgent}\nBuild ${state.version || 'unknown'}\n\n--- page and console ---\n${state.log.join('\n')}\n\n--- debug.txt ---\n`];
      if (file) contents.push(file);
      const url = URL.createObjectURL(new Blob(contents, { type: 'text/plain;charset=utf-8' }));
      const link = document.createElement('a');
      link.href = url; link.download = 'halo-log-' + new Date().toISOString().replace(/[:.]/g, '-') + '.txt';
      link.click(); setTimeout(() => URL.revokeObjectURL(url), 30000);
    };
    $('log-copy').onclick = async () => {
      try { await navigator.clipboard.writeText(await fullLog()); toast('Copied.'); } catch { toast('Could not copy.'); }
    };
    $('fatal-reload').onclick = () => location.reload();
    $('fatal-log').onclick = async () => {
      try { await navigator.clipboard.writeText(await fullLog()); toast('Copied.'); } catch { toast('Could not copy.'); }
    };

    const standalone = navigator.standalone || matchMedia('(display-mode: standalone)').matches ||
      matchMedia('(display-mode: fullscreen)').matches;
    $('install-hint').hidden = standalone || !ios;
    $('iphone-tips').hidden = !ios;
    $('rotate-iphone-tips').hidden = !ios;
    // Android (Chrome, Edge, Samsung Internet): the browser's own install
    // prompt when it offers one, otherwise where to find it
    const android = /Android/i.test(navigator.userAgent);
    $('install-android').hidden = standalone || !android;
    window.addEventListener('beforeinstallprompt', (event) => {
      event.preventDefault();
      state.installPrompt = event;
      $('install-android').hidden = standalone;
      $('install-button').hidden = false;
      $('install-android-menu').hidden = true;
    });
    $('install-button').onclick = async () => {
      if (!state.installPrompt) return;
      state.installPrompt.prompt();
      const choice = await state.installPrompt.userChoice.catch(() => null);
      state.installPrompt = null;
      if (choice && choice.outcome === 'accepted') $('install-android').hidden = true;
    };
    window.addEventListener('appinstalled', () => { $('install-android').hidden = true; });

    await ensureIsolation();
    // A failed capability or data check must not hide an available repair.
    checkForUpdate();
    setUpOnline();
    setUpInvites();
    const ok = await runChecks();
    if (!ok) return;
    state.checksReady = true;
    showSteps(await mapsState());
  }

  main().catch((error) => {
    log('start: ' + (error && error.stack || error));
    addCheck(false, 'The page could not start: ' + error.message);
  });
})();
