"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "../port/web/vertex-state-cache.js"), "utf8");

class GL {
    constructor() {
        this.MAX_VERTEX_ATTRIBS = 34921; this.MAX_COMBINED_TEXTURE_IMAGE_UNITS = 35661;
        this.calls = []; this.errors = []; this.draws = []; this.restore();
    }
    restore() { this.vao = null; this.arrays = new Map([[null, new Set()]]); this.samplers = new Set(); this.units = new Map(); }
    getParameter(p) { return p === this.MAX_VERTEX_ATTRIBS ? 16 : 8; }
    createVertexArray() { const a = {}; this.arrays.set(a, new Set()); return a; }
    bindVertexArray(a) { if (this.arrays.has(a)) this.vao = a; else this.errors.push("invalid vao"); }
    deleteVertexArray(a) { if (!a) return; this.arrays.delete(a); if (this.vao === a) this.vao = null; }
    attribute(i, enabled) {
        this.calls.push(["attribute", i, enabled]); i = Number(i) >>> 0;
        if (i >= 16) return this.errors.push("invalid attribute");
        const state = this.arrays.get(this.vao); enabled ? state.add(i) : state.delete(i);
    }
    enableVertexAttribArray(i) { this.attribute(i, true); }
    disableVertexAttribArray(i) { this.attribute(i, false); }
    createSampler() { const s = { label: this.samplers.size }; this.samplers.add(s); return s; }
    bindSampler(i, s) {
        this.calls.push(["sampler", i, s]); i = Number(i) >>> 0;
        if (i >= 8 || (s !== null && !this.samplers.has(s))) return this.errors.push("invalid sampler");
        this.units.set(i, s);
    }
    deleteSampler(s) { this.samplers.delete(s); for (const [i, v] of this.units) if (v === s) this.units.set(i, null); }
    drawArrays() { this.draws.push({ enabled: [...this.arrays.get(this.vao)].sort(),
        samplers: Array.from({ length: 8 }, (_, i) => this.units.get(i)?.label ?? null) }); }
}
function setup(search) {
    const gl = new GL(), listeners = {}, timers = [], hud = { dataset: {} };
    const canvas = { getContext: () => gl, addEventListener: (name, fn) => { listeners[name] = fn; } };
    vm.runInNewContext(source, { URLSearchParams, location: { search }, document: {
        getElementById: id => id === "canvas" ? canvas : hud }, setInterval: fn => timers.push(fn) });
    canvas.getContext("webgl2");
    return { gl, restore: () => { listeners.webglcontextlost?.(); gl.restore(); listeners.webglcontextrestored?.(); },
        stats: () => { timers.forEach(fn => fn()); return JSON.parse(hud.dataset.vertexStateCache); } };
}
function run(env) {
    const { gl } = env;
    const a = gl.createVertexArray(), b = gl.createVertexArray();
    const s = gl.createSampler(), t = gl.createSampler();
    for (let i = 0; i < 10; i++) {
        gl.enableVertexAttribArray(0); gl.disableVertexAttribArray(3);
        gl.bindSampler(0, s); gl.bindSampler(1, t);
    }
    gl.drawArrays();
    gl.bindVertexArray(a); gl.disableVertexAttribArray(0); gl.drawArrays();
    gl.enableVertexAttribArray(1); gl.enableVertexAttribArray(1); gl.drawArrays();
    gl.bindVertexArray(b); gl.enableVertexAttribArray(0); gl.drawArrays();
    gl.bindVertexArray(a); gl.disableVertexAttribArray(0); gl.drawArrays();
    gl.deleteVertexArray(a); gl.enableVertexAttribArray(0); gl.drawArrays();
    gl.deleteSampler(s); gl.bindSampler(0, null); gl.bindSampler(0, null); gl.drawArrays();
    // Invalid/deleted binds must still reach native validation and cannot make
    // cached state suppress a subsequent valid draw-state change.
    gl.bindVertexArray(a); gl.disableVertexAttribArray(0); gl.drawArrays();
    gl.bindVertexArray(null); gl.enableVertexAttribArray(0); gl.drawArrays();
    gl.bindSampler(1, s); gl.bindSampler(1, s); gl.bindSampler(1, t); gl.drawArrays();
    gl.disableVertexAttribArray("0"); gl.enableVertexAttribArray(0); gl.drawArrays();
    gl.disableVertexAttribArray(20); gl.disableVertexAttribArray(20);
    gl.bindSampler("1", null); gl.bindSampler(1, t); gl.drawArrays();
    gl.bindSampler(10, t); gl.bindSampler(10, t);
    env.restore();
    gl.enableVertexAttribArray(0); gl.enableVertexAttribArray(0); gl.drawArrays();
    const u = gl.createSampler(); gl.bindSampler(0, u); gl.bindSampler(0, u); gl.drawArrays();
}
const original = setup("?vertex_state_cache=0"), cached = setup("");
run(original); run(cached);
assert.deepEqual(cached.gl.draws, original.gl.draws, "VAO, deletion and restoration preserve visible draw state");
assert.deepEqual(cached.gl.errors, original.gl.errors, "invalid calls still reach native validation");
const stats = cached.stats();
assert.ok(stats.attributeSkipped >= 20, "redundant enable bits were suppressed");
assert.ok(stats.samplerSkipped >= 20, "redundant sampler bindings were suppressed");
assert.equal(original.gl.calls.length - cached.gl.calls.length, stats.attributeSkipped + stats.samplerSkipped);
console.log("PASS per-VAO/default-VAO draw state, deletion, invalid calls, coercion and context restoration");
console.log(JSON.stringify({ originalNativeCalls: original.gl.calls.length, cachedNativeCalls: cached.gl.calls.length, ...stats }));
const explicit = setup("?vertex_state_cache=1");
run(explicit);
assert.deepEqual(explicit.gl.draws, cached.gl.draws);
assert.deepEqual(explicit.gl.calls.length, cached.gl.calls.length);
console.log("PASS default enable, explicit enable, and opt-out");
