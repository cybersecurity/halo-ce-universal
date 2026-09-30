/*
HOST_MAIN.C

Entry point of the macOS port.

It loads the guest image (the game, built as x32 code: halo_guest.elf next
to the executable), gives it an environment and its command line, points SDL
at ANGLE (OpenGL ES over Metal, libEGL.dylib and libGLESv2.dylib next to the
executable) and runs the game's main() on the process's main thread, on a
stack in guest memory (host_thread.c): Cocoa wants the window and its events
there.

The game's folder holds maps/ (extracted there on first start from the
player's disc image), config.toml and the logs (debug.txt, the game's;
host.txt, this file's). It is the folder holding the executable, as in the
Linux build, except in the application bundle (Halo.app), whose own folder
is read-only once signed: there it is ~/Library/Application Support/Halo.
The saved games go to ~/Library/Application Support/Halo either way
(port/linux/src/xbox_files.c).
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern void *host_gles_library;
int host_gl_load(void);

char host_data_root[1024];
char host_executable_path[1024];
/* the folder holding the executable, the guest image and ANGLE */
static char image_folder[1024];

/* ---------- logging and termination */

static FILE *log_file;
/* stderr goes to host.txt too (not from a terminal: from Finder or open) */
static int stderr_to_log;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void host_log(int priority, const char *text)
{
	static const char *const names[] = { "", "", "", "", "info", "warning", "error" };
	const char *name = priority >= 0 && priority <= 6 ? names[priority] : "";

	pthread_mutex_lock(&log_lock);
	if (!stderr_to_log)
		fprintf(stderr, "halo host %s: %s\n", name, text);
	if (log_file)
	{
		fprintf(log_file, "%s: %s\n", name, text);
		fflush(log_file);
	}
	pthread_mutex_unlock(&log_lock);
}

void host_logf(int priority, const char *format, ...)
{
	char message[2048];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_log(priority, message);
}

void host_fatal(const char *format, ...)
{
	char message[2048];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_log(HOST_LOG_ERROR, message);
	/* (not for runs nobody watches: debug.hidden_window's variable) */
	if (!getenv("HALO_HIDDEN_WINDOW"))
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", message, NULL);
	_exit(1);
}

void host_abort(const char *reason)
{
	host_logf(HOST_LOG_ERROR, "guest abort: %s", reason);
	abort();
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	host_profile_write();
	SDL_Quit();
	_exit(code);
}

/* the Android port's storage paths; the desktop platform layer finds its
folders itself, so these are the game's folder */
void host_android_path(int which, char *buffer, uint32_t size)
{
	(void)which;
	snprintf(buffer, size, "%s", host_data_root);
}

/* ---------- handles for host pointers the guest keeps (the thunks of
functions returning them, tools/macos_host_thunks.py) */

#define HANDLE_COUNT 4096

static void *handle_pointers[HANDLE_COUNT];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t host_handle_new(void *pointer)
{
	uint32_t index;

	if (!pointer)
		return 0;
	pthread_mutex_lock(&handle_lock);
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (!handle_pointers[index])
		{
			handle_pointers[index] = pointer;
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	pthread_mutex_unlock(&handle_lock);
	host_logf(HOST_LOG_ERROR, "out of handles for host pointers");
	return 0;
}

void *host_handle_get(uint32_t handle)
{
	void *pointer = NULL;

	if (handle > 0 && handle < HANDLE_COUNT)
	{
		pthread_mutex_lock(&handle_lock);
		pointer = handle_pointers[handle];
		pthread_mutex_unlock(&handle_lock);
	}
	return pointer;
}

void host_handle_release(uint32_t handle)
{
	if (handle > 0 && handle < HANDLE_COUNT)
	{
		pthread_mutex_lock(&handle_lock);
		handle_pointers[handle] = NULL;
		pthread_mutex_unlock(&handle_lock);
	}
}

/* ---------- debugging hooks (the Android port's sampler is not needed:
lldb attaches to Rosetta processes) */

void host_debug_thread_started(void)
{
}

void host_debug_thread_exited(void)
{
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 128
#define ARGUMENT_MAXIMUM 16

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	size_t length = strlen(name);
	char *entry;
	int index;

	entry = malloc(length + strlen(value) + 2);
	sprintf(entry, "%s=%s", name, value);
	for (index = 0; index < environment->count; index++)
	{
		if (!strncmp(environment->entries[index], name, length) && environment->entries[index][length] == '=')
		{
			free(environment->entries[index]);
			environment->entries[index] = entry;
			return;
		}
	}
	if (environment->count < ENVIRONMENT_MAXIMUM)
		environment->entries[environment->count++] = entry;
	else
		free(entry);
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
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* copies argv and the environment into guest memory */
static uint32_t make_boot(int argc, char **argv, const struct environment *environment)
{
	size_t size = 0x20000;
	char *memory = host_low_map(size, PROT_READ | PROT_WRITE);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *guest_argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = guest_argv + ARGUMENT_MAXIMUM + 1;
	char *strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	int index, count = 0;

	if (!memory)
		host_fatal("cannot allocate the guest's environment");
	for (index = 0; index < argc && count < ARGUMENT_MAXIMUM; index++)
	{
		const char *text = index == 0 ? host_executable_path : argv[index];
		size_t length = strlen(text) + 1;

		/* macOS adds -psn_... when started from the Finder */
		if (index > 0 && !strncmp(text, "-psn_", 5))
			continue;
		if (strings + length > memory + size)
			break;
		memcpy(strings, text, length);
		guest_argv[count++] = GUEST_ADDRESS(strings);
		strings += length;
	}
	guest_argv[count] = 0;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = GUEST_ADDRESS(strings);
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = (uint32_t)count;
	boot->argv = GUEST_ADDRESS(guest_argv);
	boot->environment = GUEST_ADDRESS(environ_list);
	boot->page_size = (uint32_t)getpagesize();
	return GUEST_ADDRESS(boot);
}

/* ---------- start-up */

static void find_folders(void)
{
	char path[PATH_MAX];
	char resolved[PATH_MAX];
	uint32_t size = sizeof(path);
	char *slash;

	if (_NSGetExecutablePath(path, &size) != 0 || !realpath(path, resolved))
		host_fatal("cannot find the executable's folder");
	snprintf(image_folder, sizeof(image_folder), "%s", resolved);
	slash = strrchr(image_folder, '/');
	if (slash)
		*slash = 0;
	snprintf(host_data_root, sizeof(host_data_root), "%s", image_folder);
	if (strstr(image_folder, ".app/Contents/MacOS") && getenv("HOME"))
	{
		char folder[1024];

		snprintf(folder, sizeof(folder), "%s/Library/Application Support", getenv("HOME"));
		mkdir(folder, 0755);
		snprintf(host_data_root, sizeof(host_data_root), "%s/Halo", folder);
		mkdir(host_data_root, 0755);
	}
	/* the guest's platform layer looks for its folder through
	/proc/self/exe (host_syscall.c) */
	snprintf(host_executable_path, sizeof(host_executable_path), "%s/halo", host_data_root);
}

static void *read_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	void *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		data = malloc((size_t)length);
		if (data && fread(data, 1, (size_t)length, file) == (size_t)length)
		{
			*size = (size_t)length;
		}
		else
		{
			free(data);
			data = NULL;
		}
	}
	fclose(file);
	return data;
}

static void load_angle(void)
{
	char egl[1200], gles[1200];

	snprintf(egl, sizeof(egl), "%s/libEGL.dylib", image_folder);
	snprintf(gles, sizeof(gles), "%s/libGLESv2.dylib", image_folder);
	host_gles_library = dlopen(gles, RTLD_NOW | RTLD_GLOBAL);
	if (!host_gles_library)
		host_fatal("cannot load OpenGL ES (ANGLE) from %s: %s", gles, dlerror());
	if (!host_gl_load())
		host_fatal("%s lacks OpenGL ES 3 entry points", gles);
	SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, gles);
	SDL_SetHint(SDL_HINT_EGL_LIBRARY, egl);
	/* ANGLE on Metal (its default on macOS) */
	setenv("ANGLE_DEFAULT_PLATFORM", "metal", 0);
}

int main(int argc, char *argv[])
{
	struct environment environment = { { 0 }, 0 };
	char path[1200];
	char zone[64];
	void *image;
	size_t image_size = 0;
	char **host_environment = *_NSGetEnviron();
	int index;

	if (host_memory_reserve() != 0)
	{
		fprintf(stderr, "halo: cannot reserve the game's 4 GB of address space\n");
		return 1;
	}
	find_folders();
	if (chdir(host_data_root) != 0)
		host_fatal("cannot enter %s", host_data_root);
	/* the last run's log kept, as host.old.txt: why it ended */
	{
		char old_path[1210];

		snprintf(path, sizeof(path), "%s/host.txt", host_data_root);
		snprintf(old_path, sizeof(old_path), "%s/host.old.txt", host_data_root);
		rename(path, old_path);
	}
	log_file = fopen(path, "w");
	/* the game's own messages (platform_log: why it quit, for one) go to
	stderr, which nothing shows unless a terminal started the game */
	if (log_file && !isatty(STDERR_FILENO) && dup2(fileno(log_file), STDERR_FILENO) >= 0)
	{
		setvbuf(stderr, NULL, _IOLBF, 0);
		stderr_to_log = 1;
	}
	host_logf(HOST_LOG_INFO, "Halo for macOS starting in %s", host_data_root);
	host_install_signal_handlers();
	load_angle();

	/* the environment: the player's HALO_* settings (port_config.c),
	HOME, TMPDIR (where the Discord app's socket is, for its invites:
	posix_discord_connect), and a TZ musl understands */
	for (index = 0; host_environment[index]; index++)
	{
		const char *entry = host_environment[index];

		if (!strncmp(entry, "HALO_", 5) || !strncmp(entry, "HOME=", 5) || !strncmp(entry, "USER=", 5) ||
			!strncmp(entry, "LANG=", 5) || !strncmp(entry, "TMPDIR=", 7))
		{
			const char *equals = strchr(entry, '=');
			char name[256];

			if (!equals || equals - entry >= (long)sizeof(name))
				continue;
			memcpy(name, entry, (size_t)(equals - entry));
			name[equals - entry] = 0;
			environment_set(&environment, name, equals + 1);
		}
	}
	/* the command line's settings (--name=value, --name value, --name,
	--no-name: port_config.c's), one a line, as HALO_SETTINGS; the rest of
	the line goes to the game */
	{
		static char settings[4096];
		size_t used = 0;
		int kept = 1;

		for (index = 1; index < argc; index++)
		{
			const char *arg = argv[index];
			int written;

			if (strncmp(arg, "--", 2) || !arg[2])
			{
				argv[kept++] = argv[index];
				continue;
			}
			arg += 2;
			/* (a value after it, not itself a flag) */
			if (!strchr(arg, '=') && index + 1 < argc && strncmp(argv[index + 1], "--", 2) &&
				strncmp(arg, "no-", 3))
			{
				written = snprintf(settings + used, sizeof(settings) - used, "%s=%s\n", arg, argv[index + 1]);
				index++;
			}
			else
				written = snprintf(settings + used, sizeof(settings) - used, "%s\n", arg);
			if (written > 0 && used + (size_t)written < sizeof(settings))
				used += (size_t)written;
		}
		argc = kept;
		if (used)
			environment_set(&environment, "HALO_SETTINGS", settings);
	}
	snprintf(path, sizeof(path), "%s/", host_data_root);
	environment_set(&environment, "HALO_BASE_PATH", path);
	time_zone(zone, sizeof(zone));
	environment_set(&environment, "TZ", zone);

	/* next to the executable, or in the application bundle's Resources
	(signed code goes in Contents/MacOS, data in Contents/Resources) */
	snprintf(path, sizeof(path), "%s/halo_guest.elf", image_folder);
	image = read_file(path, &image_size);
	if (!image)
	{
		snprintf(path, sizeof(path), "%s/../Resources/halo_guest.elf", image_folder);
		image = read_file(path, &image_size);
	}
	if (!image)
		host_fatal("cannot read the game image %s", path);
	if (host_load_image(image, image_size) != 0)
		host_fatal("cannot load the game image; see %s/host.txt for details", host_data_root);
	free(image);

	host_profile_start();
	host_run_guest_main(make_boot(argc, argv, &environment));
}
