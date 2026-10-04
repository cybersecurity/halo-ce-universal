/* Mac video and events; handles, audio and controllers use the shared host. */
#include "host.h"
#include "../native_events.h"
#include "../native/host_menu.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "../../android/host/host_sdl.c"

static EGLDisplay metal_display;
static EGLContext metal_context;
static EGLSurface metal_surface;
static int requested_minor;
static SDL_Window *metal_window;
static SDL_MetalView metal_view;
static int metal_window_hidden;
static int metal_swap_interval = 1;
static int command_held;

/* ---------- general */

int host_sdl_init(uint32_t flags) {
    /* Handle close requests ourselves so Command-W does not also queue
       SDL_EVENT_QUIT while W is being used to move. */
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    if (!SDL_Init((SDL_InitFlags)flags))
        return 0;
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
    SDL_SetEventEnabled(SDL_EVENT_DROP_TEXT, true);
    return 1;
}

/* ---------- video */

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags) {
    const char *windowed = SDL_getenv("HALO_WINDOWED");
    SDL_WindowFlags mode = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    metal_window_hidden = (flags & SDL_WINDOW_HIDDEN) != 0;
    if (metal_window_hidden) mode |= SDL_WINDOW_HIDDEN;
    else if (windowed ? !SDL_atoi(windowed) : (flags & SDL_WINDOW_FULLSCREEN) != 0)
        mode |= SDL_WINDOW_FULLSCREEN;
    metal_window = SDL_CreateWindow(title, width > 0 ? width : 1280, height > 0 ? height : 960,
                                    mode);
    if (metal_window) {
        SDL_SyncWindow(metal_window);
        int w, h;
        SDL_GetWindowSizeInPixels(metal_window, &w, &h);
        host_logf(HOST_LOG_INFO, "Metal drawable %dx%d (%s)", w, h,
                  (mode & SDL_WINDOW_FULLSCREEN) ? "borderless fullscreen" : "windowed");
    }
    return handle_new(_handle_window, metal_window);
}

int host_sdl_is_fullscreen(void) {
    return metal_window && (SDL_GetWindowFlags(metal_window) & SDL_WINDOW_FULLSCREEN) != 0;
}
int host_sdl_display_mode(void) {
    if (!host_sdl_is_fullscreen()) return 0;
    return SDL_GetWindowFullscreenMode(metal_window) ? 2 : 1;
}
/* The guest's config.toml is authoritative; mode changes apply on its thread. */
int host_sdl_apply_display(int mode, int width, int height) {
    const SDL_DisplayMode *display = NULL;
    if (!metal_window || mode < -1 || mode > 2 || width < 0 || height < 0 ||
        ((width == 0) != (height == 0))) return 0;
    if (metal_window_hidden && mode >= 0) mode = 0;
    if (mode >= 0) {
        if (mode == 2) {
            SDL_DisplayID screen = SDL_GetDisplayForWindow(metal_window);
            display = screen ? SDL_GetDesktopDisplayMode(screen) : NULL;
            if (!display) return 0;
        }
        if (!SDL_SetWindowFullscreenMode(metal_window, display) ||
            !SDL_SetWindowFullscreen(metal_window, mode != 0)) return 0;
    }
    if (width && !SDL_SetWindowSize(metal_window, width, height)) return 0;
    if (!SDL_SyncWindow(metal_window)) return 0;
    return 1;
}
void host_sdl_release_mouse(void) {
    if (metal_window) SDL_SetWindowRelativeMouseMode(metal_window, false);
    /* Cocoa panels also invalidate the guest's held keys/buttons. */
    SDL_Event event = {.type = SDL_EVENT_USER};
    event.user.code = HALO_MACOS_MOUSE_RELEASE;
    SDL_PushEvent(&event);
}
void host_sdl_request_quit(void) {
    SDL_Event event = {.type = SDL_EVENT_QUIT};
    SDL_PushEvent(&event);
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled) {
    SDL_Window *object = handle_get(window, _handle_window);

    if (metal_window_hidden) return 1;
    return object ? SDL_SetWindowRelativeMouseMode(object, enabled != 0) : 0;
}

int host_sdl_gl_set_attribute(int attribute, int value) {
    if (attribute == SDL_GL_CONTEXT_MINOR_VERSION)
        requested_minor = value;
    return 1;
}
uint32_t host_sdl_gl_create_context(uint32_t window) {
    SDL_Window *object = handle_get(window, _handle_window);
    if (!object || requested_minor > 0) {
        SDL_SetError("ANGLE Metal uses ES 3.0");
        return 0;
    }
    PFNEGLGETPLATFORMDISPLAYEXTPROC getDisplay =
        (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    const EGLint displayAttributes[] = {0x3203, 0x3489, EGL_NONE};
    metal_display = getDisplay ? getDisplay(0x3202, (void *)0, displayAttributes) : EGL_NO_DISPLAY;
    EGLint major, minor, count;
    if (metal_display == EGL_NO_DISPLAY || !eglInitialize(metal_display, &major, &minor)) {
        SDL_SetError("Cannot initialize ANGLE Metal: %x", eglGetError());
        return 0;
    }
    const EGLint configAttributes[] = {EGL_SURFACE_TYPE,
                                       metal_window_hidden ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT,
                                       EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_ES3_BIT,
                                       EGL_RED_SIZE,
                                       8,
                                       EGL_GREEN_SIZE,
                                       8,
                                       EGL_BLUE_SIZE,
                                       8,
                                       EGL_ALPHA_SIZE,
                                       8,
                                       EGL_DEPTH_SIZE,
                                       24,
                                       EGL_STENCIL_SIZE,
                                       8,
                                       EGL_NONE};
    EGLConfig config;
    if (!eglChooseConfig(metal_display, configAttributes, &config, 1, &count) || !count)
        return 0;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    metal_context = eglCreateContext(metal_display, config, EGL_NO_CONTEXT, contextAttributes);
    if (metal_window_hidden) {
        /* Isolated launch/input checks use the shipped host without taking
           focus or capturing the user's mouse. Ordinary launches use MetalView. */
        int width, height;
        SDL_GetWindowSizeInPixels(object, &width, &height);
        const EGLint size[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        metal_surface = eglCreatePbufferSurface(metal_display, config, size);
    } else {
        metal_view = SDL_Metal_CreateView(object);
        metal_surface = eglCreateWindowSurface(
            metal_display, config, (EGLNativeWindowType)SDL_Metal_GetLayer(metal_view), NULL);
    }
    if (metal_context == EGL_NO_CONTEXT || metal_surface == EGL_NO_SURFACE) {
        SDL_SetError("Cannot create Metal surface: %x", eglGetError());
        return 0;
    }
    eglMakeCurrent(metal_display, metal_surface, metal_surface, metal_context);
    return handle_new(_handle_context, metal_context);
}
int host_sdl_gl_make_current(uint32_t window, uint32_t context) {
    (void)window;
    return eglMakeCurrent(metal_display, metal_surface, metal_surface,
                          handle_get(context, _handle_context));
}
int host_sdl_gl_set_swap_interval(int interval) {
    metal_swap_interval = interval;
    return eglSwapInterval(metal_display, interval);
}
int host_sdl_gl_swap_window(uint32_t window) {
    (void)window;
    uint64_t start = SDL_GetTicksNS();
    int result = eglSwapBuffers(metal_display, metal_surface);
    if (metal_window_hidden && metal_swap_interval > 0) {
        uint64_t elapsed = SDL_GetTicksNS() - start;
        if (elapsed < 16666667) SDL_DelayNS(16666667 - elapsed);
    }
    return result;
}

int host_sdl_poll_event(void *event) {
    SDL_Event host_event;

    if (!SDL_PollEvent(&host_event))
        return 0;
    if (host_event.type == SDL_EVENT_KEY_DOWN || host_event.type == SDL_EVENT_KEY_UP) {
        command_held = host_event.key.scancode == SDL_SCANCODE_LGUI ||
                       host_event.key.scancode == SDL_SCANCODE_RGUI
                           ? host_event.key.down : (host_event.key.mod & SDL_KMOD_GUI) != 0;
    }
    if (host_event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if (command_held) {
            host_logf(HOST_LOG_INFO, "Command-W does not close the game; use Command-Q to quit");
            memset(event, 0, sizeof(host_event));
            return 1;
        }
        host_event.type = SDL_EVENT_QUIT;
    }
    /* Cocoa sends opened URLs as drop-file events. Consume the native
       string here; the guest's 32-bit SDL event cannot hold that pointer. */
    if (host_event.type == SDL_EVENT_DROP_FILE || host_event.type == SDL_EVENT_DROP_TEXT) {
        if (host_is_discord_launch_url(host_event.drop.data))
            host_logf(HOST_LOG_INFO, "Internet play: Discord launch received; waiting for its invite");
        else if (host_invite_received(host_event.drop.data))
            host_logf(HOST_LOG_INFO, "Internet play: opened invite queued for this game");
        memset(event, 0, sizeof(host_event));
        return 1;
    }
    /* the layouts agree except for the pointers of text, drop and user
    events, which the guest does not read */
    memcpy(event, &host_event, sizeof(host_event));
    return 1;
}

int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y) {
    (void)duration; (void)gravity; (void)x; (void)y;
    host_logf(HOST_LOG_INFO, "%s", message);
    return 1;
}

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message) {
    return SDL_ShowSimpleMessageBox(flags, title, message, metal_window);
}
