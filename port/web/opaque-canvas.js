"use strict";

// Xbox rendering often masks alpha writes. Those frames are still fully opaque
// on an Xbox display, but a transparent WebGL canvas hides their RGB on the web.
// Choose an opaque drawing buffer before SDL/Emscripten creates the context.
(() => {
    const canvas = document.getElementById("canvas");
    const getContext = canvas.getContext.bind(canvas);
    canvas.getContext = (type, options) => getContext(type,
        /^(webgl2?|experimental-webgl)$/.test(type)
            ? { ...options, alpha: false }
            : options);
})();
