/*
HOST_PROFILE.C

A sampling profiler for the macOS port (HALO_PROFILE=1): a thread stops
every other thread of the process a thousand times a second (Mach's
thread_suspend and thread_get_state), notes where each was, and when the
game exits writes profile.txt to the game's folder: the functions the
samples fell in, most first, per thread and in all. Guest addresses are
named from the guest image's own symbol table (host_profile_load_symbols,
which host_loader.c calls), host ones (SDL, ANGLE, the system) by dladdr.

This finds what to optimise; port/macos/tests/run_determinism_test.sh
checks that an optimisation leaves the game's results alike.
*/

#include "host.h"

#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXIMUM_SYMBOLS 65536
#define MAXIMUM_BUCKETS 8192
#define MAXIMUM_THREADS 64

struct symbol
{
	uint32_t address;
	uint32_t size;
	const char *name;
};

struct bucket
{
	const char *name;
	int guest;
	unsigned long samples;
	unsigned long thread_samples[MAXIMUM_THREADS];
};

static struct symbol *symbols;
static int symbol_count;
static char *symbol_names;
static struct bucket buckets[MAXIMUM_BUCKETS];
static int bucket_count;
static unsigned long total_samples, thread_totals[MAXIMUM_THREADS];
static thread_act_t known_threads[MAXIMUM_THREADS];
static int known_thread_count;
static int profiling;
static pthread_mutex_t profile_lock = PTHREAD_MUTEX_INITIALIZER;

static int compare_symbols(const void *a, const void *b)
{
	const struct symbol *x = a, *y = b;

	return x->address < y->address ? -1 : x->address > y->address;
}

/* the function symbols of the guest's ELF image (32 or 64-bit) */
void host_profile_load_symbols(const void *file, size_t size)
{
	const unsigned char *bytes = file;
	int is64 = bytes[4] == 2;
	uint64_t section_offset;
	unsigned section_size, section_count, index;
	size_t names_used = 0;

	if (!getenv("HALO_PROFILE") || size < 64)
		return;
	section_offset = is64 ? *(const uint64_t *)(bytes + 0x28) : *(const uint32_t *)(bytes + 0x20);
	section_size = is64 ? *(const uint16_t *)(bytes + 0x3a) : *(const uint16_t *)(bytes + 0x2e);
	section_count = is64 ? *(const uint16_t *)(bytes + 0x3c) : *(const uint16_t *)(bytes + 0x30);
	if (section_offset + (uint64_t)section_size * section_count > size)
		return;
	symbols = calloc(MAXIMUM_SYMBOLS, sizeof(*symbols));
	symbol_names = malloc(4 * 1024 * 1024);
	if (!symbols || !symbol_names)
		return;
	for (index = 0; index < section_count; index++)
	{
		const unsigned char *section = bytes + section_offset + (uint64_t)index * section_size;
		uint32_t type = *(const uint32_t *)(section + 4);
		uint64_t offset, length, entry_size, link;
		const unsigned char *strings_section;
		uint64_t strings_offset;
		uint64_t entry;

		if (type != 2) /* SHT_SYMTAB */
			continue;
		offset = is64 ? *(const uint64_t *)(section + 0x18) : *(const uint32_t *)(section + 0x10);
		length = is64 ? *(const uint64_t *)(section + 0x20) : *(const uint32_t *)(section + 0x14);
		link = is64 ? *(const uint32_t *)(section + 0x28) : *(const uint32_t *)(section + 0x18);
		entry_size = is64 ? *(const uint64_t *)(section + 0x38) : *(const uint32_t *)(section + 0x24);
		if (!entry_size || link >= section_count || offset + length > size)
			continue;
		strings_section = bytes + section_offset + link * section_size;
		strings_offset = is64 ? *(const uint64_t *)(strings_section + 0x18) : *(const uint32_t *)(strings_section + 0x10);
		for (entry = 0; entry < length / entry_size && symbol_count < MAXIMUM_SYMBOLS; entry++)
		{
			const unsigned char *symbol = bytes + offset + entry * entry_size;
			uint32_t name = *(const uint32_t *)symbol;
			unsigned char info = is64 ? symbol[4] : symbol[12];
			uint64_t value = is64 ? *(const uint64_t *)(symbol + 8) : *(const uint32_t *)(symbol + 4);
			uint64_t symbol_size = is64 ? *(const uint64_t *)(symbol + 16) : *(const uint32_t *)(symbol + 8);
			const char *text = (const char *)bytes + strings_offset + name;
			size_t text_length;

			/* functions (STT_FUNC), and untyped symbols in the code (the
			assembler's) */
			if ((info & 15) != 2 && (info & 15) != 0)
				continue;
			if (!value || !*text || text[0] == '.' || text[0] == '$' || strings_offset + name >= size)
				continue;
			text_length = strlen(text) + 1;
			if (names_used + text_length > 4 * 1024 * 1024)
				break;
			memcpy(symbol_names + names_used, text, text_length);
			symbols[symbol_count].address = (uint32_t)value;
			symbols[symbol_count].size = (uint32_t)symbol_size;
			symbols[symbol_count].name = symbol_names + names_used;
			names_used += text_length;
			symbol_count++;
		}
	}
	qsort(symbols, (size_t)symbol_count, sizeof(*symbols), compare_symbols);
	host_logf(HOST_LOG_INFO, "profile: %d guest symbols", symbol_count);
}

static const char *guest_symbol(uint32_t address)
{
	int low = 0, high = symbol_count - 1, found = -1;

	while (low <= high)
	{
		int middle = (low + high) / 2;

		if (symbols[middle].address <= address)
		{
			found = middle;
			low = middle + 1;
		}
		else
		{
			high = middle - 1;
		}
	}
	return found >= 0 ? symbols[found].name : "(guest)";
}

static void count(const char *name, int guest, int thread)
{
	int index;

	for (index = 0; index < bucket_count; index++)
	{
		if (buckets[index].name == name || !strcmp(buckets[index].name, name))
			break;
	}
	if (index == bucket_count)
	{
		if (bucket_count == MAXIMUM_BUCKETS)
			return;
		buckets[index].name = name;
		buckets[index].guest = guest;
		bucket_count++;
	}
	buckets[index].samples++;
	buckets[index].thread_samples[thread]++;
	total_samples++;
	thread_totals[thread]++;
}

static int thread_slot(thread_act_t thread)
{
	int index;

	for (index = 0; index < known_thread_count; index++)
	{
		if (known_threads[index] == thread)
			return index;
	}
	if (known_thread_count == MAXIMUM_THREADS)
		return MAXIMUM_THREADS - 1;
	known_threads[known_thread_count] = thread;
	return known_thread_count++;
}

static void sample(thread_act_t self)
{
	thread_act_array_t threads;
	mach_msg_type_number_t thread_count, index;

	if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS)
		return;
	for (index = 0; index < thread_count; index++)
	{
		uint64_t pc = 0;

		if (threads[index] != self && thread_suspend(threads[index]) == KERN_SUCCESS)
		{
#ifdef __aarch64__
			arm_thread_state64_t state;
			mach_msg_type_number_t size = ARM_THREAD_STATE64_COUNT;

			if (thread_get_state(threads[index], ARM_THREAD_STATE64, (thread_state_t)&state, &size) == KERN_SUCCESS)
				pc = arm_thread_state64_get_pc(state);
#else
			x86_thread_state64_t state;
			mach_msg_type_number_t size = x86_THREAD_STATE64_COUNT;

			if (thread_get_state(threads[index], x86_THREAD_STATE64, (thread_state_t)&state, &size) == KERN_SUCCESS)
				pc = state.__rip;
#endif
			thread_resume(threads[index]);
		}
		if (pc)
		{
			int slot = thread_slot(threads[index]);

			if (pc >= host_guest_base + host_image.base && pc < host_guest_base + host_image.end)
			{
				count(guest_symbol((uint32_t)(pc - host_guest_base)), 1, slot);
			}
			else
			{
				Dl_info information;

				if (dladdr((void *)(uintptr_t)pc, &information) && information.dli_sname)
					count(information.dli_sname, 0, slot);
				else
					count("(host, unknown)", 0, slot);
			}
		}
		mach_port_deallocate(mach_task_self(), threads[index]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, thread_count * sizeof(thread_act_t));
}

static void *profile_thread(void *unused)
{
	thread_act_t self = mach_thread_self();

	(void)unused;
	for (;;)
	{
		pthread_mutex_lock(&profile_lock);
		if (profiling)
			sample(self);
		pthread_mutex_unlock(&profile_lock);
		usleep(1000);
	}
	return NULL;
}

void host_profile_start(void)
{
	pthread_t thread;

	if (!getenv("HALO_PROFILE"))
		return;
	profiling = 1;
	if (pthread_create(&thread, NULL, profile_thread, NULL) == 0)
		pthread_detach(thread);
	host_logf(HOST_LOG_INFO, "profile: sampling every thread 1000 times a second");
}

static int compare_buckets(const void *a, const void *b)
{
	const struct bucket *x = a, *y = b;

	return x->samples < y->samples ? 1 : x->samples > y->samples ? -1 : 0;
}

void host_profile_write(void)
{
	char path[1200];
	FILE *file;
	int index, thread;

	if (!profiling)
		return;
	pthread_mutex_lock(&profile_lock);
	profiling = 0;
	qsort(buckets, (size_t)bucket_count, sizeof(buckets[0]), compare_buckets);
	snprintf(path, sizeof(path), "%s/profile.txt", host_data_root);
	file = fopen(path, "w");
	if (file)
	{
		fprintf(file, "%lu samples (1 ms apart, every thread)\n\n", total_samples);
		fprintf(file, "all threads:\n");
		for (index = 0; index < bucket_count && index < 80; index++)
		{
			fprintf(file, "%6.2f%%  %8lu  %s %s\n", 100.0 * (double)buckets[index].samples / (double)total_samples,
				buckets[index].samples, buckets[index].guest ? "guest" : "host ", buckets[index].name);
		}
		for (thread = 0; thread < known_thread_count; thread++)
		{
			int shown = 0;

			if (thread_totals[thread] < total_samples / 50)
				continue;
			fprintf(file, "\nthread %d (%lu samples):\n", thread, thread_totals[thread]);
			for (index = 0; index < bucket_count && shown < 30; index++)
			{
				if (!buckets[index].thread_samples[thread])
					continue;
				fprintf(file, "%6.2f%%  %8lu  %s %s\n",
					100.0 * (double)buckets[index].thread_samples[thread] / (double)thread_totals[thread],
					buckets[index].thread_samples[thread], buckets[index].guest ? "guest" : "host ", buckets[index].name);
				shown++;
			}
		}
		fclose(file);
		host_logf(HOST_LOG_INFO, "profile: %lu samples written to %s", total_samples, path);
	}
	pthread_mutex_unlock(&profile_lock);
}
