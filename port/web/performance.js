"use strict";

// Count actual engine presents, not browser animation callbacks.
(() => {
    const canvas = document.getElementById("canvas");
    const params = new URLSearchParams(location.search);
    if (params.has("pixel_ratio")) {
        const ratio = Math.max(0.25, Math.min(2, Number(params.get("pixel_ratio")) || 1));
        Object.defineProperty(window, "devicePixelRatio", { get: () => ratio, configurable: true });
    }
    const calls = {};
    if (params.has("profile")) {
        const getContext = canvas.getContext.bind(canvas);
        let wrapped = false;
        canvas.getContext = (type, options) => {
            const gl = getContext(type, options);
            if (gl && !wrapped && /webgl/.test(type)) {
                wrapped = true;
                const names = new Set([...Object.getOwnPropertyNames(WebGLRenderingContext.prototype), ...Object.getOwnPropertyNames(WebGL2RenderingContext.prototype)]);
                for (const name of names) {
                    if (name === "constructor" || typeof gl[name] !== "function") continue;
                    const call = gl[name].bind(gl);
                    calls[name] = { count: 0, ms: 0 };
                    gl[name] = (...args) => {
                        const begin = performance.now();
                        const result = call(...args);
                        calls[name].count++;
                        calls[name].ms += performance.now() - begin;
                        if (name === "uniform4fv") {
                            const key = args[0] ? `length${args[1].length}` : "null";
                            calls[name][key] = (calls[name][key] || 0) + 1;
                        }
                        return result;
                    };
                }
            }
            return gl;
        };
    }
    const hud = document.getElementById("hud");
    const output = document.createElement("output");
    output.id = "performance-stats";
    output.style.cssText = "padding:4px 10px;font:13px monospace;background:#11161ddd;border-radius:8px";
    output.textContent = "FPS —";
    hud.prepend(output);
    hud.style.opacity = "0.85";
    let last = performance.now(), frames = 0;
    const samples = [];
    setInterval(() => {
        const now = performance.now();
        const presented = window.halo?.haloFramesPresented || 0;
        const fps = (presented - frames) * 1000 / (now - last);
        if (presented && !document.hidden) {
            output.textContent = `${fps.toFixed(1)} FPS`;
            output.title = `${canvas.width} × ${canvas.height} canvas`;
            samples.push({ fps: +fps.toFixed(2), frames: presented, ms: Math.round(now), width: canvas.width, height: canvas.height });
            if (samples.length > 120) samples.shift();
            output.dataset.samples = JSON.stringify(samples);
            if (params.has("profile")) {
                output.dataset.calls = JSON.stringify(calls);
                for (const call of Object.values(calls)) for (const key of Object.keys(call)) call[key] = 0;
            }
        }
        frames = presented;
        last = now;
    }, 1000);
})();
