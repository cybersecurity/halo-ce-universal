#include "host_imports.h"

#include "guest_address.h"
#include "guest_call.h"
#include "guest_thread.h"
#include "host_sdl.h"
#include "host_memory_fingerprint.h"
#include "host_bink.h"
#include "host_sdl_audio.h"
#include "host_services.h"
#include "host_syscall.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define GUEST_STRING_LIMIT 4096u

static struct mac_guest_import_registers registers;
static pthread_mutex_t registers_mutex = PTHREAD_MUTEX_INITIALIZER;
static int registers_installed;
static _Atomic int audio_disabled_reported;

int mac_guest_host_import_registers_install(const struct mac_guest_import_registers *value)
{
	int result = -1;
	if (!value || !value->get_esp || !value->set_esp || !value->set_eax || !value->set_edx)
		return -1;
	pthread_mutex_lock(&registers_mutex);
	if (!registers_installed)
	{
		registers = *value;
		registers_installed = 1;
		result = 0;
	}
	pthread_mutex_unlock(&registers_mutex);
	return result;
}

static struct mac_guest_import_registers active_registers(void)
{
	struct mac_guest_import_registers value;
	pthread_mutex_lock(&registers_mutex);
	value = registers;
	pthread_mutex_unlock(&registers_mutex);
	if (!registers_installed)
		mac_host_abort("host import entered before translator register accessors were installed");
	return value;
}

uint32_t mac_guest_host_import_esp(void)
{
	struct mac_guest_import_registers state = active_registers();
	return state.get_esp(state.context);
}

uint32_t mac_guest_host_import_arg32(uint32_t index)
{
	struct mac_guest_import_registers state = active_registers();
	uint32_t result;
	if (mac_guest_stack_arg32(state.get_esp(state.context), index, &result) != 0)
		mac_host_abort("invalid guest stack argument in host import");
	return result;
}

uint64_t mac_guest_host_import_arg64_words(uint32_t low_index)
{
	uint32_t low = mac_guest_host_import_arg32(low_index);
	uint32_t high = mac_guest_host_import_arg32(low_index + 1);
	return (uint64_t)low | ((uint64_t)high << 32);
}

void mac_guest_host_import_return32(uint32_t value)
{
	struct mac_guest_import_registers state = active_registers();
	uint32_t entry_esp = state.get_esp(state.context), after;
	if (mac_guest_stack_after_return(entry_esp, 0, &after) != 0)
		mac_host_abort("guest stack overflow returning from host import");
	state.set_eax(state.context, value);
	state.set_esp(state.context, after);
}

void mac_guest_host_import_return64(int64_t value)
{
	struct mac_guest_import_registers state = active_registers();
	uint64_t bits = (uint64_t)value;
	uint32_t entry_esp = state.get_esp(state.context), after;
	if (mac_guest_stack_after_return(entry_esp, 0, &after) != 0)
		mac_host_abort("guest stack overflow returning from host import");
	state.set_eax(state.context, (uint32_t)bits);
	state.set_edx(state.context, (uint32_t)(bits >> 32));
	state.set_esp(state.context, after);
}

void mac_guest_host_import_return_void(void)
{
	struct mac_guest_import_registers state = active_registers();
	uint32_t after;
	if (mac_guest_stack_after_return(state.get_esp(state.context), 0, &after) != 0)
		mac_host_abort("guest stack overflow returning from host import");
	state.set_esp(state.context, after);
}

static int copy_guest_string(uint32_t address, char output[GUEST_STRING_LIMIT])
{
	for (size_t index = 0; index < GUEST_STRING_LIMIT; ++index)
	{
		uint64_t guest_byte_address = (uint64_t)address + index;
		const char *byte;
		if (guest_byte_address > UINT32_MAX || !(byte = mac_guest_address_resolve(
			(uint32_t)guest_byte_address, 1)))
			return -1;
		output[index] = *byte;
		if (!*byte)
			return 0;
	}
	return -1;
}

static void bridge_host_log(void)
{
	char message[GUEST_STRING_LIMIT];
	uint32_t priority = mac_guest_host_import_arg32(0), text_va = mac_guest_host_import_arg32(1);
	if (copy_guest_string(text_va, message) != 0)
		mac_host_abort("host_log received an invalid guest string");
	/* Android priorities: verbose/debug/info/warn/error/fatal. */
	if (priority <= 3) priority = 0;
	else if (priority == 4) priority = 1;
	else if (priority == 5) priority = 2;
	else priority = 3;
	mac_host_log((int)priority, message);
	mac_guest_host_import_return_void();
}

static void bridge_host_abort(void)
{
	char message[GUEST_STRING_LIMIT];
	if (copy_guest_string(mac_guest_host_import_arg32(0), message) != 0)
		mac_host_abort("host_abort received an invalid guest string");
	mac_host_abort(message);
}

static void bridge_host_exit(void)
{
	mac_host_exit((int32_t)mac_guest_host_import_arg32(0));
}

static void bridge_host_errno(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_errno());
}

static void bridge_host_get_tp(void)
{
	mac_guest_host_import_return32(mac_host_get_guest_tp());
}

static void bridge_host_set_tp(void)
{
	mac_host_set_guest_tp(mac_guest_host_import_arg32(0));
	mac_guest_host_import_return_void();
}

static void bridge_host_thread_create(void)
{
	int result = mac_host_thread_create(mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1));
	mac_guest_host_import_return32((uint32_t)result);
}

static void bridge_host_syscall(void)
{
	int64_t result = mac_host_arm64_32_syscall_from_stack(mac_guest_host_import_esp());
	mac_guest_host_import_return64(result);
}

static void bridge_sdl_init(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_init(mac_guest_host_import_arg32(0)));
}

static void bridge_sdl_set_hint(void)
{
	char name[GUEST_STRING_LIMIT], value[GUEST_STRING_LIMIT];
	if (copy_guest_string(mac_guest_host_import_arg32(0), name) != 0 ||
		copy_guest_string(mac_guest_host_import_arg32(1), value) != 0)
		mac_host_abort("host_sdl_set_hint received an invalid guest string");
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_set_hint(mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1)));
}

static void bridge_sdl_get_error(void)
{
	(void)mac_host_sdl_get_error(mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1));
	mac_guest_host_import_return_void();
}

static void bridge_sdl_ticks(void)
{
	mac_guest_host_import_return64(mac_host_sdl_ticks());
}

static void bridge_sdl_thread_id(void)
{
	mac_guest_host_import_return64(mac_host_sdl_thread_id());
}

static void bridge_sdl_create_window(void)
{
	uint32_t title_va = mac_guest_host_import_arg32(0);
	uint64_t flags = mac_guest_host_import_arg64_words(3);
	mac_guest_host_import_return32(mac_host_sdl_create_window(title_va, (int32_t)mac_guest_host_import_arg32(1),
		(int32_t)mac_guest_host_import_arg32(2), (int64_t)flags));
}

static void bridge_sdl_window_size(void)
{
	mac_host_sdl_window_size_in_pixels(mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1), mac_guest_host_import_arg32(2));
	mac_guest_host_import_return_void();
}

static void bridge_platform_screen_mode(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_platform_screen_mode(
		mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1)));
}

static void bridge_sdl_window_logical_size(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_window_size(
		mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1), mac_guest_host_import_arg32(2)));
}

static void bridge_sdl_window_flags(void)
{
	mac_guest_host_import_return64(mac_host_sdl_window_flags(mac_guest_host_import_arg32(0)));
}

static void bridge_sdl_window_set_size(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_set_window_size(
		mac_guest_host_import_arg32(0), (int32_t)mac_guest_host_import_arg32(1), (int32_t)mac_guest_host_import_arg32(2)));
}

static void bridge_sdl_window_fullscreen(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_set_window_fullscreen(
		mac_guest_host_import_arg32(0), (int32_t)mac_guest_host_import_arg32(1)));
}

static void bridge_sdl_warp_mouse(void)
{
	uint32_t x_bits = mac_guest_host_import_arg32(1), y_bits = mac_guest_host_import_arg32(2);
	float x, y;
	memcpy(&x, &x_bits, sizeof(x));
	memcpy(&y, &y_bits, sizeof(y));
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_warp_mouse(mac_guest_host_import_arg32(0), x, y));
}

static void bridge_sdl_relative_mouse(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_set_relative_mouse(mac_guest_host_import_arg32(0),
		(int32_t)mac_guest_host_import_arg32(1)));
}

static void bridge_sdl_gl_attribute(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_gl_set_attribute((int32_t)mac_guest_host_import_arg32(0),
		(int32_t)mac_guest_host_import_arg32(1)));
}

static void bridge_sdl_gl_create_context(void)
{
	mac_guest_host_import_return32(mac_host_sdl_gl_create_context(mac_guest_host_import_arg32(0)));
}

static void bridge_sdl_gl_make_current(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_gl_make_current(mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1)));
}

static void bridge_sdl_gl_swap_interval(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_gl_set_swap_interval((int32_t)mac_guest_host_import_arg32(0)));
}

static void bridge_sdl_gl_swap_window(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_gl_swap_window(mac_guest_host_import_arg32(0)));
}

static void bridge_sdl_poll_event(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_poll_event(mac_guest_host_import_arg32(0)));
}

static void bridge_sdl_set_clipboard(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_set_clipboard_text(mac_guest_host_import_arg32(0)));
}

static void bridge_sdl_get_clipboard(void)
{
	(void)mac_host_sdl_get_clipboard_text(mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1));
	mac_guest_host_import_return_void();
}

static void bridge_sdl_toast(void)
{
	char message[GUEST_STRING_LIMIT];
	if (copy_guest_string(mac_guest_host_import_arg32(0), message) != 0)
		mac_host_abort("host_sdl_show_toast received an invalid guest string");
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_show_toast(mac_guest_host_import_arg32(0),
		(int32_t)mac_guest_host_import_arg32(1), (int32_t)mac_guest_host_import_arg32(2),
		(int32_t)mac_guest_host_import_arg32(3), (int32_t)mac_guest_host_import_arg32(4)));
}

static void bridge_sdl_message_box(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_show_simple_message_box(mac_guest_host_import_arg32(0),
		mac_guest_host_import_arg32(1), mac_guest_host_import_arg32(2)));
}

static void bridge_sdl_get_gamepads(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_get_gamepads(mac_guest_host_import_arg32(0),
		(int32_t)mac_guest_host_import_arg32(1)));
}

static void bridge_sdl_open_gamepad(void)
{
	mac_guest_host_import_return32(mac_host_sdl_open_gamepad(mac_guest_host_import_arg32(0)));
}

static void bridge_sdl_gamepad_from_id(void)
{
	mac_guest_host_import_return32(mac_host_sdl_gamepad_from_id(mac_guest_host_import_arg32(0)));
}

static void bridge_sdl_gamepad_axis(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_gamepad_axis(mac_guest_host_import_arg32(0), (int32_t)mac_guest_host_import_arg32(1)));
}

static void bridge_sdl_gamepad_button(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_gamepad_button(mac_guest_host_import_arg32(0), (int32_t)mac_guest_host_import_arg32(1)));
}

static void bridge_sdl_gamepad_type(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_gamepad_type(mac_guest_host_import_arg32(0)));
}

static void bridge_sdl_rumble(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_rumble_gamepad(mac_guest_host_import_arg32(0),
		mac_guest_host_import_arg32(1), mac_guest_host_import_arg32(2), mac_guest_host_import_arg32(3)));
}

static void report_audio_disabled(void)
{
	if (!atomic_exchange_explicit(&audio_disabled_reported, 1, memory_order_relaxed))
		mac_host_log(1, "SDL guest audio is opt-in; set HALO_AUDIO_ENABLE=1 to enable translated callbacks");
}

static void bridge_sdl_audio_open(void)
{
	uint32_t device = mac_guest_host_import_arg32(0);
	uint32_t spec_va = mac_guest_host_import_arg32(1);
	uint32_t callback_va = mac_guest_host_import_arg32(2);
	uint32_t userdata_va = mac_guest_host_import_arg32(3);
	if (!mac_host_sdl_audio_enabled()) report_audio_disabled();
	mac_guest_host_import_return32(mac_host_sdl_open_audio_stream(device, spec_va,
		callback_va, userdata_va));
}

static void bridge_sdl_audio_put(void)
{
	uint32_t stream = mac_guest_host_import_arg32(0);
	uint32_t data_va = mac_guest_host_import_arg32(1);
	int length = (int32_t)mac_guest_host_import_arg32(2);
	if (!mac_host_sdl_audio_enabled()) report_audio_disabled();
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_put_audio_stream_data(stream,
		data_va, length));
}

static void bridge_sdl_audio_resume(void)
{
	uint32_t stream = mac_guest_host_import_arg32(0);
	if (!mac_host_sdl_audio_enabled()) report_audio_disabled();
	mac_guest_host_import_return32((uint32_t)mac_host_sdl_resume_audio_stream_device(stream));
}

#define IMPORT(name, fn) { name, fn }
static void bridge_memory_fingerprint_page(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_memory_fingerprint_page(
		mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1)));
}
static void bridge_memory_fingerprint_range(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_memory_fingerprint_range(
		mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1),
		mac_guest_host_import_arg32(2), mac_guest_host_import_arg32(3), mac_guest_host_import_arg32(4)));
}
static void bridge_bink_open(void)
{
	mac_guest_host_import_return32(mac_host_bink_open(mac_guest_host_import_arg32(0),
		mac_guest_host_import_arg32(1), (int32_t)mac_guest_host_import_arg32(2)));
}
static void bridge_bink_decode(void) { mac_guest_host_import_return32((uint32_t)mac_host_bink_decode(mac_guest_host_import_arg32(0))); }
static void bridge_bink_next(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_bink_next(mac_guest_host_import_arg32(0), mac_guest_host_import_arg32(1)));
}
static void bridge_bink_wait(void) { mac_guest_host_import_return32((uint32_t)mac_host_bink_wait(mac_guest_host_import_arg32(0))); }
static void bridge_bink_close(void) { mac_host_bink_close(mac_guest_host_import_arg32(0)); mac_guest_host_import_return_void(); }
static void bridge_bink_copy(void)
{
	mac_guest_host_import_return32((uint32_t)mac_host_bink_copy(mac_guest_host_import_arg32(0),
		mac_guest_host_import_arg32(1), (int32_t)mac_guest_host_import_arg32(2),
		mac_guest_host_import_arg32(3), mac_guest_host_import_arg32(4),
		mac_guest_host_import_arg32(5), mac_guest_host_import_arg32(6)));
}

static const struct
{
	const char *name;
	mac_guest_import_bridge bridge;
} host_imports[] =
{
	IMPORT("host_abort", bridge_host_abort),
	IMPORT("host_bink_open", bridge_bink_open),
	IMPORT("host_bink_decode", bridge_bink_decode),
	IMPORT("host_bink_next", bridge_bink_next),
	IMPORT("host_bink_wait", bridge_bink_wait),
	IMPORT("host_bink_close", bridge_bink_close),
	IMPORT("host_bink_copy", bridge_bink_copy),
	IMPORT("host_errno", bridge_host_errno),
	IMPORT("host_exit", bridge_host_exit),
	IMPORT("host_get_tp", bridge_host_get_tp),
	IMPORT("host_log", bridge_host_log),
	IMPORT("host_memory_fingerprint_page", bridge_memory_fingerprint_page),
	IMPORT("host_memory_fingerprint_range", bridge_memory_fingerprint_range),
	IMPORT("host_sdl_create_window", bridge_sdl_create_window),
	IMPORT("host_sdl_gamepad_axis", bridge_sdl_gamepad_axis),
	IMPORT("host_sdl_gamepad_button", bridge_sdl_gamepad_button),
	IMPORT("host_sdl_gamepad_from_id", bridge_sdl_gamepad_from_id),
	IMPORT("host_sdl_gamepad_type", bridge_sdl_gamepad_type),
	IMPORT("host_sdl_get_clipboard_text", bridge_sdl_get_clipboard),
	IMPORT("host_sdl_get_error", bridge_sdl_get_error),
	IMPORT("host_sdl_get_gamepads", bridge_sdl_get_gamepads),
	IMPORT("host_sdl_gl_create_context", bridge_sdl_gl_create_context),
	IMPORT("host_sdl_gl_make_current", bridge_sdl_gl_make_current),
	IMPORT("host_sdl_gl_set_attribute", bridge_sdl_gl_attribute),
	IMPORT("host_sdl_gl_set_swap_interval", bridge_sdl_gl_swap_interval),
	IMPORT("host_sdl_gl_swap_window", bridge_sdl_gl_swap_window),
	IMPORT("host_sdl_init", bridge_sdl_init),
	IMPORT("host_sdl_open_audio_stream", bridge_sdl_audio_open),
	IMPORT("host_sdl_open_gamepad", bridge_sdl_open_gamepad),
	IMPORT("host_sdl_poll_event", bridge_sdl_poll_event),
	IMPORT("host_sdl_put_audio_stream_data", bridge_sdl_audio_put),
	IMPORT("host_sdl_resume_audio_stream_device", bridge_sdl_audio_resume),
	IMPORT("host_sdl_rumble_gamepad", bridge_sdl_rumble),
	IMPORT("host_sdl_set_clipboard_text", bridge_sdl_set_clipboard),
	IMPORT("host_sdl_set_hint", bridge_sdl_set_hint),
	IMPORT("host_sdl_set_relative_mouse", bridge_sdl_relative_mouse),
	IMPORT("host_sdl_show_simple_message_box", bridge_sdl_message_box),
	IMPORT("host_sdl_show_toast", bridge_sdl_toast),
	IMPORT("host_sdl_thread_id", bridge_sdl_thread_id),
	IMPORT("host_sdl_ticks", bridge_sdl_ticks),
	IMPORT("host_sdl_window_size_in_pixels", bridge_sdl_window_size),
	IMPORT("host_sdl_window_size", bridge_sdl_window_logical_size),
	IMPORT("host_sdl_window_flags", bridge_sdl_window_flags),
	IMPORT("host_sdl_set_window_fullscreen", bridge_sdl_window_fullscreen),
	IMPORT("host_sdl_set_window_size", bridge_sdl_window_set_size),
	IMPORT("host_sdl_warp_mouse", bridge_sdl_warp_mouse),
	IMPORT("platform_screen_mode", bridge_platform_screen_mode),
	IMPORT("host_set_tp", bridge_host_set_tp),
	IMPORT("host_syscall", bridge_host_syscall),
	IMPORT("host_thread_create", bridge_host_thread_create)
};

mac_guest_import_bridge mac_macos_host_import_bridge(const char *name)
{
	if (!name)
		return NULL;
	for (size_t index = 0; index < sizeof(host_imports) / sizeof(host_imports[0]); ++index)
		if (strcmp(name, host_imports[index].name) == 0)
			return host_imports[index].bridge;
	return NULL;
}
