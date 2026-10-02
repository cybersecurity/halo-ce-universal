#!/usr/bin/env node
// Compare observable draw inputs under immediate GL and deferred stream batching.
// Run from any directory: node tools/test_stream_batch.mjs
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

const source = readFileSync(new URL("../port/web/stream-batch.js", import.meta.url), "utf8");
const vertexCapacity = 16 * 1024 * 1024;
const indexCapacity = 2 * 1024 * 1024;

class MockGL {
    constructor() {
        Object.assign(this, { ARRAY_BUFFER: 34962, ELEMENT_ARRAY_BUFFER: 34963, STREAM_DRAW: 35040,
            DYNAMIC_DRAW: 35048, FLOAT: 5126, UNSIGNED_BYTE: 5121, UNSIGNED_SHORT: 5123,
            TRIANGLES: 4, COLOR: 6144, COPY_READ_BUFFER: 36662, COPY_WRITE_BUFFER: 36663, FRAMEBUFFER: 36160, DRAW_FRAMEBUFFER: 36009, READ_FRAMEBUFFER: 36008, TRANSFORM_FEEDBACK_BUFFER: 35982, COLOR_BUFFER_BIT: 16384, NEAREST: 9728 });
        this.bindings = new Map();
        this.arrays = new Map([[null, { element: null, attributes: new Map() }]]);
        this.vao = null;
        this.draws = [];
        this.uploads = 0;
        this.color = [];
        this.trace = [];
        this.drawFramebuffer = null;
        this.presented = [];
        this.submissions = [];
        this.feedback = null;
        this.feedbackActive = false;
    }
    createBuffer() { return { bytes: new Uint8Array(0) }; }
    createFramebuffer() { return {}; }
    bindFramebuffer(target, framebuffer) { if (target === this.FRAMEBUFFER || target === this.DRAW_FRAMEBUFFER) this.drawFramebuffer = framebuffer; }
    deleteFramebuffer(framebuffer) { if (framebuffer && this.drawFramebuffer === framebuffer) this.drawFramebuffer = null; }
    blitFramebuffer() { if (this.drawFramebuffer === null) this.presented.push(this.draws.slice()); }
    flush() { this.submissions.push(this.draws.length); }
    bindBufferRange(target, index, buffer, offset, size) {
        this.bindings.set(target, buffer);
        if (target === this.TRANSFORM_FEEDBACK_BUFFER) this.feedback = { buffer, offset };
    }
    beginTransformFeedback() { this.feedbackActive = true; }
    endTransformFeedback() { this.feedbackActive = false; }
    createVertexArray() { const value = {}; this.arrays.set(value, { element: null, attributes: new Map() }); return value; }
    bindVertexArray(value) { this.vao = value; }
    bindBuffer(target, buffer) {
        if (target === this.ELEMENT_ARRAY_BUFFER) this.arrays.get(this.vao).element = buffer;
        else this.bindings.set(target, buffer);
    }
    bufferData(target, value) {
        const buffer = target === this.ELEMENT_ARRAY_BUFFER ? this.arrays.get(this.vao).element : this.bindings.get(target);
        buffer.bytes = typeof value === "number" ? new Uint8Array(value) : new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice();
    }
    bufferSubData(target, offset, value, sourceOffset = 0, length = 0) {
        const buffer = target === this.ELEMENT_ARRAY_BUFFER ? this.arrays.get(this.vao).element : this.bindings.get(target);
        const unit = value.BYTES_PER_ELEMENT || 1;
        buffer.bytes.set(new Uint8Array(value.buffer, value.byteOffset + sourceOffset * unit, (length || value.byteLength / unit - sourceOffset) * unit), offset);
        this.uploads++;
    }
    enableVertexAttribArray(index) { this.arrays.get(this.vao).attributes.get(index).enabled = true; }
    vertexAttribPointer(index, size, type, normalized, stride, offset) {
        this.arrays.get(this.vao).attributes.set(index, { buffer: this.bindings.get(this.ARRAY_BUFFER), size, type, stride: stride || size, offset, enabled: true });
    }
    uniform4fv(location, data, offset = 0, length = 0) { if (location !== null) this.color = Array.from(data).slice(offset, offset + (length || data.length - offset)); }
    uniform4iv(location, data) { if (location !== null) this.color = Array.from(data); }
    uniform1f(location, value) { if (location !== null) this.color = [value]; }
    drawArrays(mode, first, count) { this.capture(Array.from({ length: count }, (_, i) => first + i)); }
    drawElements(mode, count, type, offset) {
        const bytes = this.arrays.get(this.vao).element.bytes;
        const indices = Array.from({ length: count }, (_, i) => type === this.UNSIGNED_BYTE ? bytes[offset + i] : new DataView(bytes.buffer).getUint16(offset + i * 2, true));
        this.capture(indices);
    }
    capture(indices) {
        const attribute = this.arrays.get(this.vao).attributes.get(0);
        this.draws.push({ vertices: indices.map(i => attribute.buffer.bytes[attribute.offset + i * attribute.stride]), color: this.color.slice() });
        this.trace.push("draw");
        if (this.feedbackActive) this.feedback.buffer.bytes[this.feedback.offset] = 88;
    }
    getBufferSubData(target, offset, destination) {
        const buffer = target === this.ELEMENT_ARRAY_BUFFER ? this.arrays.get(this.vao).element : this.bindings.get(target);
        destination.set(buffer.bytes.subarray(offset, offset + destination.byteLength));
        this.trace.push("read");
    }
    copyBufferSubData(readTarget, writeTarget, readOffset, writeOffset, size) {
        this.bindings.get(writeTarget).bytes.set(this.bindings.get(readTarget).bytes.slice(readOffset, readOffset + size), writeOffset);
    }
    getError() { this.trace.push("error-check"); return 0; }
    deleteBuffer(buffer) {
        for (const [target, bound] of this.bindings) if (bound === buffer) this.bindings.set(target, null);
        if (this.arrays.get(this.vao).element === buffer) this.arrays.get(this.vao).element = null;
    }
    deleteVertexArray(array) { if (!array) return; if (this.vao === array) this.vao = null; this.arrays.delete(array); }
}

function environment(batched, search = "?batch_streams") {
    const gl = new MockGL();
    const canvas = { getContext: () => gl };
    const window = { requestAnimationFrame: () => 1 };
    const timers = [], output = { dataset: {} };
    if (batched) {
        vm.runInNewContext(source, { URLSearchParams, Uint8Array, ArrayBuffer,
            location: { search }, document: { getElementById: id => id === "canvas" ? canvas : output }, window,
            WebGLRenderingContext: { prototype: MockGL.prototype }, WebGL2RenderingContext: { prototype: {} }, setInterval(fn) { timers.push(fn); } });
        canvas.getContext("webgl2");
    }
    return { gl, frame: () => window.requestAnimationFrame(() => {}), tick: () => timers.forEach(fn => fn()), stats: () => JSON.parse(output.dataset.streamBatch) };
}

function vertexBuffer(gl, size = vertexCapacity, usage = gl.STREAM_DRAW) {
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, size, usage);
    gl.vertexAttribPointer(0, 1, gl.UNSIGNED_BYTE, false, 1, 0);
    gl.enableVertexAttribArray(0);
    return buffer;
}

function compare(name, sequence, expectation) {
    const original = environment(false), batched = environment(true);
    sequence(original.gl, original.frame);
    sequence(batched.gl, batched.frame);
    original.frame(); batched.frame();
    assert.deepEqual(batched.gl.draws, original.gl.draws, `${name}: changed geometry or uniforms`);
    expectation?.(original.gl, batched.gl);
    process.stdout.write(`PASS ${name}\n`);
}

compare("persistent appends retain per-draw vertices and uniform snapshots", (gl, frame) => {
    vertexBuffer(gl); frame(); // Allocation in an earlier flush, as in the real triple ring.
    const color = new Float32Array([1, 0, 0, 1]);
    gl.uniform4fv({}, color);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([10, 20, 30]));
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    color.set([0, 1, 0, 1]); gl.uniform4fv({}, color);
    gl.bufferSubData(gl.ARRAY_BUFFER, 4, new Uint8Array([40, 50, 60]));
    gl.vertexAttribPointer(0, 1, gl.UNSIGNED_BYTE, false, 1, 4);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    color.fill(9);
}, (original, batched) => assert.equal(batched.uploads, original.uploads - 1));

compare("overwriting an already drawn range preserves old and new draws", gl => {
    vertexBuffer(gl);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1, 2, 3])); gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([7, 8, 9])); gl.drawArrays(gl.TRIANGLES, 0, 3);
}, (original, batched) => assert.equal(batched.uploads, original.uploads));

compare("readbacks observe all prior writes before later mutations", gl => {
    vertexBuffer(gl);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1, 2, 3]));
    const read = new Uint8Array(3); gl.getBufferSubData(gl.ARRAY_BUFFER, 0, read); assert.deepEqual(Array.from(read), [1, 2, 3]);
    gl.bufferSubData(gl.ARRAY_BUFFER, 4, new Uint8Array([4, 5, 6])); gl.drawArrays(gl.TRIANGLES, 0, 3);
});

compare("existing bytes between later appends survive frame boundaries", (gl, frame) => {
    vertexBuffer(gl);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([10, 20, 30, 40, 50, 60, 70])); frame();
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1])); gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bufferSubData(gl.ARRAY_BUFFER, 6, new Uint8Array([9])); gl.drawArrays(gl.TRIANGLES, 4, 3);
});

compare("two VAOs retain independent index bindings and offsets", gl => {
    const first = gl.createVertexArray(), second = gl.createVertexArray();
    for (const [array, vertices] of [[first, [10, 20, 30]], [second, [40, 50, 60]]]) {
        gl.bindVertexArray(array); vertexBuffer(gl);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array(vertices));
        const index = gl.createBuffer(); gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indexCapacity, gl.STREAM_DRAW);
        gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, 0, new Uint16Array([2, 0, 1]));
    }
    gl.bindVertexArray(first); gl.drawElements(gl.TRIANGLES, 3, gl.UNSIGNED_SHORT, 0);
    gl.bindVertexArray(second); gl.drawElements(gl.TRIANGLES, 3, gl.UNSIGNED_SHORT, 0);
});

compare("unrelated dynamic buffers keep upload order", gl => {
    vertexBuffer(gl, 65536, gl.DYNAMIC_DRAW);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1, 2, 3])); gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bufferSubData(gl.ARRAY_BUFFER, 4, new Uint8Array([4, 5, 6])); gl.drawArrays(gl.TRIANGLES, 0, 3);
}, (original, batched) => assert.equal(batched.uploads, original.uploads));

compare("typed buffer replacement and heap-offset writes stay ordered", gl => {
    vertexBuffer(gl);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([0, 1, 2, 3, 4]), 1, 3); gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bufferData(gl.ARRAY_BUFFER, new Uint8Array([7, 8, 9]), gl.STREAM_DRAW); gl.drawArrays(gl.TRIANGLES, 0, 3);
});

compare("deleting a ring before allocating its replacement releases bindings", gl => {
    const old = vertexBuffer(gl); gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1, 2, 3])); gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.deleteBuffer(old); vertexBuffer(gl); gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([4, 5, 6])); gl.drawArrays(gl.TRIANGLES, 0, 3);
});

{
    const { gl, frame } = environment(true);
    let copied = 0;
    gl.uniform4fv(null, { length: 4, slice() { copied++; return [1, 2, 3, 4]; } });
    gl.uniform1f(null, 9); frame();
    assert.equal(copied, 0, "null uniform must not copy source data");
    assert.deepEqual(gl.color, []);
    process.stdout.write("PASS null uniforms skip unused copies and native writes\n");
}

compare("GPU buffer copies invalidate stale CPU snapshots", (gl, frame) => {
    const vertices = vertexBuffer(gl);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([10, 20, 30, 40, 50, 60, 70])); frame();
    const source = gl.createBuffer(); gl.bindBuffer(gl.COPY_READ_BUFFER, source);
    gl.bufferData(gl.COPY_READ_BUFFER, new Uint8Array([90, 91]), gl.STREAM_DRAW);
    gl.bindBuffer(gl.COPY_WRITE_BUFFER, vertices);
    gl.copyBufferSubData(gl.COPY_READ_BUFFER, gl.COPY_WRITE_BUFFER, 0, 2, 2);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1])); gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bufferSubData(gl.ARRAY_BUFFER, 6, new Uint8Array([9])); gl.drawArrays(gl.TRIANGLES, 4, 3);
});

for (const [query, expectedUploads] of [["", 1], ["?batch_streams=0", 2]]) {
    const { gl, frame } = environment(true, query);
    vertexBuffer(gl); frame();
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1, 2, 3])); gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bufferSubData(gl.ARRAY_BUFFER, 4, new Uint8Array([4, 5, 6])); frame();
    assert.equal(gl.uploads, expectedUploads);
}
process.stdout.write("PASS default fast path and explicit opt-out\n");

compare("transform-feedback binding separates pending uploads from GPU writes", (gl, frame) => {
    const vertices = vertexBuffer(gl);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120])); frame();
    const source = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, source);
    gl.bufferData(gl.ARRAY_BUFFER, new Uint8Array([5, 6, 7]), gl.STREAM_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, vertices);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1]));
    gl.bindBufferRange(gl.TRANSFORM_FEEDBACK_BUFFER, 0, vertices, 8, 4);
    gl.bindBuffer(gl.ARRAY_BUFFER, source); gl.vertexAttribPointer(0, 1, gl.UNSIGNED_BYTE, false, 1, 0);
    gl.beginTransformFeedback(gl.TRIANGLES); gl.drawArrays(gl.TRIANGLES, 0, 3); gl.endTransformFeedback();
    gl.bindBuffer(gl.ARRAY_BUFFER, vertices);
    gl.bufferSubData(gl.ARRAY_BUFFER, 8, new Uint8Array([9]));
    gl.vertexAttribPointer(0, 1, gl.UNSIGNED_BYTE, false, 1, 6); gl.drawArrays(gl.TRIANGLES, 0, 3);
});

{
    const { gl } = environment(true);
    const offscreen = gl.createFramebuffer();
    vertexBuffer(gl);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1, 2, 3])); gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, offscreen);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    gl.blitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assert.equal(gl.draws.length, 0, "offscreen blits must retain batching even with default read framebuffer");
    gl.bindFramebuffer(gl.FRAMEBUFFER, null); gl.blitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assert.equal(gl.presented.length, 1, "canvas presentation must flush without requestAnimationFrame");
    assert.deepEqual(gl.presented[0][0].vertices, [1, 2, 3]);
    process.stdout.write("PASS presentation without RAF and offscreen blit batching\n");
}


compare("nonmonotonic disjoint writes keep earlier draw inputs", (gl, frame) => {
    vertexBuffer(gl);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110])); frame();
    gl.bufferSubData(gl.ARRAY_BUFFER, 8, new Uint8Array([1, 2, 3]));
    gl.vertexAttribPointer(0, 1, gl.UNSIGNED_BYTE, false, 1, 4); gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bufferSubData(gl.ARRAY_BUFFER, 4, new Uint8Array([4, 5, 6])); gl.drawArrays(gl.TRIANGLES, 0, 3);
}, (original, batched) => assert.equal(batched.uploads, original.uploads));

compare("mixed uniform snapshots retain source offsets and survive later source mutations", (gl, frame) => {
    vertexBuffer(gl);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1, 2, 3]));
    const values = new Float32Array(770), integers = new Int32Array(4);
    for (let tick = 0; tick < 3; tick++) {
        for (let draw = 0; draw < 30; draw++) {
            values.fill(tick * 100 + draw); gl.uniform4fv({}, values, 1, 768); gl.drawArrays(gl.TRIANGLES, 0, 3);
            integers.fill(-draw); gl.uniform4iv({}, integers); gl.drawArrays(gl.TRIANGLES, 0, 3);
        }
        values.fill(-1); integers.fill(-1); frame();
    }
});


{
    const run = environment(true);
    run.tick(); assert.equal(run.stats().commandLimit, 40000);
    for (let i = 0; i < 50000; i++) run.gl.uniform1f({}, i);
    assert.deepEqual(run.gl.color, [40000], "a large frame must flush once at the fixed queue bound");
    run.frame(); assert.deepEqual(run.gl.color, [49999], "the final queued updates must replay in order");
}
process.stdout.write("PASS fixed queue bound and complete large-frame replay\n");

compare("snapshot arena wraps without changing earlier draws or uniform values", (gl, frame) => {
    vertexBuffer(gl); frame();
    const data = new Uint8Array(8192), color = new Float32Array(4);
    for (let draw = 0; draw < 2300; draw++) {
        data.fill(draw & 255); color.fill(draw);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, data);
        gl.uniform4fv({}, color);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
    }
    data.fill(0); color.fill(0);
});

compare("oversized synchronous upload invalidates snapshots before later partial writes", (gl, frame) => {
    vertexBuffer(gl); frame();
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([1, 2, 3]));
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    // A desktop streaming slot is larger than the fixed snapshot arena.
    gl.bufferData(gl.ARRAY_BUFFER, 32 * 1024 * 1024, gl.STREAM_DRAW);
    const big = new Uint8Array(17 * 1024 * 1024).fill(8);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, big);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Uint8Array([4]));
    gl.bufferSubData(gl.ARRAY_BUFFER, 2, new Uint8Array([6]));
    gl.drawArrays(gl.TRIANGLES, 0, 3);
});

// The worker has no document, window, timers or animation-frame callback.
// Exercise the same installer through the source runtime's presentation path,
// including a frame without a final blit and the explicit source opt-out.
const workerLibrary = readFileSync(new URL("../port/web/src/web_library.js", import.meta.url), "utf8");
for (const enabled of [0, 1]) {
    const gl = new MockGL(), messages = [];
    let library, bitmapTransfers = 0, expectedDrawsAtTransfer = 2;
    const context = vm.createContext({
        Uint8Array, ArrayBuffer, SharedArrayBuffer,
        WebGLRenderingContext: { prototype: MockGL.prototype }, WebGL2RenderingContext: { prototype: {} },
        OffscreenCanvas: class {
            constructor(width, height) { this.width = width; this.height = height; }
            getContext(type) { assert.equal(type, "webgl2"); return gl; }
            transferToImageBitmap() {
                bitmapTransfers++;
                assert.equal(gl.draws.length, expectedDrawsAtTransfer, "all draws must execute before bitmap transfer");
                return { width: this.width, height: this.height, draws: gl.draws.slice() };
            }
        },
        GL: { registerContext(value) { assert.equal(value, gl); return 7; }, makeContextCurrent() {} },
        ENVIRONMENT_IS_PTHREAD: true,
        postMessage(message, transfer) { messages.push({ message, transfer }); },
        addToLibrary(value) { library = value; },
    });
    vm.runInContext(source, context);
    vm.runInContext(workerLibrary, context);
    context.webHalo = library.$webHalo;
    assert.equal(library.web_js_gl_create(640, 480, enabled, true), 7);
    const controller = context.webHalo.streamBatch;
    if (enabled) assert.equal(context.HaloStreamBatch.install(gl), controller, "install must be idempotent");
    else assert.equal(controller, null);
    vertexBuffer(gl);
    if (enabled) controller.flush();
    const vertices = new Uint8Array([1, 2, 3]);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, vertices); gl.drawArrays(gl.TRIANGLES, 0, 3);
    vertices.set([4, 5, 6]);
    gl.bufferSubData(gl.ARRAY_BUFFER, 4, vertices);
    gl.vertexAttribPointer(0, 1, gl.UNSIGNED_BYTE, false, 1, 4); gl.drawArrays(gl.TRIANGLES, 0, 3);
    vertices.fill(9);
    assert.equal(gl.draws.length, enabled ? 0 : 2, "source opt-out must retain the immediate path");
    library.web_js_gl_present();
    assert.equal(bitmapTransfers, 1);
    assert.equal(gl.uploads, enabled ? 1 : 2);
    assert.deepEqual(gl.draws.map(draw => draw.vertices), [[1, 2, 3], [4, 5, 6]]);
    assert.equal(messages[0].message.handler, "haloPresent");
    assert.equal(messages[0].message.args[0], messages[0].transfer[0]);
    const pending = new Int32Array(messages[0].message.args[1]);
    assert.equal(pending.buffer.byteLength, 4, "presentation acknowledgement uses a separate shared word");
    assert.equal(Atomics.load(pending, 0), 1);
    assert.equal(messages[0].transfer.length, 1, "shared counters must be shared, not transferred");
    if (enabled) {
        assert.equal(controller.snapshot().uploadsSaved, 1);
        controller.resetStats();
        assert.equal(controller.snapshot().uploadsSaved, 0);
    }
    // SDL's hidden quick-play swap uses a flush-only barrier. Each frame
    // replays immediately, without retaining draws until visibility returns
    // and without creating or posting any additional bitmap.
    for (let frame = 0; frame < 10; frame++) {
        gl.bufferSubData(gl.ARRAY_BUFFER, 4, new Uint8Array([frame, 8, 9]));
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        library.web_js_gl_flush();
        assert.equal(gl.draws.length, frame + 3);
        assert.deepEqual(gl.draws.at(-1).vertices, [frame, 8, 9]);
    }
    assert.equal(bitmapTransfers, 1, "hidden frame barriers must not transfer bitmaps");
    assert.equal(messages.length, 1, "hidden frames must not queue page presentation messages");
    assert.deepEqual(gl.submissions, Array.from({ length: 10 }, (_, frame) => frame + 3),
        "every hidden frame must submit native GPU work after replaying its draw");

    // Leave both visible transfers unconsumed, then render another frame.
    // Backpressure must replay and submit its draw without retaining another
    // bitmap; a shared acknowledgement makes the next handoff possible.
    expectedDrawsAtTransfer = 12;
    library.web_js_gl_present();
    assert.equal(Atomics.load(pending, 0), 2);
    gl.bufferSubData(gl.ARRAY_BUFFER, 4, new Uint8Array([10, 11, 12]));
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    library.web_js_gl_present();
    assert.equal(gl.draws.length, 13);
    assert.deepEqual(gl.draws.at(-1).vertices, [10, 11, 12]);
    assert.equal(gl.submissions.at(-1), 13, "a full handoff queue must still submit the new draw");
    assert.equal(gl.submissions.length, 11);
    assert.equal(bitmapTransfers, 2, "a blocked consumer must bound GPU bitmap allocations");
    assert.equal(messages.length, 2);
    Atomics.sub(pending, 0, 1);
    expectedDrawsAtTransfer = 13;
    library.web_js_gl_present();
    assert.equal(bitmapTransfers, 3, "acknowledgement releases a handoff slot");
    assert.equal(Atomics.load(pending, 0), 2);
}
process.stdout.write("PASS worker source runtime submits visible, hidden and backpressured frames and supports opt-out\n");

// Optional CPU-only comparison of recorder overhead. This uses no-op native GL
// methods; it cannot predict GPU time or game FPS. Pin a checked-in baseline:
// node tools/test_stream_batch.mjs --benchmark --baseline-ref=b9da047
if (process.argv.includes("--benchmark")) {
    const { execFileSync } = await import("node:child_process");
    const { performance } = await import("node:perf_hooks");
    const baselineRef = process.argv.find(value => value.startsWith("--baseline-ref="))?.slice(15);
    assert(baselineRef, "--benchmark requires --baseline-ref=<git-ref>");
    const baseline = execFileSync("git", ["show", `${baselineRef}:port/web/stream-batch.js`], { cwd: new URL("..", import.meta.url), encoding: "utf8" });
    const drawsPerFrame = 400, framesPerSample = 24, rounds = 3;

    function benchmarkEnvironment(script) {
        let checksum = 0, presents = 0, calls = 0, datasetWrites = 0;
        const methods = {
            createBuffer() { return {}; },
            bufferData() {},
            bufferSubData(target, offset, data) { calls++; checksum += data[0] || 0; },
            uniform4fv(location, values) { calls++; if (location) checksum += values[0] || 0; },
            drawElements() { calls++; },
            blitFramebuffer() { calls++; presents++; },
        };
        for (const name of ["bindBuffer", "bindVertexArray", "bindFramebuffer", "activeTexture", "bindTexture", "bindSampler", "samplerParameteri", "useProgram", "uniform1f", "enableVertexAttribArray", "vertexAttribPointer"]) methods[name] = function () { calls++; };
        const gl = Object.assign({ ARRAY_BUFFER: 34962, ELEMENT_ARRAY_BUFFER: 34963, STREAM_DRAW: 35040,
            UNSIGNED_SHORT: 5123, FLOAT: 5126, TRIANGLES: 4, FRAMEBUFFER: 36160, DRAW_FRAMEBUFFER: 36009,
            COLOR_BUFFER_BIT: 16384, NEAREST: 9728 }, methods);
        const canvas = { getContext: () => gl };
        const output = { dataset: {} };
        Object.defineProperty(output.dataset, "streamBatch", { set() { datasetWrites++; } });
        const window = { requestAnimationFrame() {} };
        vm.runInNewContext(script, { URLSearchParams, Uint8Array, ArrayBuffer,
            location: { search: "?batch_streams" }, document: { getElementById: id => id === "canvas" ? canvas : output }, window,
            WebGLRenderingContext: { prototype: methods }, WebGL2RenderingContext: { prototype: {} }, setInterval() {} });
        canvas.getContext("webgl2");
        const vertex = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, vertex); gl.bufferData(gl.ARRAY_BUFFER, vertexCapacity, gl.STREAM_DRAW);
        const index = gl.createBuffer(); gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indexCapacity, gl.STREAM_DRAW);
        window.requestAnimationFrame(() => {});
        const vertexData = new Uint8Array(8192), indexData = new Uint8Array(768);
        const constants = new Float32Array(768), material = new Float32Array(32);
        const locations = Array.from({ length: 12 }, () => ({}));
        const textures = Array.from({ length: 4 }, () => ({})), samplers = textures.map(() => ({}));
        const program = {};
        const frame = frameNumber => {
            for (let draw = 0; draw < drawsPerFrame; draw++) {
                gl.useProgram(program);
                gl.bindBuffer(gl.ARRAY_BUFFER, vertex); vertexData[0] = draw & 255;
                gl.bufferSubData(gl.ARRAY_BUFFER, draw * vertexData.byteLength, vertexData);
                gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index); indexData[0] = draw & 255;
                gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, draw * indexData.byteLength, indexData);
                constants[0] = frameNumber + draw;
                gl.uniform4fv(locations[0], constants);
                for (let field = 1; field <= 10; field++) { material[0] = field + draw; gl.uniform4fv(locations[field], material); }
                gl.uniform1f(locations[11], frameNumber);
                for (let unused = 0; unused < 6; unused++) gl.uniform4fv(null, material);
                for (let unit = 0; unit < 4; unit++) {
                    gl.activeTexture(33984 + unit); gl.bindTexture(3553, textures[unit]); gl.bindSampler(unit, samplers[unit]);
                    gl.samplerParameteri(samplers[unit], 10241, 9729); gl.samplerParameteri(samplers[unit], 10242, 10497); gl.samplerParameteri(samplers[unit], 10243, 10497);
                }
                for (let attribute = 0; attribute < 8; attribute++) {
                    gl.enableVertexAttribArray(attribute); gl.vertexAttribPointer(attribute, 3, gl.FLOAT, false, 32, draw * vertexData.byteLength);
                }
                gl.drawElements(gl.TRIANGLES, 384, gl.UNSIGNED_SHORT, draw * indexData.byteLength);
            }
            gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            gl.blitFramebuffer(0, 0, 640, 480, 0, 0, 640, 480, gl.COLOR_BUFFER_BIT, gl.NEAREST);
        };
        return { frame, counters: () => ({ checksum, presents, calls, datasetWrites }) };
    }

    const cases = { baseline, current: source };
    const results = Object.fromEntries(Object.keys(cases).map(name => [name, []]));
    for (let round = 0; round < rounds; round++) {
        const order = Object.keys(cases);
        if (round % 2) order.reverse();
        const counters = {};
        for (const name of order) {
            const run = benchmarkEnvironment(cases[name]);
            for (let frame = 0; frame < 6; frame++) run.frame(frame);
            const start = performance.now();
            for (let frame = 0; frame < framesPerSample; frame++) run.frame(frame);
            const elapsed = performance.now() - start;
            results[name].push(elapsed / framesPerSample);
            counters[name] = run.counters();
        }
        for (const name of Object.keys(cases).filter(name => name !== "baseline")) for (const field of ["checksum", "presents", "calls"]) assert.equal(counters[name][field], counters.baseline[field], `benchmark ${name} changed native ${field}`);
    }
    const summary = {};
    for (const [name, samples] of Object.entries(results)) {
        const sorted = samples.slice().sort((a, b) => a - b);
        summary[name] = { medianMsPerFrame: +sorted[Math.floor(sorted.length / 2)].toFixed(3), samplesMsPerFrame: samples.map(value => +value.toFixed(3)) };
    }
    console.log(JSON.stringify({ benchmark: "CPU recorder/replay only; no GPU or game FPS", baselineRef, drawsPerFrame,
        recordedCommandsPerFrame: 400 * 58 + 2, streamBytesPerFrame: drawsPerFrame * (8192 + 768), framesPerSample, rounds, results: summary }, null, 2));
}
