"use strict";

// Suppress redundant VAO enable bits and sampler bindings before stream
// recording. Load after stream-batch.js (and replay-cache.js when present).
// Buffer pointers, uploads and draws retain their original order.
// Use ?vertex_state_cache=0 to compare with the uncached calls.
(() => {
    if (new URLSearchParams(location.search).get("vertex_state_cache") === "0") return;
    const canvas = document.getElementById("canvas");
    const getContext = canvas.getContext.bind(canvas);
    let wrapped = false;
    canvas.getContext = (type, options) => {
        const gl = getContext(type, options);
        if (!gl || wrapped || type !== "webgl2") return gl;
        wrapped = true;
        const native = {};
        for (const name of ["getParameter", "createVertexArray", "bindVertexArray", "deleteVertexArray",
            "enableVertexAttribArray", "disableVertexAttribArray", "createSampler", "bindSampler", "deleteSampler"])
            native[name] = gl[name].bind(gl);
        const maxAttributes = native.getParameter(gl.MAX_VERTEX_ATTRIBS);
        const maxUnits = native.getParameter(gl.MAX_COMBINED_TEXTURE_IMAGE_UNITS);
        const stats = { attributeCalls: 0, attributeSkipped: 0, samplerCalls: 0, samplerSkipped: 0 };
        let arrays, knownArrays, defaultArray, samplers, boundSamplers, currentArray, lost = false;
        const reset = () => {
            arrays = new WeakMap(); knownArrays = new WeakSet(); defaultArray = [];
            samplers = new WeakSet(); boundSamplers = new Map(); currentArray = null;
        };
        reset();
        gl.createVertexArray = () => {
            const array = native.createVertexArray();
            if (array) { knownArrays.add(array); arrays.set(array, []); }
            return array;
        };
        gl.bindVertexArray = array => {
            native.bindVertexArray(array);
            // Unknown/deleted objects may fail to bind. Stop caching until an
            // intercepted live object or the default VAO is explicitly bound.
            if (array === null || knownArrays.has(array)) {
                currentArray = array;
                if (array && !arrays.has(array)) arrays.set(array, []);
            } else {
                // Later setters may mutate the last actual VAO. Invalidate all
                // enable caches so rebinding it cannot reuse stale bits.
                arrays = new WeakMap(); defaultArray = []; currentArray = undefined;
            }
        };
        gl.deleteVertexArray = array => {
            native.deleteVertexArray(array);
            if (!array) return;
            knownArrays.delete(array);
            arrays.delete(array);
            if (currentArray === array) currentArray = null;
        };
        for (const name of ["enableVertexAttribArray", "disableVertexAttribArray"]) {
            const enabled = name === "enableVertexAttribArray";
            gl[name] = index => {
                stats.attributeCalls++;
                const state = currentArray === null ? defaultArray : arrays.get(currentArray);
                if (lost || !state || !Number.isInteger(index) || index < 0 || index >= maxAttributes) {
                    // Coercible non-integer arguments can still mutate native
                    // state. Conservatively invalidate this VAO before forwarding.
                    if (state) state.length = 0;
                    return native[name](index);
                }
                if (state[index] === enabled) { stats.attributeSkipped++; return; }
                native[name](index);
                state[index] = enabled;
            };
        }
        gl.createSampler = () => {
            const sampler = native.createSampler();
            if (sampler) samplers.add(sampler);
            return sampler;
        };
        gl.bindSampler = (unit, sampler) => {
            stats.samplerCalls++;
            if (lost || !Number.isInteger(unit) || unit < 0 || unit >= maxUnits ||
                (sampler !== null && !samplers.has(sampler))) {
                boundSamplers.clear();
                return native.bindSampler(unit, sampler);
            }
            if (boundSamplers.has(unit) && boundSamplers.get(unit) === sampler) {
                stats.samplerSkipped++; return;
            }
            native.bindSampler(unit, sampler);
            boundSamplers.set(unit, sampler);
        };
        gl.deleteSampler = sampler => {
            native.deleteSampler(sampler);
            if (!sampler) return;
            samplers.delete(sampler);
            for (const [unit, current] of boundSamplers)
                if (current === sampler) boundSamplers.delete(unit);
        };
        canvas.addEventListener("webglcontextlost", () => { lost = true; reset(); });
        canvas.addEventListener("webglcontextrestored", () => { lost = false; reset(); });
        setInterval(() => {
            const output = document.getElementById("performance-stats");
            if (output) output.dataset.vertexStateCache = JSON.stringify(stats);
        }, 1000);
        return gl;
    };
})();
