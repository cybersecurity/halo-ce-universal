/*
HOST_MAIN.C

Entry point of the 64-bit ARM Linux build (tools/linux_arm64_build.py).

The host is the Android port's (port/android/host, port/android/README.md),
with this file in place of its host_main.c: it loads the guest image (the
game, built as ILP32 code) from inside the executable (guest_image.S), and
runs its main() on a thread of its own with its stack in guest memory
(host_thread.c). The main thread waits; the game ends the process.

The guest is the Linux desktop build of the game, so it finds the game data,
the saves and config.toml as the x86 build does (port/linux/README.md): it
gets the part of this process's environment that it reads (the HALO_
settings, HOME, the XDG folders), and /proc/self/exe is this executable.
*/

#include "host.h"
#include "tomlc17.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

void host_install_signal_handlers(void);

/* guest_image.S */
extern const unsigned char halo_guest_image_start[];
extern const unsigned char halo_guest_image_end[];

extern char **environ;

/* ---------- logging and termination */

static const char *priority_name(int priority)
{
	return priority >= HOST_LOG_ERROR ? "error: " : priority >= HOST_LOG_WARN ? "warning: " : "";
}

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	fprintf(stderr, "halo-host: %s", priority_name(priority));
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

void host_log(int priority, const char *text)
{
	fprintf(stderr, "halo-host: %s%s\n", priority_name(priority), text);
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	fprintf(stderr, "halo-host: fatal: %s\n", message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", message, NULL);
	_exit(1);
}

void host_abort(const char *reason)
{
	fprintf(stderr, "halo-host: guest abort: %s\n", reason);
	abort();
}

void host_exit(int code)
{
	fflush(stdout);
	fflush(stderr);
	_exit(code);
}

int host_errno(void)
{
	return errno;
}

/* the app's folders (host_imports.list); the desktop guest finds its own */
void host_android_path(int which, char *buffer, uint32_t size)
{
	(void)which;
	if (size)
		buffer[0] = 0;
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 128

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_add(struct environment *environment, const char *entry)
{
	if (environment->count < ENVIRONMENT_MAXIMUM)
		environment->entries[environment->count++] = strdup(entry);
}

/* what the game reads: its settings (HALO_...), where the saves go
(port/linux/src/xbox_files.c) and where Discord's socket is (posix_net.c);
the display and sound are the host's. TZ is game_main's. */
static int environment_passes(const char *entry)
{
	static const char *const prefixes[] =
	{
		"HALO_", "HOME=", "XDG_", "USER=", "LANG=", "LC_", "TMPDIR=", "TMP=", "TEMP=",
	};
	size_t index;

	for (index = 0; index < sizeof(prefixes) / sizeof(prefixes[0]); index++)
	{
		if (!strncmp(entry, prefixes[index], strlen(prefixes[index])))
			return 1;
	}
	return 0;
}

/* POSIX TZ for the current local offset (the guest's musl has no zone
database) */
static void time_zone(char *buffer, size_t size)
{
	time_t now = time(NULL);
	struct tm local;
	long offset;

	localtime_r(&now, &local);
	offset = -local.tm_gmtoff;
	snprintf(buffer, size, "TZ=<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* copies argv and the environment into guest memory */
static uint32_t make_boot(const struct environment *environment)
{
	size_t size = 0x10000;
	char *memory = host_low_map(size, PROT_READ | PROT_WRITE);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = argv + 2;
	char *strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	int index;

	if (!memory)
		host_fatal("cannot allocate the guest's environment");
	strcpy(strings, "halo");
	argv[0] = (uint32_t)(uintptr_t)strings;
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = (uint32_t)(uintptr_t)strings;
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)environ_list;
	boot->page_size = (uint32_t)getpagesize();
	return (uint32_t)(uintptr_t)boot;
}

/* debug.sample_seconds from config.toml (next to the executable, as the
game reads it), as text for the sampler, or 0 */
static int config_sample_seconds(char *text, size_t size)
{
	const char *base = SDL_GetBasePath();
	char path[1024];
	toml_result_t result;
	int found = 0;

	snprintf(path, sizeof(path), "%sconfig.toml", base ? base : "");
	result = toml_parse_file_ex(path);
	if (!result.ok)
		return 0;
	{
		toml_datum_t seconds = toml_seek(result.toptab, "debug.sample_seconds");
		double value = seconds.type == TOML_FP64 ? seconds.u.fp64 :
			seconds.type == TOML_INT64 ? (double)seconds.u.int64 : 0.0;

		if (value > 0.0)
		{
			snprintf(text, size, "%g", value);
			found = 1;
		}
	}
	toml_free(result);
	return found;
}

/* ---------- main */

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

static void *game_main(void *unused)
{
	struct environment environment = { { 0 }, 0 };
	char zone[64];
	char seconds[32];
	char **entry;

	(void)unused;
	for (entry = environ; *entry; entry++)
	{
		if (environment_passes(*entry))
			environment_add(&environment, *entry);
	}
	time_zone(zone, sizeof(zone));
	environment_add(&environment, zone);

	if (host_load_image(halo_guest_image_start, (size_t)(halo_guest_image_end - halo_guest_image_start)) != 0)
		host_fatal("cannot load the game image");
	if (config_sample_seconds(seconds, sizeof(seconds)))
		host_debug_start_sampler(seconds);
	host_run_guest_main(make_boot(&environment));
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	host_install_signal_handlers();
	if (host_native_thread_create(game_main, NULL, MAIN_STACK_SIZE) != 0)
		host_fatal("cannot start the game thread");
	/* the game ends the process itself (host_exit) */
	for (;;)
		pause();
}
