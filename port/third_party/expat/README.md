# Expat

A streaming XML parser in C, by James Clark, Sebastian Pipping and its other
authors, MIT licensed (see `COPYING`).

Upstream: https://github.com/libexpat/libexpat, release `R_2_8_5`
(`expat-2.8.5.tar.xz`, SHA-256
1e727b8933ec51a77a9a9d9afcf8e688bce45d907c13e36ab7393fe36e703182, signed by
Sebastian Pipping). The `.c` and `.h` files are copied unchanged from its
`lib/` directory (the parser, the tokenizer and Windows' `rand_s` entropy);
`expat_config.h` is ours.

The native ports read the menus' XML files with it
(`port/linux/src/menu_files.c`); it builds into the Linux, Windows and
Android platform layers.
