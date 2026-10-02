"use strict";

// Load AFTER stream-batch.js to suppress redundant state changes before their
// arguments are copied and queued. Use ?replay_cache=0 to disable for comparison.
// No buffer uploads or draw commands are moved, combined, or suppressed here.
(() => {
    if (new URLSearchParams(location.search).get("replay_cache") === "0") return;
    const canvas = document.getElementById("canvas");
    const getContext = canvas.getContext.bind(canvas);
    let wrapped = false;
    canvas.getContext = (type, options) => {
        const gl = getContext(type, options);
        if (!gl || wrapped || type !== "webgl2") return gl;
        wrapped = true;
        const native = {};
        const methods = new Set([
            ...Object.getOwnPropertyNames(WebGLRenderingContext.prototype),
            ...Object.getOwnPropertyNames(WebGL2RenderingContext.prototype),
        ]);
        for (const name of methods)
            if (name !== "constructor" && typeof gl[name] === "function") native[name] = gl[name].bind(gl);
        const stats = { uniformCalls: 0, uniformSkipped: 0, uniformBytesSkipped: 0,
            stateCalls: 0, stateSkipped: 0, attributeCalls: 0, attributeSkipped: 0 };
        let programs, locations, scalars, capabilities, constants, samplerValues;
        let currentProgram;
        const reset = () => {
            programs = new WeakMap(); locations = new WeakMap(); samplerValues = new WeakMap();
            scalars = new Map(); capabilities = new Map(); constants = new Map();
            currentProgram = undefined;
        };
        reset();
        const same = (a, b) => {
            if (!a || a.length !== b.length) return false;
            for (let index = 0; index < b.length; index++)
                if (!Object.is(a[index], b[index])) return false;
            return true;
        };
        const stateCall = (key, values, method, args) => {
            stats.stateCalls++;
            if (same(scalars.get(key), values)) { stats.stateSkipped++; return; }
            native[method](...args);
            scalars.set(key, values);
        };
        // These states have exactly one setter each. Setter pairs which affect
        // overlapping state are handled together below or left uncached.
        for (const name of ["depthFunc", "depthMask", "depthRange", "cullFace", "frontFace",
            "colorMask", "viewport", "scissor", "polygonOffset", "clearColor", "clearDepth",
            "clearStencil", "lineWidth", "blendColor", "sampleCoverage", "activeTexture"])
            if (native[name]) gl[name] = (...args) => stateCall(name, args, name, args);
        for (const name of ["enable", "disable"]) if (native[name]) gl[name] = capability => {
            stats.stateCalls++;
            const value = name === "enable";
            if (capabilities.get(capability) === value) { stats.stateSkipped++; return; }
            native[name](capability); capabilities.set(capability, value);
        };
        if (native.blendFunc) gl.blendFunc = (source, destination) =>
            stateCall("blendFactors", [source, destination, source, destination], "blendFunc", [source, destination]);
        if (native.blendFuncSeparate) gl.blendFuncSeparate = (...args) => stateCall("blendFactors", args, "blendFuncSeparate", args);
        if (native.blendEquation) gl.blendEquation = mode => stateCall("blendEquations", [mode, mode], "blendEquation", [mode]);
        if (native.blendEquationSeparate) gl.blendEquationSeparate = (...args) => stateCall("blendEquations", args, "blendEquationSeparate", args);

        gl.useProgram = program => {
            // A link/delete can change whether useProgram succeeds, so those
            // operations invalidate this cache even if the object is unchanged.
            stateCall("useProgram", [program], "useProgram", [program]);
            currentProgram = program;
        };
        const splitName = name => {
            const match = /^(\w+)(?:\[(\d+)\])?$/.exec(name);
            return match ? { base: match[1], index: Number(match[2] || 0) } : null;
        };
        const inspectProgram = program => {
            const record = { groups: new Map(), live: true };
            programs.set(program, record);
            if (!native.getProgramParameter(program, gl.LINK_STATUS)) return record;
            const count = native.getProgramParameter(program, gl.ACTIVE_UNIFORMS);
            for (let index = 0; index < count; index++) {
                const info = native.getActiveUniform(program, index);
                const name = info && splitName(info.name);
                // Halo uses plain float/vec4 arrays and scalar int samplers.
                // Unknown shapes remain on the untouched native path.
                const width = info?.type === gl.FLOAT_VEC4 ? 4 : info?.type === gl.FLOAT ? 1 : 0;
                const integer = info && [gl.INT, gl.BOOL, gl.SAMPLER_2D, gl.SAMPLER_CUBE, gl.SAMPLER_3D,
                    gl.SAMPLER_2D_ARRAY, gl.SAMPLER_2D_SHADOW, gl.SAMPLER_CUBE_SHADOW,
                    gl.INT_SAMPLER_2D, gl.UNSIGNED_INT_SAMPLER_2D].includes(info.type);
                if (!name || (!width && !integer)) continue;
                const components = width || 1;
                record.groups.set(name.base, { width: components, integer, size: info.size,
                    values: integer ? new Int32Array(info.size * components) : new Float32Array(info.size * components),
                    valid: new Uint8Array(info.size * components) });
            }
            return record;
        };
        gl.linkProgram = program => {
            const old = programs.get(program);
            if (old) old.live = false;
            scalars.delete("useProgram");
            native.linkProgram(program);
            inspectProgram(program);
        };
        gl.deleteProgram = program => {
            const old = program && programs.get(program);
            if (old) old.live = false;
            if (program) programs.delete(program);
            scalars.delete("useProgram");
            return native.deleteProgram(program);
        };
        gl.getUniformLocation = (program, name) => {
            const location = native.getUniformLocation(program, name);
            const parsed = splitName(name), record = program && programs.get(program);
            const group = parsed && record?.groups.get(parsed.base);
            if (location && group && parsed.index < group.size)
                locations.set(location, { program, record, group, start: parsed.index * group.width });
            return location;
        };
        const entryFor = location => {
            const entry = location && locations.get(location);
            return entry?.record.live && entry.program === currentProgram ? entry : null;
        };
        const invalidate = location => {
            const entry = location && locations.get(location);
            if (entry?.record.live) entry.group.valid.fill(0);
        };
        // Track every component of a uniform array, not just its location or
        // last argument list: c[0] + count can overlap later c[17] uploads.
        const upload = (name, location, data, offset, count, args, width, integer) => {
            stats.uniformCalls++;
            if (location === null) { stats.uniformSkipped++; return; }
            const entry = entryFor(location);
            const end = entry ? entry.start + count : 0;
            if (!entry || entry.group.width !== width || !!entry.group.integer !== integer ||
                !Number.isInteger(offset) || !Number.isInteger(count) || offset < 0 || count <= 0 ||
                count % width || offset + count > data.length || end > entry.group.values.length) {
                invalidate(location);
                return native[name](...args);
            }
            const { group, start } = entry;
            // Emscripten's Float32Array already contains rounded float32 values.
            // Other realms/ordinary arrays use the original conversion path.
            const float32 = !integer && data instanceof Float32Array;
            let equal = true;
            for (let index = 0; index < count; index++) {
                const value = float32 ? data[offset + index] : integer ? data[offset + index] | 0 : Math.fround(data[offset + index]);
                if (!group.valid[start + index] || !Object.is(group.values[start + index], value)) { equal = false; break; }
            }
            if (equal) { stats.uniformSkipped++; stats.uniformBytesSkipped += count * 4; return; }
            native[name](...args);
            for (let index = 0; index < count; index++) {
                group.values[start + index] = float32 ? data[offset + index] : integer ? data[offset + index] | 0 : Math.fround(data[offset + index]);
                group.valid[start + index] = 1;
            }
        };
        for (const name of ["uniform4fv", "uniform1fv", "uniform1iv"]) if (native[name]) gl[name] = (...args) => {
            const [location, data, offset = 0, length = 0] = args;
            if (!data || !Number.isInteger(data.length)) { invalidate(location); return native[name](...args); }
            upload(name, location, data, offset, length || data.length - offset, args, name === "uniform4fv" ? 4 : 1, name === "uniform1iv");
        };
        for (const name of ["uniform1f", "uniform1i", "uniform4f"]) if (native[name]) gl[name] = (...args) =>
            upload(name, args[0], args.slice(1), 0, args.length - 1, args, name === "uniform4f" ? 4 : 1, name === "uniform1i");
        for (const name of methods) if (/^uniform(?:[1234][fiu]v?|Matrix\w+fv)$/.test(name) && native[name] &&
            !["uniform4fv", "uniform1fv", "uniform1iv", "uniform1f", "uniform1i", "uniform4f"].includes(name))
            gl[name] = (...args) => { invalidate(args[0]); return native[name](...args); };

        // Keep the common repeated FLOAT constant-attribute path allocation-free.
        // This retains Object.is semantics for NaN and signed zero.
        const sameConstant = (index, x, y, z, w) => {
            const value = constants.get(index);
            return value && Object.is(value[0], x) && Object.is(value[1], y) &&
                Object.is(value[2], z) && Object.is(value[3], w);
        };
        const rememberConstant = (index, x, y, z, w) => {
            let value = constants.get(index);
            if (!value) { value = [0, 0, 0, 1]; constants.set(index, value); }
            value[0] = x; value[1] = y; value[2] = z; value[3] = w;
        };
        for (const name of methods) {
            const match = /^vertexAttrib([1234])(f|fv)$/.exec(name);
            if (!match || !native[name]) continue;
            const width = Number(match[1]), vector = match[2] === "fv";
            if (vector) gl[name] = (index, data) => {
                stats.attributeCalls++;
                if (!data || data.length < width) { constants.delete(index); return native[name](index, data); }
                const float32 = data instanceof Float32Array;
                const x = float32 ? data[0] : Math.fround(data[0]);
                const y = width > 1 ? float32 ? data[1] : Math.fround(data[1]) : 0;
                const z = width > 2 ? float32 ? data[2] : Math.fround(data[2]) : 0;
                const w = width > 3 ? float32 ? data[3] : Math.fround(data[3]) : 1;
                if (sameConstant(index, x, y, z, w)) { stats.attributeSkipped++; return; }
                native[name](index, data);
                rememberConstant(index, x, y, z, w);
            };
            else gl[name] = (index, a, b, c, d) => {
                stats.attributeCalls++;
                const x = Math.fround(a), y = width > 1 ? Math.fround(b) : 0;
                const z = width > 2 ? Math.fround(c) : 0, w = width > 3 ? Math.fround(d) : 1;
                if (sameConstant(index, x, y, z, w)) { stats.attributeSkipped++; return; }
                if (width === 1) native[name](index, a);
                else if (width === 2) native[name](index, a, b);
                else if (width === 3) native[name](index, a, b, c);
                else native[name](index, a, b, c, d);
                rememberConstant(index, x, y, z, w);
            };
        }
        for (const name of ["vertexAttribI4i", "vertexAttribI4ui", "vertexAttribI4iv", "vertexAttribI4uiv"])
            if (native[name]) gl[name] = (...args) => { constants.delete(args[0]); return native[name](...args); };
        for (const name of ["samplerParameteri", "samplerParameterf"]) if (native[name]) gl[name] = (sampler, pname, value) => {
            stats.stateCalls++;
            if (!sampler) return native[name](sampler, pname, value);
            let parameters = samplerValues.get(sampler);
            if (!parameters) samplerValues.set(sampler, parameters = new Map());
            // Keep setter kinds separate: integer and float conversion can differ.
            const previous = parameters.get(pname);
            if (previous?.name === name && Object.is(previous.value, value)) { stats.stateSkipped++; return; }
            native[name](sampler, pname, value); parameters.set(pname, { name, value });
        };
        if (native.deleteSampler) gl.deleteSampler = sampler => {
            if (sampler) samplerValues.delete(sampler);
            return native.deleteSampler(sampler);
        };
        canvas.addEventListener("webglcontextlost", reset);
        canvas.addEventListener("webglcontextrestored", reset);
        setInterval(() => {
            const output = document.getElementById("performance-stats");
            if (output) output.dataset.replayCache = JSON.stringify(stats);
        }, 1000);
        return gl;
    };
})();
