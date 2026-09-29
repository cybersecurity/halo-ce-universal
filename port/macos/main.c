#include "guest_address.h"
#include "guest_allocator.h"
#include "guest_call.h"
#include "guest_callback.h"
#include "guest_heap.h"
#include "guest_errno.h"
#include "guest_image.h"
#include "guest_import_registry.h"
#include "guest_thread.h"
#include "host_import_bindings.h"
#include "host_services.h"
#include "host_game_evidence.h"
#include "gl_token_registry.h"
#include "recomp_state.h"

#include "recomp_funcs.h"
#include "recomp_image_symbols.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define GUEST_PAGE_SIZE 4096u
#define MAIN_GUEST_STACK_SIZE (8u * 1024u * 1024u)
#define GUEST_RETURN_SENTINEL UINT32_C(0xffffffff)

struct guest_boot
{
	uint32_t argc;
	uint32_t argv;
	uint32_t environment;
	uint32_t page_size;
};

static void *read_file(const char *path, size_t *size_out)
{
	FILE *file = fopen(path, "rb");
	long size;
	void *bytes;
	if (!file) return NULL;
	if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 ||
		fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		return NULL;
	}
	bytes = malloc((size_t)size);
	if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size)
	{
		free(bytes);
		fclose(file);
		return NULL;
	}
	fclose(file);
	*size_out = (size_t)size;
	return bytes;
}

static recomp_func_t resolve_guest_function(uint32_t guest_va)
{
	recomp_func_t function = mac_guest_import_resolve(guest_va);
	if (!function) function = recomp_lookup_manual(guest_va);
	if (!function) function = recomp_lookup(guest_va);
	return function;
}

static void initialize_worker(void *context, uint32_t guest_thread_va,
	uint32_t guest_esp)
{
	(void)context;
	(void)guest_thread_va;
	mac_recomp_state_initialize_thread(guest_esp, 0);
}

static void dispatch_worker(void *context, uint32_t guest_function_va)
{
	recomp_func_t function;
	(void)context;
	function = resolve_guest_function(guest_function_va);
	if (!function)
	{
		mac_host_logf(3, "no translated worker start at 0x%08x", guest_function_va);
		mac_host_abort("unresolved guest worker entry");
	}
	function();
}

static int verify_indirect_host_import(void)
{
	uint32_t token = mac_guest_import_token("host_get_tp");
	uint32_t gl_token = mac_macos_gl_token_for_name("glGetString");
	uint32_t stack_va, sentinel = GUEST_RETURN_SENTINEL;
	recomp_func_t callback;
	if (!token || !RECOMP_ICALL_IS_CODE(token) || !gl_token ||
		!RECOMP_ICALL_IS_CODE(gl_token) || !recomp_lookup_kernel(gl_token) ||
		mac_guest_allocate(16, 16, &stack_va) != 0 ||
		mac_guest_write(stack_va, &sentinel, sizeof(sentinel)) != 0)
		return -1;
	callback = recomp_lookup_manual(token);
	if (!callback)
		return -1;
	g_esp = stack_va;
	g_eax = UINT32_MAX;
	callback();
	return g_eax == 0 && g_esp == stack_va + 4 ? 0 : -1;
}

static int make_directory(const char *path)
{
	if (mkdir(path, 0700) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static int write_guest_string(const char *value, uint32_t *guest_va_out)
{
	size_t length = strlen(value) + 1;
	if (mac_guest_allocate(length, 1, guest_va_out) != 0)
		return -1;
	return mac_guest_write(*guest_va_out, value, length);
}

static int make_guest_boot(const char *data_root, const char *save_root,
	uint32_t *boot_va_out)
{
	struct guest_boot boot = { 1, 0, 0, GUEST_PAGE_SIZE };
	uint32_t argv_va, env_va, argv[2], env[128];
	uint32_t boot_va;
	const char *home = save_root;
	const char *env_names[] = { "HALO_DATA_ROOT", "HALO_SAVE_ROOT", "HOME", "XDG_DATA_HOME" };
	const char *env_values[] = { data_root, save_root, home, save_root };
	const char *trace_rt_alloc = getenv("HALO_TRACE_RT_ALLOC");
	const char *exit_after = getenv("HALO_EXIT_AFTER");
	size_t env_count = 0;

	if (write_guest_string("halo", &argv[0]) != 0 ||
		mac_guest_allocate(sizeof(argv), 4, &argv_va) != 0 ||
		mac_guest_allocate(sizeof(env), 4, &env_va) != 0 ||
		mac_guest_allocate(sizeof(boot), 4, &boot_va) != 0)
		return -1;
	argv[1] = 0;
	for (size_t i = 0; i < 4; ++i)
	{
		char entry[4096];
		int count = snprintf(entry, sizeof(entry), "%s=%s", env_names[i], env_values[i]);
		if (count < 0 || (size_t)count >= sizeof(entry) ||
			write_guest_string(entry, &env[i]) != 0)
			return -1;
	}
	env_count = 4;
	if (trace_rt_alloc && *trace_rt_alloc)
	{
		char entry[128];
		int count = snprintf(entry, sizeof(entry), "HALO_TRACE_RT_ALLOC=%s", trace_rt_alloc);
		if (count < 0 || (size_t)count >= sizeof(entry) ||
			write_guest_string(entry, &env[env_count++]) != 0)
			return -1;
	}
	if (exit_after && *exit_after)
	{
		char entry[128];
		int count = snprintf(entry, sizeof(entry), "HALO_EXIT_AFTER=%s", exit_after);
		if (count < 0 || (size_t)count >= sizeof(entry) ||
			write_guest_string(entry, &env[env_count++]) != 0)
			return -1;
	}
	/* Guest configuration owns the HALO_* options, including user-rebound
	 * keys. Forward them with a hard capacity bound rather than maintaining
	 * a second incomplete list of configuration names in the native host. */
	extern char **environ;
	for (char **entry = environ; entry && *entry; ++entry)
	{
		if (strncmp(*entry, "HALO_", 5) != 0)
			continue;
		const char *owned_names[] = { "HALO_DATA_ROOT", "HALO_SAVE_ROOT", "HALO_TRACE_RT_ALLOC", "HALO_EXIT_AFTER" };
		int owned = 0;
		for (size_t index = 0; index < sizeof(owned_names) / sizeof(owned_names[0]); ++index)
		{
			size_t length = strlen(owned_names[index]);
			if (strncmp(*entry, owned_names[index], length) == 0 && (*entry)[length] == '=') owned = 1;
		}
		if (owned) continue;
		if (env_count + 1 >= sizeof(env) / sizeof(env[0]) || strlen(*entry) >= 4096 ||
			write_guest_string(*entry, &env[env_count++]) != 0)
			return -1;
	}
	env[env_count] = 0;
	boot.argv = argv_va;
	boot.environment = env_va;
	if (mac_guest_write(argv_va, argv, sizeof(argv)) != 0 ||
		mac_guest_write(env_va, env, (env_count + 1) * sizeof(env[0])) != 0 ||
		mac_guest_write(boot_va, &boot, sizeof(boot)) != 0)
		return -1;
	*boot_va_out = boot_va;
	return 0;
}

static int launch_guest(const char *elf_path)
{
	struct mac_guest_image image;
	struct mac_guest_thread_runtime thread_runtime = {
		NULL, initialize_worker, dispatch_worker
	};
	char error[256];
	void *elf_bytes;
	size_t elf_size;
	uint32_t guest_boot_va, guest_stack_va, thread_start_va, thread_attach_va, errno_location_va;
	uint32_t guest_esp;
	uint32_t stack_frame[2] = { GUEST_RETURN_SENTINEL, 0 };
	recomp_func_t entry;
	const char *data_root = getenv("HALO_DATA_ROOT");
	const char *save_root = getenv("HALO_SAVE_ROOT");
	char default_data_root[] = ".";
	char default_save_root[4096];

	if (!data_root || !*data_root) data_root = default_data_root;
	if (!save_root || !*save_root)
	{
		int n = snprintf(default_save_root, sizeof(default_save_root),
			"%s/build/macos-aot/run-data/save", getenv("HALO_PROJECT_ROOT") ?
			getenv("HALO_PROJECT_ROOT") : ".");
		if (n < 0 || (size_t)n >= sizeof(default_save_root))
			return 2;
		save_root = default_save_root;
	}
	if (make_directory("build/macos-aot/run-data") != 0 && errno != EEXIST)
	{
		fprintf(stderr, "cannot create isolated run-data directory: %s\n", strerror(errno));
		return 2;
	}
	if (make_directory(save_root) != 0 && errno != EEXIST)
	{
		fprintf(stderr, "cannot create isolated save root %s: %s\n", save_root, strerror(errno));
		return 2;
	}
	if (setenv("HALO_DATA_ROOT", data_root, 1) != 0 ||
		setenv("HALO_SAVE_ROOT", save_root, 1) != 0 || setenv("HOME", save_root, 1) != 0)
	{
		fprintf(stderr, "cannot configure guest data/save roots: %s\n", strerror(errno));
		return 2;
	}

	elf_bytes = read_file(elf_path, &elf_size);
	if (!elf_bytes)
	{
		fprintf(stderr, "cannot read AOT guest ELF: %s (%s)\n", elf_path, strerror(errno));
		return 2;
	}
	if (mac_guest_image_load(elf_bytes, elf_size, &image, error, sizeof(error)) != 0)
	{
		fprintf(stderr, "guest ELF load failed: %s\n", error);
		free(elf_bytes);
		return 2;
	}
	if (image.entry_va != HALO_RECOMP_ENTRY_VA ||
		mac_guest_image_find_symbol(elf_bytes, elf_size, "__guest_thread_start",
			&thread_start_va) != 0 || thread_start_va != HALO_RECOMP_THREAD_START_VA ||
		mac_guest_image_find_symbol(elf_bytes, elf_size, "__guest_thread_attach",
			&thread_attach_va) != 0 || thread_attach_va != HALO_RECOMP_THREAD_ATTACH_VA ||
		mac_guest_image_find_symbol(elf_bytes, elf_size, "__errno_location",
			&errno_location_va) != 0 || errno_location_va != HALO_RECOMP_ERRNO_LOCATION_VA)
	{
		fprintf(stderr, "AOT ELF does not match generated lift symbols (%s)\n",
			HALO_RECOMP_IMAGE_SHA256);
		free(elf_bytes);
		return 2;
	}
	if (getenv("HALO_MAP_EVIDENCE"))
	{
		uint32_t globals_va = 0, scenario_va = 0;
		if (mac_guest_image_find_object(elf_bytes, elf_size, "game_globals", 4, &globals_va) != 0 ||
			mac_guest_image_find_object(elf_bytes, elf_size, "global_scenario_index", 4, &scenario_va) != 0)
		{
			fprintf(stderr, "map evidence symbols unavailable in loaded ELF\n");
			free(elf_bytes);
			return 2;
		}
		mac_game_evidence_configure(globals_va, scenario_va);
	}
	if (getenv("HALO_UI_POINTER_TRACE"))
	{
		uint32_t count=0,targets=0,x=0,y=0;
		if(mac_guest_image_find_object(elf_bytes,elf_size,"ui_mouse_target_count",4,&count)||
		   mac_guest_image_find_object(elf_bytes,elf_size,"ui_mouse_targets",1536,&targets)||
		   mac_guest_image_find_object(elf_bytes,elf_size,"ui_mouse_click_x",2,&x)||
		   mac_guest_image_find_object(elf_bytes,elf_size,"ui_mouse_click_y",2,&y))
		{ fprintf(stderr,"UI pointer evidence symbols unavailable\n");free(elf_bytes);return 2; }
		mac_ui_evidence_configure(count,targets,x,y);
	}
	free(elf_bytes);

	if (mac_recomp_state_set_image(image.memory_offset, image.code_lo_va,
		image.code_hi_va) != 0 || mac_guest_heap_install(image.image_end_va) != 0)
	{
		fprintf(stderr, "failed to install guest image memory/runtime state\n");
		return 2;
	}
	if (mac_macos_host_imports_register() != 0)
	{
		fprintf(stderr, "failed to register host import manifest\n");
		return 2;
	}
	fprintf(stderr, "AOT imports: %zu bound, %zu fail-fast\n",
		mac_macos_host_imports_bound_count(), mac_macos_host_imports_missing_count());
	(void)recomp_dispatch_init();
	if (mac_guest_errno_install(recomp_lookup(errno_location_va)) != 0)
	{
		fprintf(stderr, "failed to install hash-matched guest errno accessor\n");
		return 2;
	}
	if (verify_indirect_host_import() != 0)
	{
		fprintf(stderr, "translated function-pointer host import check failed\n");
		return 2;
	}
	if (mac_guest_thread_runtime_install(&thread_runtime, thread_start_va) != 0)
	{
		fprintf(stderr, "failed to install guest thread runtime\n");
		return 2;
	}
	if (mac_guest_callback_runtime_install(&thread_runtime, thread_attach_va) != 0)
	{
		fprintf(stderr, "failed to install guest callback runtime\n");
		return 2;
	}
	if (make_guest_boot(data_root, save_root, &guest_boot_va) != 0 ||
		mac_guest_allocate(MAIN_GUEST_STACK_SIZE, 16 * 1024, &guest_stack_va) != 0)
	{
		fprintf(stderr, "cannot allocate guest boot block or main stack\n");
		return 2;
	}
	guest_esp = (guest_stack_va + MAIN_GUEST_STACK_SIZE) & ~UINT32_C(15);
	guest_esp -= sizeof(stack_frame);
	if (getenv("HALO_TRACE_GUEST_STACK"))
		fprintf(stderr, "[GUEST-MAIN-STACK] thread=%llu base=0x%08x size=%u top=0x%08x entry_esp=0x%08x host=%p\n",
			(unsigned long long)(uintptr_t)pthread_self(), guest_stack_va,
			MAIN_GUEST_STACK_SIZE, guest_stack_va + MAIN_GUEST_STACK_SIZE,
			guest_esp, mac_guest_address_resolve(guest_stack_va, MAIN_GUEST_STACK_SIZE));
	stack_frame[1] = guest_boot_va;
	if (mac_guest_write(guest_esp, stack_frame, sizeof(stack_frame)) != 0)
	{
		fprintf(stderr, "cannot initialize guest entry stack\n");
		return 2;
	}
	mac_recomp_state_initialize_thread(guest_esp, 0);
	entry = resolve_guest_function(image.entry_va);
	if (!entry)
	{
		fprintf(stderr, "guest entry VA 0x%08x is not translated\n", image.entry_va);
		return 2;
	}
	fprintf(stderr, "AOT image %s (%s), image end 0x%08x; data %s; saves %s\n",
		elf_path, HALO_RECOMP_IMAGE_SHA256, image.image_end_va, data_root, save_root);
	fprintf(stderr, "starting translated source guest at 0x%08x\n", image.entry_va);
	entry();
	fprintf(stderr, "guest entry returned unexpectedly (EAX=0x%08x)\n", g_eax);
	return 1;
}

int main(int argc, char **argv)
{
	const char *elf_path = argc > 1 ? argv[1] : getenv("HALO_GUEST_ELF");
	if (!elf_path || !*elf_path)
		elf_path = "build/macos-aot/halo_guest.elf";
	return launch_guest(elf_path);
}
