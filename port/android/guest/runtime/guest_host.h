/*
GUEST_HOST.H

The host services the guest imports (the guest's view; the host defines
them in port/android/host). Every name here must also be listed in
port/android/host_imports.list, which generates the import stubs.

Parameter types follow the rules in halo_android_abi.h: 32-bit values are
int or unsigned int, 64-bit values long long, and pointers are passed as
they are.
*/

#ifndef __GUEST_HOST_H
#define __GUEST_HOST_H

/* ---------- process */

/* performs a Linux system call on the guest's behalf, converting the
structures whose layout differs; returns the raw result (-errno on failure) */
long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f);

/* Android log priorities (android/log.h) */
void host_log(int priority, const char *text);
void host_abort(const char *reason) __attribute__((noreturn));
void host_exit(int code) __attribute__((noreturn));
/* the host's errno on this thread, after a call to a host function */
int host_errno(void);

/* ---------- threads

The guest's thread pointer (its struct pthread) is kept by the host for
each thread. */

unsigned int host_get_tp(void);
void host_set_tp(unsigned int thread);
/* starts a host thread with a stack in guest memory that calls the image's
__guest_thread_start(thread); returns 0 or an errno value */
int host_thread_create(unsigned int thread, unsigned int stack_size);

/* ---------- memory write tracking (port/linux/src/memory_watch.c) */

void host_memory_watch_initialize(void);
void host_memory_watch_protect(unsigned int address, unsigned int size);
unsigned int host_memory_watch_generation(unsigned int address, unsigned int size);
unsigned int host_memory_watch_serial(void);
void host_memory_watch_prepare_write(unsigned int address, unsigned int size);
void host_memory_watch_forget(unsigned int address, unsigned int size);

/* ---------- SDL (guest/runtime/guest_sdl.c)

Window, context, gamepad and audio stream objects are small integer
handles on this side. */

int host_sdl_init(unsigned int flags);
int host_sdl_set_hint(const char *name, const char *value);
void host_sdl_get_error(char *buffer, unsigned int size);
long long host_sdl_ticks(void);
long long host_sdl_thread_id(void);
unsigned int host_sdl_create_window(const char *title, int width, int height, long long flags);
void host_sdl_window_size_in_pixels(unsigned int window, int *width, int *height);
int host_sdl_set_relative_mouse(unsigned int window, int enabled);
int host_sdl_gl_set_attribute(int attribute, int value);
unsigned int host_sdl_gl_create_context(unsigned int window);
int host_sdl_gl_make_current(unsigned int window, unsigned int context);
int host_sdl_gl_set_swap_interval(int interval);
int host_sdl_gl_swap_window(unsigned int window);
int host_sdl_poll_event(void *event);
int host_sdl_set_clipboard_text(const char *text);
void host_sdl_get_clipboard_text(char *buffer, unsigned int size);
/* the keyboard's keys by name (the controls' bindings, xinput_sdl.c) */
void host_sdl_scancode_name(int scancode, char *buffer, unsigned int size);
int host_sdl_scancode_from_name(const char *name);
int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y);
int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message);
int host_sdl_get_gamepads(unsigned int *ids, int capacity);
unsigned int host_sdl_open_gamepad(unsigned int id);
unsigned int host_sdl_gamepad_from_id(unsigned int id);
int host_sdl_gamepad_axis(unsigned int gamepad, int axis);
int host_sdl_gamepad_button(unsigned int gamepad, int button);
int host_sdl_gamepad_type(unsigned int gamepad);
int host_sdl_rumble_gamepad(unsigned int gamepad, unsigned int low, unsigned int high, unsigned int milliseconds);
/* callback: void (*)(void *userdata, unsigned int stream, int additional, int total),
called on the audio thread */
unsigned int host_sdl_open_audio_stream(unsigned int device, const void *spec, unsigned int callback, unsigned int userdata);
int host_sdl_put_audio_stream_data(unsigned int stream, const void *data, int length);
int host_sdl_resume_audio_stream_device(unsigned int stream);

/* ---------- OpenGL ES */

/* copies glGetString(name) (or glGetStringi when index >= 0) */
void host_gl_get_string(unsigned int name, int index, char *buffer, unsigned int size);
/* nonzero if the context supports the named extension */
int host_gl_has_extension(const char *name);
/* copies size bytes at offset of a GL buffer object into data, waiting for
the GPU's writes to it */
void host_gl_read_buffer(unsigned int buffer, unsigned int offset, unsigned int size, void *data);
/* unsynchronized write into the buffer bound to target */
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data);
/* fences the GPU work queued so far as that of ring slot `slot`; waits for
the GPU to finish the work last fenced for a slot */
void host_gl_fence_frame(unsigned int slot);
void host_gl_wait_frame(unsigned int slot);

/* ---------- Android */

/* the storage directories the port uses, copied into buffer */
void host_android_path(int which, char *buffer, unsigned int size);

#endif
