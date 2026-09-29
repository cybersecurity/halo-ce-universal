#include "gl_native_proc.h"

#include <SDL3/SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define GL_PROC_CACHE_CAPACITY 256
#define GL_PROC_NAME_CAPACITY 128

struct gl_proc_cache_entry
{
	SDL_GLContext context;
	char name[GL_PROC_NAME_CAPACITY];
	void *address;
	unsigned hash;
	int occupied;
};

static struct gl_proc_cache_entry gl_proc_cache[GL_PROC_CACHE_CAPACITY];
static pthread_mutex_t gl_proc_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static int no_context_reported;

void mac_macos_gl_native_proc_clear(void)
{
	pthread_mutex_lock(&gl_proc_cache_mutex);
	memset(gl_proc_cache, 0, sizeof(gl_proc_cache));
	no_context_reported = 0;
	pthread_mutex_unlock(&gl_proc_cache_mutex);
}

void *mac_macos_gl_native_proc(const char *name)
{
	SDL_GLContext context;
	size_t name_length;
	size_t free_slot = GL_PROC_CACHE_CAPACITY;
	void *address;
	unsigned hash = 2166136261u;

	if (!name || !name[0])
		return NULL;
	name_length = strlen(name);
	if (name_length >= GL_PROC_NAME_CAPACITY)
	{
		fprintf(stderr, "[mac-gl] refusing overlong GL entry-point name\n");
		return NULL;
	}
	for (size_t index = 0; index < name_length; ++index)
		hash = (hash ^ (unsigned char)name[index]) * 16777619u;
	context = SDL_GL_GetCurrentContext();
	if (!context)
	{
		pthread_mutex_lock(&gl_proc_cache_mutex);
		if (!no_context_reported)
		{
			fprintf(stderr, "[mac-gl] GL proc lookup requested without a current SDL context\n");
			no_context_reported = 1;
		}
		pthread_mutex_unlock(&gl_proc_cache_mutex);
		return NULL;
	}

	pthread_mutex_lock(&gl_proc_cache_mutex);
	/* Look up by name hash instead of comparing every cached GL name on
	 * each draw. Context remains part of the key; clear still invalidates all. */
	for (size_t probe = 0; probe < GL_PROC_CACHE_CAPACITY; probe++)
	{
		size_t index = (hash + probe) % GL_PROC_CACHE_CAPACITY;
		struct gl_proc_cache_entry *entry = &gl_proc_cache[index];
		if (!entry->occupied)
		{
			if (free_slot == GL_PROC_CACHE_CAPACITY)
				free_slot = index;
			break;
		}
		if (entry->hash == hash && entry->context == context && strcmp(entry->name, name) == 0)
		{
			address = entry->address;
			pthread_mutex_unlock(&gl_proc_cache_mutex);
			return address;
		}
	}

	/* SDL requires a current GL context for portable proc-address lookup. */
	SDL_FunctionPointer function = SDL_GL_GetProcAddress(name);
	address = (void *)function;
	if (!address)
		fprintf(stderr, "[mac-gl] SDL has no current-context entry point for %s: %s\n",
			name, SDL_GetError());
	if (free_slot == GL_PROC_CACHE_CAPACITY)
	{
		pthread_mutex_unlock(&gl_proc_cache_mutex);
		fprintf(stderr, "[mac-gl] GL proc cache is full; cannot cache %s\n", name);
		return address;
	}
	struct gl_proc_cache_entry *entry = &gl_proc_cache[free_slot];
	entry->context = context;
	memcpy(entry->name, name, name_length + 1);
	entry->address = address;
	entry->hash = hash;
	entry->occupied = 1;
	pthread_mutex_unlock(&gl_proc_cache_mutex);
	return address;
}
