/*
WEB_MAIN.C

The web build's entry point, and the WebAssembly memory's layout.

main runs on a pthread (Emscripten's PROXY_TO_PTHREAD), where it may block.
It mounts the site's Origin Private File System, where the page extracted
the game's maps folder (port/web/site/xiso.js) and where the saved games
and config.toml are kept, at /data, then starts the game
(shell_xbox.c's main, renamed halo_game_main by tools/web_build.py).
*/

#include <emscripten.h>
#include <emscripten/wasmfs.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "posix.h"

#include "web_shared.h"

int halo_game_main(void);
void web_js_post(int kind, const char *text);

/* ---------- memory

The Xbox window (port/linux/src/platform.h) is the top 128 MB of the
WebAssembly memory, 0x80000000 to 0x88000000 (tools/web_build.py makes the
memory that size). sbrk grows the C heap up to the heap size this reports,
so reporting the window's start keeps the heap below it. */

#define WEB_HEAP_LIMIT 0x80000000UL

size_t emscripten_get_heap_size(void)
{
	size_t size = (size_t)__builtin_wasm_memory_size(0) << 16;

	return size > WEB_HEAP_LIMIT ? WEB_HEAP_LIMIT : size;
}

/* ---------- start */


static void set_environment(const char *name, const char *value)
{
	/* the page's choice (Module.arguments) wins */
	setenv(name, value, 0);
}

int main(int argc, char **argv)
{
	backend_t opfs;
	struct web_shared_state *shared = web_shared_state();
	char width[32];
	int index;

	/* --NAME=value arguments from the page become environment variables,
	which override config.toml (port/linux/src/port_config.c) */
	for (index = 1; index < argc; index++)
	{
		if (!strncmp(argv[index], "--", 2) && strchr(argv[index], '='))
		{
			char name[128];
			const char *equals = strchr(argv[index], '=');
			size_t length = (size_t)(equals - argv[index] - 2);

			if (length < sizeof(name))
			{
				memcpy(name, argv[index] + 2, length);
				name[length] = '\0';
				setenv(name, equals + 1, 1);
			}
		}
	}

	opfs = wasmfs_create_opfs_backend();
	if (!opfs || wasmfs_create_directory("/data", 0777, opfs) != 0)
	{
		web_js_post(3, "The browser's private storage (OPFS) cannot be opened.");
		return EXIT_FAILURE;
	}
	posix_make_directory("/data/save");

	set_environment("HALO_DATA_ROOT", "/data");
	set_environment("HALO_SAVE_ROOT", "/data/save");
	/* no UDP in a browser: system link and internet play stay off */
	set_environment("HALO_NET_ONLINE", "false");
	set_environment("HALO_UPDATE_AUTO", "false");
	/* 480 lines in the shape of the page (d3d8_gl.c screen_mode_choose) */
	if (shared->display_width > 0 && shared->display_height > 0)
	{
		snprintf(width, sizeof(width), "%d", (int)(480L * shared->display_width / shared->display_height));
		set_environment("HALO_DISPLAY_WIDTH", width);
	}
	web_js_post(0, "starting");
	return halo_game_main();
}
