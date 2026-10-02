# SMAA

Enhanced Subpixel Morphological Antialiasing, by Jorge Jimenez, Jose I.
Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez, MIT licensed
(see `LICENSE`).

Upstream: https://github.com/iryoku/smaa, commit
71c806a838bdd7d517df19192a20f0c61b3ca29d. `SMAA.hlsl` is copied
unchanged. `area_tex.zlib` and `search_tex.zlib` are the bytes of
`Textures/AreaTex.h` (160x560, two channels) and `Textures/SearchTex.h`
(64x16, one channel), compressed with zlib:

    python -c "import re, sys, zlib; t = open(sys.argv[1]).read(); \
    open(sys.argv[2], 'wb').write(zlib.compress(bytes(int(h, 16) for h in \
    re.findall(r'0x([0-9a-fA-F]{2})', t[t.index('{'):])), 9))" AreaTex.h area_tex.zlib

`display.anti_aliasing = "smaa"` antialiases the 3D view with it, compiled
as GLSL (`port/linux/src/xgpu_post.c`); the files are embedded in the Linux,
Windows and Android platform layers by `tools/embed_assets.py`.
