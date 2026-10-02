"use strict";

// WebGL command recording for Halo's append-only streamed vertex
// and index buffers. The default path merges a frame's disjoint uploads into one
// upload at its first upload (or numeric bufferData orphan), before replaying draws.
// Halo's stream allocator writes each range before first use and advances the
// offset monotonically within the frame; overlapping writes disable the merge.
// Bounded CPU shadows retain untouched bytes in the persistent triple-buffer ring.
// Readbacks, texture transfers and buffer mutations outside this pattern flush
// the queue. Use ?batch_streams=0 to compare with the unmodified GL call path.
(() => {
    const commandLimit = 40000;
    const installed = new WeakMap();
    // The source-built runtime installs this directly on its worker-owned
    // OffscreenCanvas context. It must flush before transferring the bitmap;
    // that worker never returns to its event loop or schedules animation frames.
    const install = gl => {
        if (installed.has(gl)) return installed.get(gl);
        const native = {};
        const methodNames = new Set([...Object.getOwnPropertyNames(WebGLRenderingContext.prototype), ...Object.getOwnPropertyNames(WebGL2RenderingContext.prototype)]);
        for (const name of methodNames) if (name !== "constructor" && typeof gl[name] === "function") native[name] = gl[name].bind(gl);
        let commands = [], bufferCommands = [], bytes = 0, flushing = false;
        // Keep snapshots until replay without allocating a new backing store
        // for every draw. Native GL consumes the bytes before this arena is
        // reused. Align views for every uniform element type (including f64).
        const snapshotLimit = 16 * 1024 * 1024;
        const snapshots = new Uint8Array(snapshotLimit);
        let snapshotOffset = 0;
        const buffers = new Map(), elementBuffers = new Map(), storage = new WeakMap();
        let vao = null, drawFramebuffer = null;
        const bound = target => target === gl.ELEMENT_ARRAY_BUFFER ? elementBuffers.get(vao) : buffers.get(target);
        // These are the actual Halo streaming capacities: Android/browser uses
        // three 16MiB/2MiB buffers; the desktop path uses 32MiB/8MiB orphans.
        // Do not infer append-only behavior for other dynamic game buffers.
        const streamStorage = (target, size, usage) => usage === gl.STREAM_DRAW &&
            ((target === gl.ARRAY_BUFFER && (size === 16 * 1024 * 1024 || size === 32 * 1024 * 1024)) ||
             (target === gl.ELEMENT_ARRAY_BUFFER && (size === 2 * 1024 * 1024 || size === 8 * 1024 * 1024)));
        const stats = { flushes: 0, commands: 0, batches: 0, uploadsSaved: 0, mergedBytes: 0, recordBytes: 0, numericAllocations: 0, typedAllocations: 0, eligibleGroups: 0, overlappingGroups: 0, outOfRangeGroups: 0, uploadsWithoutOrphan: 0, groupsWithMultipleUploads: 0, persistentGroups: 0 };
        const allocationKinds = new Map(), uploadTargets = new Map();
        const countKind = (map, key) => { if (map.size < 32 || map.has(key)) map.set(key, (map.get(key) || 0) + 1); };
        const flush = () => {
            if (flushing || !commands.length) return;
            flushing = true;
            const work = commands, bufferWork = bufferCommands;
            commands = [];
            bufferCommands = [];
            bytes = 0;
            const candidates = new Map();
            for (const command of bufferWork) {
                const [name, args, buffer, info] = command;
                if (name === "bufferData") {
                    // Match Halo's streaming allocations only. Arbitrary persistent
                    // buffers may be overwritten between draws and must stay ordered.
                    if (buffer && info?.shadow && streamStorage(args[0], args[1], args[2])) {
                        const group = { command, size: args[1], writes: [], first: Infinity, last: 0, valid: true, info };
                        stats.eligibleGroups++;
                        candidates.set(buffer, group);
                        command.batch = group;
                    } else candidates.delete(buffer);
                } else if (name === "bufferSubData") {
                    let group = candidates.get(buffer);
                    if (!group && info?.shadow) {
                        group = { command, size: info.size, writes: [], first: Infinity, last: 0, valid: true, info, persistent: true };
                        candidates.set(buffer, group);
                        command.batch = group;
                        stats.persistentGroups++;
                    }
                    if (group) {
                        const first = args[1], last = first + args[2].byteLength;
                        if (first < 0 || last > group.size) { if (group.valid) stats.outOfRangeGroups++; group.valid = false; }
                        if (first < group.last) { if (group.valid) stats.overlappingGroups++; group.valid = false; }
                        group.writes.push(command);
                        group.first = Math.min(group.first, first);
                        group.last = Math.max(group.last, last);
                        if (group.writes.length === 2) stats.groupsWithMultipleUploads++;
                    } else stats.uploadsWithoutOrphan++;
                }
            }
            for (const command of work) {
                if (command.skip) continue;
                const [name, args] = command;
                const group = command.batch;
                const merge = group?.valid && group.writes.length > 1 && group.last - group.first <= 16 * 1024 * 1024;
                if (!merge || !group.persistent) native[name](...args);
                if (merge) {
                    // Each recorded upload already updated this shadow. GL consumes the
                    // source synchronously, so the existing span can be submitted
                    // without allocating and filling another merged byte buffer.
                    const merged = group.info.shadow.subarray(group.first, group.last);
                    for (const write of group.writes) write.skip = true;
                    native.bufferSubData(args[0], group.first, merged);
                    stats.batches++;
                    stats.uploadsSaved += group.writes.length - 1;
                    stats.mergedBytes += merged.byteLength;
                }
            }
            stats.flushes++;
            stats.commands += work.length;
            snapshotOffset = 0;
            flushing = false;
        };
        const snapshot = (data, sourceOffset = 0, length = data.length) => {
            const source = sourceOffset || length !== data.length ? data.subarray(sourceOffset, sourceOffset + length) : data;
            const size = source.byteLength;
            if (size > snapshotLimit) return null;
            let offset = (snapshotOffset + 7) & ~7;
            if (offset + size > snapshotLimit || bytes + size > snapshotLimit) {
                flush();
                offset = 0;
            }
            const copy = new data.constructor(snapshots.buffer, offset, source.length);
            copy.set(source);
            snapshotOffset = offset + size;
            bytes += size;
            return copy;
        };
        const updateShadow = (info, offset, data) => {
            if (!info || offset < 0 || offset + data.byteLength > info.size) return;
            const end = offset + data.byteLength;
            if (end > info.shadow.length) {
                // Retain only the used part of a streaming buffer. Most frames
                // use a fraction of the 16 MiB capacity of each ring slot.
                let size = Math.max(65536, info.shadow.length);
                while (size < end) size *= 2;
                const grown = new Uint8Array(Math.min(size, info.size));
                grown.set(info.shadow);
                info.shadow = grown;
            }
            info.shadow.set(data, offset);
        };
        const record = (name, args, buffer, info) => {
            const command = [name, args, buffer, info];
            commands.push(command);
            if (buffer) bufferCommands.push(command);
            if (commands.length > commandLimit || bytes > 16 * 1024 * 1024) flush();
        };
        const simple = [
            "activeTexture", "bindTexture", "bindFramebuffer", "bindRenderbuffer", "bindSampler", "bindBufferBase", "bindBufferRange",
            "blendColor", "blendEquation", "blendEquationSeparate", "blendFunc", "blendFuncSeparate", "clear", "clearBufferfi", "clearColor", "clearDepth", "clearStencil",
            "colorMask", "cullFace", "depthFunc", "depthMask", "depthRange", "disable", "enable", "frontFace", "hint", "lineWidth", "pixelStorei", "polygonOffset", "sampleCoverage", "scissor", "viewport",
            "stencilFunc", "stencilFuncSeparate", "stencilMask", "stencilMaskSeparate", "stencilOp", "stencilOpSeparate",
            "framebufferRenderbuffer", "framebufferTexture2D", "framebufferTextureLayer", "renderbufferStorage", "renderbufferStorageMultisample",
            "samplerParameteri", "samplerParameterf", "texParameteri", "texParameterf", "useProgram",
            "enableVertexAttribArray", "disableVertexAttribArray", "vertexAttribPointer", "vertexAttribIPointer", "vertexAttribDivisor",
            "drawArrays", "drawElements", "drawRangeElements", "drawArraysInstanced", "drawElementsInstanced", "blitFramebuffer",
            "beginQuery", "endQuery", "bindTransformFeedback", "beginTransformFeedback", "endTransformFeedback", "pauseTransformFeedback", "resumeTransformFeedback", "flush", "deleteBuffer", "deleteTexture", "deleteSampler", "deleteProgram", "deleteVertexArray", "deleteFramebuffer", "deleteRenderbuffer", "deleteQuery",
            "uniform1f", "uniform2f", "uniform3f", "uniform4f", "uniform1i", "uniform2i", "uniform3i", "uniform4i", "uniform1ui", "uniform2ui", "uniform3ui", "uniform4ui",
            "vertexAttrib1f", "vertexAttrib2f", "vertexAttrib3f", "vertexAttrib4f", "vertexAttribI4i", "vertexAttribI4ui",
        ];
        for (const name of simple) if (native[name]) {
            gl[name] = name.startsWith("uniform")
                ? (...args) => { if (args[0] !== null) record(name, args); }
                : (...args) => record(name, args);
        }
        for (const name of ["bindBufferBase", "bindBufferRange"]) if (native[name]) {
            gl[name] = (target, index, buffer, ...rest) => {
                if (target === gl.TRANSFORM_FEEDBACK_BUFFER && buffer) {
                    // A pending group must finish before GPU writes can alter its
                    // storage; invalidating only future snapshots is insufficient.
                    flush();
                    storage.delete(buffer);
                }
                buffers.set(target, buffer);
                record(name, [target, index, buffer, ...rest]);
            };
        }
        gl.bindFramebuffer = (target, framebuffer) => {
            if (target === gl.FRAMEBUFFER || target === gl.DRAW_FRAMEBUFFER) drawFramebuffer = framebuffer;
            record("bindFramebuffer", [target, framebuffer]);
        };
        gl.deleteFramebuffer = framebuffer => {
            record("deleteFramebuffer", [framebuffer]);
            if (framebuffer && drawFramebuffer === framebuffer) drawFramebuffer = null;
        };
        gl.blitFramebuffer = (...args) => {
            record("blitFramebuffer", args);
            // Halo's final blit presents its offscreen backbuffer to the canvas.
            // Flush here also when vsync is disabled or the page runs hidden:
            // those refresh paths do not schedule requestAnimationFrame.
            if (drawFramebuffer === null) flush();
        };
        gl.bindVertexArray = next => { vao = next; record("bindVertexArray", [next]); };
        gl.bindBuffer = (target, buffer) => {
            if (target === gl.ELEMENT_ARRAY_BUFFER) elementBuffers.set(vao, buffer);
            else buffers.set(target, buffer);
            record("bindBuffer", [target, buffer]);
        };
        gl.deleteBuffer = buffer => {
            record("deleteBuffer", [buffer]);
            if (!buffer) return;
            storage.delete(buffer);
            for (const [target, current] of buffers) if (current === buffer) buffers.set(target, null);
            if (elementBuffers.get(vao) === buffer) elementBuffers.set(vao, null);
        };
        gl.deleteVertexArray = array => {
            record("deleteVertexArray", [array]);
            if (!array) return;
            elementBuffers.delete(array);
            if (vao === array) vao = null;
        };
        gl.bufferData = (target, data, usage, ...rest) => {
            countKind(allocationKinds, `${target}:${typeof data === "number" ? data : data.byteLength}:${usage}`);
            const buffer = bound(target);
            if (typeof data === "number") {
                stats.numericAllocations++;
                let info;
                if (buffer && streamStorage(target, data, usage)) {
                    info = { size: data, usage, shadow: new Uint8Array(0) };
                    storage.set(buffer, info);
                } else if (buffer) storage.delete(buffer);
                record("bufferData", [target, data, usage], buffer, info);
            } else { stats.typedAllocations++; if (buffer) storage.delete(buffer); flush(); native.bufferData(target, data, usage, ...rest); }
        };
        gl.bufferSubData = (target, offset, data, sourceOffset = 0, length = 0) => {
            countKind(uploadTargets, String(target));
            const unit = data.BYTES_PER_ELEMENT || 1;
            const count = length || (data.byteLength / unit - sourceOffset);
            const source = new Uint8Array(data.buffer, data.byteOffset + sourceOffset * unit, count * unit);
            const copy = snapshot(source);
            const buffer = bound(target), info = buffer && storage.get(buffer);
            if (!copy) {
                // A single oversized write is consumed synchronously instead
                // of exceeding the recorder's memory bound.
                flush();
                if (buffer) storage.delete(buffer);
                native.bufferSubData(target, offset, data, sourceOffset, length);
                return;
            }
            stats.recordBytes += copy.byteLength;
            updateShadow(info, offset, copy);
            record("bufferSubData", [target, offset, copy], buffer, info);
        };
        for (const name of methodNames) {
            if (!native[name]) continue;
            if (/^uniform(?:[1234][fiu]v|Matrix\w+fv)$/.test(name)) {
                gl[name] = (...args) => {
                    if (args[0] === null) return;
                    const dataIndex = name.includes("Matrix") ? 2 : 1;
                    const data = args[dataIndex];
                    const offset = args[dataIndex + 1] || 0;
                    const length = args[dataIndex + 2] || data.length - offset;
                    let copy;
                    if (ArrayBuffer.isView(data)) {
                        copy = snapshot(data, offset, length);
                        if (!copy) { flush(); native[name](...args); return; }
                    } else {
                        copy = data.slice(offset, offset + length);
                        bytes += copy.length * 8;
                    }
                    const result = args.slice(0, dataIndex).concat([copy]);
                    record(name, result);
                };
            } else if (/^clearBuffer(?:fv|iv|uiv)$/.test(name)) {
                gl[name] = (buffer, drawbuffer, values, sourceOffset = 0) => {
                    const count = buffer === gl.COLOR ? 4 : 1;
                    record(name, [buffer, drawbuffer, values.slice(sourceOffset, sourceOffset + count)]);
                };
            } else if (/^vertexAttrib(?:[1234]fv|I4[iu]v)$/.test(name) || name === "drawBuffers") {
                gl[name] = (...args) => record(name, args.map(value => ArrayBuffer.isView(value) ? value.slice() : Array.isArray(value) ? value.slice() : value));
            }
        }
        // These commands either observe mutable GL state or depend on immediate
        // object allocation/transfer. Flush to preserve their synchronous behavior.
        for (const name of methodNames) {
            if (!native[name] || (simple.includes(name) || ["bindVertexArray", "bindBuffer", "bufferData", "bufferSubData"].includes(name) || /^uniform(?:[1234][fiu]v|Matrix\w+fv)$/.test(name) || /^vertexAttrib(?:[1234]fv|I4[iu]v)$/.test(name) || name === "drawBuffers" || /^clearBuffer[fiuv]+$/.test(name))) continue;
            if (/^(create|get|is|check|read|texImage|texSubImage|compressedTex|copy|flush|finish|fenceSync|clientWaitSync|waitSync|invalidate|linkProgram|validateProgram|generateMipmap)/.test(name) &&
                !["getUniformLocation", "getActiveUniform", "getActiveAttrib", "getAttribLocation", "getShaderParameter", "getShaderInfoLog", "getShaderSource", "getShaderPrecisionFormat", "getProgramParameter", "getProgramInfoLog", "getUniformBlockIndex", "getActiveUniformBlockParameter", "getActiveUniformBlockName", "getActiveUniforms", "getUniformIndices", "getExtension", "getSupportedExtensions"].includes(name)) {
                gl[name] = (...args) => { flush(); return native[name](...args); };
            }
        }
        // GPU-side copies bypass our CPU snapshots. Conservatively stop batching
        // that destination until a fresh stream allocation establishes its bytes.
        if (native.copyBufferSubData) gl.copyBufferSubData = (readTarget, writeTarget, ...args) => {
            flush();
            native.copyBufferSubData(readTarget, writeTarget, ...args);
            const destination = bound(writeTarget);
            if (destination) storage.delete(destination);
        };
        const controller = {
            flush,
            snapshot: () => ({ ...stats, commandLimit,
                allocationKinds: Object.fromEntries(allocationKinds), uploadTargets: Object.fromEntries(uploadTargets) }),
            resetStats: () => { for (const key of Object.keys(stats)) stats[key] = 0; },
        };
        installed.set(gl, controller);
        return controller;
    };
    globalThis.HaloStreamBatch = { install };

    // Retain the publisher/Apollo page integration. The reusable installer
    // above has no document, window, location or timer dependency.
    if (typeof document === "undefined" || typeof window === "undefined") return;
    if (new URLSearchParams(location.search).get("batch_streams") === "0") return;
    const canvas = document.getElementById("canvas");
    if (!canvas) return;
    const getContext = canvas.getContext.bind(canvas);
    let wrapped = false;
    canvas.getContext = (type, options) => {
        const gl = getContext(type, options);
        if (!gl || wrapped || type !== "webgl2") return gl;
        wrapped = true;
        const controller = install(gl);
        const requestFrame = window.requestAnimationFrame.bind(window);
        window.requestAnimationFrame = callback => { controller.flush(); return requestFrame(callback); };
        setInterval(() => {
            const output = document.getElementById("performance-stats");
            if (output) output.dataset.streamBatch = JSON.stringify(controller.snapshot());
            controller.resetStats();
        }, 1000);
        return gl;
    };
})();
