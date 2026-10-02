/*
WEB_LIBRARY.JS

The JavaScript half of the web runtime (web_sdl.c), which runs on the game's
own thread (a Web Worker).

The game's WebGL 2 context draws into an OffscreenCanvas of this thread.
The game never returns to the worker's event loop, so frames cannot be
committed to a canvas on the page the usual way: each frame is taken out as
an ImageBitmap (transferToImageBitmap needs no event loop) and posted to the
page, which shows it (Module.haloPresent in port/web/site/app.js). Chrome on
macOS instead receives owned RGBA pixel buffers: its native ImageBitmap
transfer path can crash the renderer when a GPU backing image is missing.
*/

addToLibrary({
  $webHalo: {
    canvas: null,
    streamBatch: null,
    flushContext: null,
    pendingFrames: null,
    readFrame: null,
    emptyBitmapReported: false,
    // the number the main thread's pthread message handler uses for
    // Module[handler](...args) (Emscripten's CMD_CALL_HANDLER)
    callHandler: 9,
    post(handler, args, transfer) {
      if (ENVIRONMENT_IS_PTHREAD) {
        postMessage({ cmd: webHalo.callHandler, handler, args }, transfer || []);
      } else {
        Module[handler]?.(...args);
      }
    },
  },

  web_js_gl_create__deps: ['$GL', '$webHalo'],
  web_js_gl_create: (width, height, batchStreams, presentAck, pixelFrames) => {
    if (typeof OffscreenCanvas == 'undefined') {
      webHalo.post('haloMessage', [3, 'This browser cannot draw from a worker (OffscreenCanvas). iOS 17 or later is needed.']);
      return 0;
    }
    var canvas = new OffscreenCanvas(width, height);
    var attributes = {
      alpha: false,
      depth: false,
      stencil: false,
      antialias: false,
      premultipliedAlpha: false,
      preserveDrawingBuffer: false,
      powerPreference: 'high-performance',
      failIfMajorPerformanceCaveat: false,
    };
    var context = canvas.getContext('webgl2', attributes);
    if (!context) {
      webHalo.post('haloMessage', [3, 'WebGL 2 is not available.']);
      return 0;
    }
    // The shared recorder is bundled into halo.js with --pre-js, including
    // the pthread runtime. Native state caches already suppress redundant
    // setters; this merges append-only streamed uploads before their draws.
    webHalo.flushContext = context.flush.bind(context);
    // A separate shared counter lets the page release transferred bitmaps
    // even though this worker cannot service acknowledgement messages. Only
    // enable it when the launcher advertises support: an already open old
    // page can load a newer runtime after a service-worker update.
    webHalo.pendingFrames = presentAck ? new Int32Array(new SharedArrayBuffer(4)) : null;
    var readback = {};
    if (pixelFrames) {
      for (var name of ['getParameter', 'bindFramebuffer', 'bindBuffer', 'pixelStorei', 'readBuffer', 'readPixels'])
        readback[name] = context[name].bind(context);
    }
    // Chrome on macOS can crash its renderer while serializing a GPU-backed
    // ImageBitmap from this continuously running worker. Transfer owned RGBA
    // bytes instead. Capture the real methods before the stream recorder wraps
    // them, and restore every readback state the renderer can observe.
    webHalo.readFrame = pixelFrames ? () => {
      var width = canvas.width, height = canvas.height;
      var pixels = new Uint8Array(width * height * 4);
      var framebuffer = readback.getParameter(context.READ_FRAMEBUFFER_BINDING);
      var packBuffer = readback.getParameter(context.PIXEL_PACK_BUFFER_BINDING);
      var parameters = [context.PACK_ALIGNMENT, context.PACK_ROW_LENGTH,
        context.PACK_SKIP_PIXELS, context.PACK_SKIP_ROWS];
      var values = parameters.map(parameter => readback.getParameter(parameter));
      var readBuffer;
      try {
        readback.bindFramebuffer(context.READ_FRAMEBUFFER, null);
        readBuffer = readback.getParameter(context.READ_BUFFER);
        readback.readBuffer(context.BACK);
        readback.bindBuffer(context.PIXEL_PACK_BUFFER, null);
        parameters.forEach((parameter, index) => readback.pixelStorei(parameter, index ? 0 : 1));
        readback.readPixels(0, 0, width, height, context.RGBA, context.UNSIGNED_BYTE, pixels);
      } finally {
        parameters.forEach((parameter, index) => readback.pixelStorei(parameter, values[index]));
        readback.bindBuffer(context.PIXEL_PACK_BUFFER, packBuffer);
        if (readBuffer !== undefined) readback.readBuffer(readBuffer);
        readback.bindFramebuffer(context.READ_FRAMEBUFFER, framebuffer);
      }
      // WebGL's default framebuffer starts at the bottom; ImageData starts at
      // the top. Reverse rows without allocating another full-sized frame.
      var stride = width * 4, row = new Uint8Array(stride);
      for (var top = 0, bottom = height - 1; top < bottom; top++, bottom--) {
        var first = top * stride, last = bottom * stride;
        row.set(pixels.subarray(first, first + stride));
        pixels.copyWithin(first, last, last + stride);
        pixels.set(row, last);
      }
      for (var alpha = 3; alpha < pixels.length; alpha += 4) pixels[alpha] = 255;
      return { width, height, pixels: pixels.buffer };
    } : null;
    webHalo.streamBatch = batchStreams ? globalThis.HaloStreamBatch.install(context) : null;
    canvas.addEventListener?.('webglcontextlost', (event) => {
      event.preventDefault();
      webHalo.post('haloMessage', [3, 'The graphics context was lost. Reload the page to continue.']);
    });
    webHalo.canvas = canvas;
    var handle = GL.registerContext(context, Object.assign({
      majorVersion: 2,
      minorVersion: 0,
      enableExtensionsByDefault: 1,
    }, attributes));
    GL.makeContextCurrent(handle);
    return handle;
  },

  web_js_gl_resize__deps: ['$webHalo'],
  web_js_gl_resize: (width, height) => {
    var canvas = webHalo.canvas;
    if (canvas && (canvas.width != width || canvas.height != height)) {
      canvas.width = width;
      canvas.height = height;
    }
  },

  // Hidden quick-play frames still need a replay barrier, without producing
  // ImageBitmaps that the suspended page cannot present.
  web_js_gl_flush__deps: ['$webHalo'],
  web_js_gl_flush: () => {
    webHalo.streamBatch?.flush();
    // Hidden frames have no bitmap transfer (the normal implicit GL flush),
    // and this worker never yields to its event loop. Submit their commands
    // explicitly after replaying the recorder.
    webHalo.flushContext?.();
  },

  web_js_gl_present__deps: ['$webHalo'],
  web_js_gl_present: () => {
    var canvas = webHalo.canvas;
    if (!canvas) return;
    // Explicitly flush even when a frame has no final blit. Timers and RAF
    // cannot provide this barrier on the continuously running game worker.
    webHalo.streamBatch?.flush();
    var pending = webHalo.pendingFrames;
    if (pending && Atomics.load(pending, 0) >= 2) {
      // The page is busy or suspended. Keep submitting rendered commands,
      // without allocating more GPU-backed bitmaps or blocking simulation.
      webHalo.flushContext?.();
      return;
    }
    var bitmap = webHalo.readFrame ? webHalo.readFrame() : canvas.transferToImageBitmap();
    // Chrome can return an empty ImageBitmap after GPU allocation failure.
    // Its serializer dereferences the absent backing image; the dimensions
    // safely report zero, so discard it before passing a transfer list.
    if (!bitmap || !bitmap.width || !bitmap.height) {
      bitmap?.close?.();
      webHalo.flushContext?.();
      if (!webHalo.emptyBitmapReported) {
        webHalo.emptyBitmapReported = true;
        webHalo.post('haloMessage', [0, 'Skipped an empty browser frame before transfer.']);
      }
      return;
    }
    if (pending) Atomics.add(pending, 0, 1);
    try {
      webHalo.post('haloPresent', pending ? [bitmap, pending.buffer] : [bitmap], [bitmap.pixels || bitmap]);
    } catch (error) {
      if (pending) Atomics.sub(pending, 0, 1);
      bitmap.close?.();
      throw error;
    }
  },

  // kind: 0 status, 1 notice, 2 clipboard text, 3 fatal error
  web_js_post__deps: ['$webHalo'],
  web_js_post: (kind, text) => {
    webHalo.post('haloMessage', [kind, UTF8ToString(text)]);
  },
});
