// Direct driver-call mock for the opt-in replay cache; no browser required.
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

class FakeGL {
    constructor() {
        Object.assign(this, { LINK_STATUS: 1, ACTIVE_UNIFORMS: 2, FLOAT: 5126, FLOAT_VEC4: 35666,
            INT: 5124, BOOL: 35670, SAMPLER_2D: 35678, calls: [], errors: [], current: null,
            draws: [], constantValues: new Map() });
    }
    linkProgram(program) { program.version = (program.version || 0) + 1; program.values = new Map(); }
    deleteProgram(program) { program.deleted = true; }
    getProgramParameter(program, name) { return name === this.LINK_STATUS ? !program.deleted : program.uniforms.length; }
    getActiveUniform(program, index) { return program.uniforms[index]; }
    getUniformLocation(program, name) { return { program, name, version: program.version }; }
    useProgram(program) { this.calls.push(["useProgram", program]); this.current = program; }
    write(name, location, data, offset = 0, length = 0) {
        this.calls.push([name, location, [...data]]);
        if (location.program !== this.current || location.version !== location.program.version || location.program.deleted) {
            this.errors.push("invalid location/program"); return;
        }
        const match = /^(\w+)(?:\[(\d+)\])?$/.exec(location.name);
        const width = match[1] === "c" ? 4 : 1;
        const values = location.program.values.get(match[1]) || [];
        const start = Number(match[2] || 0) * width;
        const count = length || data.length - offset;
        for (let index = 0; index < count; index++) values[start + index] = data[offset + index];
        location.program.values.set(match[1], values);
    }
    uniform4fv(location, data, offset, length) { this.write("uniform4fv", location, data, offset, length); }
    uniform1fv(location, data, offset, length) { this.write("uniform1fv", location, data, offset, length); }
    uniform1iv(location, data, offset, length) { this.write("uniform1iv", location, data, offset, length); }
    uniform1f(location, value) { this.write("uniform1f", location, [value]); }
    uniform1i(location, value) { this.write("uniform1i", location, [value]); }
    uniform4f(location, ...values) { this.write("uniform4f", location, values); }
    uniform4iv(location, values) { this.calls.push(["uniform4iv", location, values]); }
    depthMask(value) { this.calls.push(["depthMask", value]); this.depthWrite = value; }
    enable(value) { this.calls.push(["enable", value]); }
    disable(value) { this.calls.push(["disable", value]); }
    blendFunc(...values) { this.calls.push(["blendFunc", ...values]); }
    blendFuncSeparate(...values) { this.calls.push(["blendFuncSeparate", ...values]); }
    blendEquation(...values) { this.calls.push(["blendEquation", ...values]); }
    blendEquationSeparate(...values) { this.calls.push(["blendEquationSeparate", ...values]); }
    vertexAttrib4fv(index, values) { this.calls.push(["vertexAttrib4fv", index, [...values]]); this.constantValues.set(index, [...values]); }
    vertexAttrib4f(index, ...values) { this.calls.push(["vertexAttrib4f", index, values]); this.constantValues.set(index, values); }
    vertexAttrib1f(index, value) { this.calls.push(["vertexAttrib1f", index, value]); this.constantValues.set(index, [value, 0, 0, 1]); }
    vertexAttribI4ui(index, ...values) { this.calls.push(["vertexAttribI4ui", index, values]); this.constantValues.set(index, values); }
    samplerParameteri(...args) { this.calls.push(["samplerParameteri", ...args]); }
    samplerParameterf(...args) { this.calls.push(["samplerParameterf", ...args]); }
    deleteSampler(sampler) { this.calls.push(["deleteSampler", sampler]); }
    drawArrays() {
        this.draws.push({ program: this.current.label, depthWrite: this.depthWrite,
            uniforms: Object.fromEntries([...this.current.values].map(([key, value]) => [key, [...value]])),
            constants: Object.fromEntries([...this.constantValues].map(([key, value]) => [key, [...value]])) });
    }
}
class FakeGL2 extends FakeGL {}

const gl = new FakeGL2(), callbacks = {}, hud = { dataset: {} };
const canvas = { getContext: () => gl, addEventListener: (name, fn) => { callbacks[name] = fn; } };
vm.runInNewContext(fs.readFileSync(path.join(__dirname, "../port/web/replay-cache.js"), "utf8"), {
    URLSearchParams, Float32Array, location: { search: "" },
    document: { getElementById: id => id === "canvas" ? canvas : hud },
    WebGLRenderingContext: FakeGL, WebGL2RenderingContext: FakeGL2,
    setInterval: () => {},
});
canvas.getContext("webgl2");
const makeProgram = () => ({ uniforms: [
    { name: "c[0]", size: 4, type: gl.FLOAT_VEC4 },
    { name: "alpha", size: 1, type: gl.FLOAT },
    { name: "tex0", size: 1, type: gl.SAMPLER_2D },
] });
const calls = name => gl.calls.filter(call => call[0] === name).length;
const program = makeProgram(), other = makeProgram();
gl.linkProgram(program); gl.linkProgram(other); gl.useProgram(program);
const c0 = gl.getUniformLocation(program, "c[0]"), alias = gl.getUniformLocation(program, "c");
const c1 = gl.getUniformLocation(program, "c[1]");
const values = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
gl.uniform4fv(c0, values); gl.uniform4fv(alias, values);
assert.equal(calls("uniform4fv"), 1, "array aliases share state");
gl.uniform4fv(c1, new Float32Array([9, 10, 11, 12]));
gl.uniform4fv(c0, values);
assert.equal(calls("uniform4fv"), 3, "overlapping slices must restore changed elements");
gl.uniform4fv(c1, new Float32Array([0, 0, 5, 6, 7, 8, 0, 0]), 2, 4);
assert.equal(calls("uniform4fv"), 3, "offset/length selects the matching slice");
gl.uniform4f(c1, 5, 6, 7, 8);
assert.equal(calls("uniform4f"), 0, "scalar vec4 setter shares array state");
gl.uniform4iv(c1, new Int32Array([1, 2, 3, 4])); gl.uniform4fv(c0, values);
assert.equal(calls("uniform4fv"), 4, "alternate setter invalidates the uniform group");

gl.useProgram(other);
const otherC = gl.getUniformLocation(other, "c[0]");
gl.uniform4fv(otherC, values);
assert.equal(calls("uniform4fv"), 5, "programs have independent uniform state");
gl.uniform4fv(c0, new Float32Array([31, 32, 33, 34]));
assert.equal(gl.errors.length, 1, "wrong-current-program call is forwarded");
gl.useProgram(program); gl.uniform4fv(c0, values);
assert.equal(calls("uniform4fv"), 7, "invalid call cannot poison the cache");
const alpha = gl.getUniformLocation(program, "alpha"), tex = gl.getUniformLocation(program, "tex0");
gl.uniform1f(alpha, 0.25); gl.uniform1fv(alpha, new Float32Array([0.25]));
gl.uniform1i(tex, 2); gl.uniform1iv(tex, new Int32Array([2]));
assert.equal(calls("uniform1fv"), 0); assert.equal(calls("uniform1iv"), 0);

gl.linkProgram(program); gl.uniform4fv(c0, values);
assert.equal(gl.errors.length, 2, "old locations are invalid after relink");
const relinked = gl.getUniformLocation(program, "c[0]");
gl.uniform4fv(relinked, values); gl.uniform4fv(relinked, values);
assert.equal(calls("uniform4fv"), 9, "relinked uniforms start with unknown cache values");
gl.deleteProgram(program); gl.uniform4fv(relinked, values);
assert.equal(gl.errors.length, 3, "deleted-program locations are forwarded");

gl.depthMask(true); gl.depthMask(true); gl.depthMask(false);
assert.equal(calls("depthMask"), 2);
gl.enable(123); gl.enable(123); gl.disable(123); gl.disable(123);
assert.equal(calls("enable"), 1); assert.equal(calls("disable"), 1);
gl.blendFuncSeparate(1, 2, 3, 4); gl.blendFunc(1, 2); gl.blendFuncSeparate(1, 2, 1, 2);
assert.equal(calls("blendFuncSeparate"), 1); assert.equal(calls("blendFunc"), 1);
gl.blendEquationSeparate(1, 2); gl.blendEquation(1); gl.blendEquationSeparate(1, 1);
assert.equal(calls("blendEquationSeparate"), 1); assert.equal(calls("blendEquation"), 1);
const constant = new Float32Array([2, 0, 0, 1]);
gl.vertexAttrib4fv(7, constant); gl.vertexAttrib1f(7, 2); gl.vertexAttrib4f(7, 2, 0, 0, 1);
assert.equal(calls("vertexAttrib4fv"), 1); assert.equal(calls("vertexAttrib1f"), 0); assert.equal(calls("vertexAttrib4f"), 0);
constant[0] = 3; gl.vertexAttrib4fv(7, constant);
assert.equal(calls("vertexAttrib4fv"), 2, "caller mutation does not change the cached snapshot");
gl.vertexAttribI4ui(7, 3, 0, 0, 1); gl.vertexAttrib4fv(7, constant);
assert.equal(calls("vertexAttrib4fv"), 3, "integer constant setter invalidates float state");
const sampler = {};
gl.samplerParameteri(sampler, 1, 2); gl.samplerParameteri(sampler, 1, 2);
gl.samplerParameterf(sampler, 1, 2); gl.samplerParameteri(sampler, 1, 2);
assert.equal(calls("samplerParameteri"), 2); assert.equal(calls("samplerParameterf"), 1);
callbacks.webglcontextrestored(); gl.depthMask(false); gl.vertexAttrib4fv(7, constant);
assert.equal(calls("depthMask"), 3); assert.equal(calls("vertexAttrib4fv"), 4);
// Exercise the Float32Array fast path and ordinary-array conversion against
// the same values, preserving signed-zero distinctions and NaN equality.
const floats = makeProgram(); gl.linkProgram(floats); gl.useProgram(floats);
const floatLocation = gl.getUniformLocation(floats, "c[0]");
const floatCalls = calls("uniform4fv");
gl.uniform4fv(floatLocation, [0.1, NaN, -0, 4]);
gl.uniform4fv(floatLocation, new Float32Array([0.1, NaN, -0, 4]));
assert.equal(calls("uniform4fv"), floatCalls + 1, "float32 and rounded ordinary arrays share cache values");
gl.uniform4fv(floatLocation, new Float32Array([0.1, NaN, 0, 4]));
assert.equal(calls("uniform4fv"), floatCalls + 2, "uniform fast path distinguishes negative zero");
const attributeCalls = calls("vertexAttrib4fv");
gl.vertexAttrib4fv(8, [0.1, NaN, -0, 1]);
gl.vertexAttrib4fv(8, new Float32Array([0.1, NaN, -0, 1]));
assert.equal(calls("vertexAttrib4fv"), attributeCalls + 1, "constant fast path matches float32 rounding and NaN");
gl.vertexAttrib4fv(8, new Float32Array([0.1, NaN, 0, 1]));
assert.equal(calls("vertexAttrib4fv"), attributeCalls + 2, "constant fast path distinguishes negative zero");
console.log("PASS: uniform overlap/aliases/offsets, alternate setters, program switches/relink/delete, state collisions, constant attributes, and context restore.");

// Exercise the real stream recorder in both script orders. Compare the values
// actually consumed by every native draw, not only wrapper call counts.
function integration(order, search = "") {
    const context = new FakeGL2(), output = { dataset: {} }, timers = [];
    const canvas = { getContext: () => context, addEventListener: () => {} };
    const window = { requestAnimationFrame: () => 1 };
    const sandbox = vm.createContext({ URLSearchParams, Float32Array, location: { search },
        document: { getElementById: id => id === "canvas" ? canvas : output },
        WebGLRenderingContext: FakeGL, WebGL2RenderingContext: FakeGL2, window, setInterval: callback => { timers.push(callback); } });
    for (const file of order)
        vm.runInContext(fs.readFileSync(path.join(__dirname, "../port/web", file), "utf8"), sandbox);
    canvas.getContext("webgl2");
    const first = { ...makeProgram(), label: "first" }, second = { ...makeProgram(), label: "second" };
    context.linkProgram(first); context.linkProgram(second);
    let c = context.getUniformLocation(first, "c[0]");
    const cAlias = context.getUniformLocation(first, "c"), cNext = context.getUniformLocation(first, "c[1]");
    const cOther = context.getUniformLocation(second, "c[0]");
    const data = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const draw = () => context.drawArrays(4, 0, 3);
    context.useProgram(first); context.uniform4fv(c, data); context.depthMask(true);
    context.vertexAttrib4fv(7, new Float32Array([2, 0, 0, 1])); draw();
    context.useProgram(first); context.uniform4fv(cAlias, data); context.depthMask(true);
    context.vertexAttrib1f(7, 2); draw();
    context.uniform4fv(cNext, new Float32Array([21, 22, 23, 24])); draw();
    context.uniform4fv(c, data); draw();
    const mutable = new Float32Array([31, 32, 33, 34]);
    context.uniform4fv(cNext, mutable); mutable.fill(99); draw();
    context.useProgram(second); context.uniform4fv(cOther, data); context.depthMask(false); draw();
    context.useProgram(first); context.uniform4fv(cNext, new Float32Array([31, 32, 33, 34])); draw();
    // linkProgram must flush older draws before resetting this program's values.
    context.linkProgram(first);
    c = context.getUniformLocation(first, "c[0]");
    context.useProgram(first); context.uniform4fv(c, data); draw();
    window.requestAnimationFrame(() => {});
    for (const callback of timers) callback();
    assert.deepStrictEqual(context.errors, []);
    return { draws: context.draws, driverCalls: context.calls.length,
        commands: output.dataset.streamBatch ? JSON.parse(output.dataset.streamBatch).commands : null };
}
const direct = integration([]);
const recordOnly = integration(["stream-batch.js"]);
const cacheOnReplay = integration(["replay-cache.js", "stream-batch.js"]);
const cacheBeforeRecord = integration(["stream-batch.js", "replay-cache.js"]);
const explicitEnabled = integration(["stream-batch.js", "replay-cache.js"], "?replay_cache=1");
const optedOut = integration(["stream-batch.js", "replay-cache.js"], "?replay_cache=0");
assert.deepStrictEqual(recordOnly.draws, direct.draws);
assert.deepStrictEqual(cacheOnReplay.draws, direct.draws);
assert.deepStrictEqual(cacheBeforeRecord.draws, direct.draws);
assert.deepStrictEqual(explicitEnabled, cacheBeforeRecord, "explicit enable matches the default");
assert.deepStrictEqual(optedOut, recordOnly, "opt-out leaves all stream commands and driver calls untouched");
assert(cacheBeforeRecord.commands < cacheOnReplay.commands, "early cache reduces queued commands");
assert.equal(cacheBeforeRecord.driverCalls, cacheOnReplay.driverCalls, "both orders suppress the same driver calls");
console.log(`PASS: default/explicit enable and opt-out; real stream/cache integration in both orders; all ${direct.draws.length} draw snapshots match direct GL; queued commands ${cacheOnReplay.commands} → ${cacheBeforeRecord.commands}.`);
