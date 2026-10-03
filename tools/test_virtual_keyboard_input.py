"""Exercise the production virtual-keyboard lifecycle and keyboard-to-pad mapping.

Only tag lookup, profile validation and unrelated UI services are stubbed. The
fixtures need no game data or running application.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

if __package__:
    from .test_custom_maps import c_block, function
else:
    from test_custom_maps import c_block, function

ROOT = Path(__file__).resolve().parents[1]

STUBS = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char byte, boolean, BYTE;
typedef unsigned short word;
typedef int BOOL;
typedef short SHORT;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define csmemset memset
#define csmemmove memmove
#define match_assert(file,line,test) assert(test)
#define match_vassert(file,line,test,message) assert(test)
#define error(...) ((void)0)
#define BITMAP_GROUP_TAG 1
#define VIRTUAL_KEYBOARD_TAG 2
#define virtual_keyboard_definition_get(index) ((struct virtual_keyboard_definition *)1)
enum {_error_already_a_saved_game_file_with_that_name, _error_cannot_create_saved_game_file_with_empty_name};
static int keyboard_available=1, unique_name=1, valid_name=1, action;
static long tag_loaded(int group,const char *name) {return keyboard_available?0:NONE;}
static void event_manager_flush(void) {}
static void ui_play_audio_feedback_sound(int sound) {}
static void display_error(int code,int controller,int a,int b) {}
static unsigned long system_milliseconds(void) {return 0;}
static int saved_game_file_name_unique(const wchar_t *name) {return unique_name;}
static int player_name_clean(wchar_t *name,long count) {return valid_name;}
static size_t ustrlen(const wchar_t *s) {size_t n=0;while(s[n])n++;return n;}
static int ustrcmp(const wchar_t *a,const wchar_t *b) {while(*a && *a==*b){a++;b++;}return *a-*b;}
static void ustrncpy(wchar_t *d,const wchar_t *s,size_t n) {for(size_t i=0;i<n;i++)d[i]=*s?*s++:0;}
static void ustrcpy(wchar_t *d,const wchar_t *s) {do{*d++=*s;}while(*s++);}
static wchar_t virtual_keyboard_get_current_character(void) {return L'x';}
static void virtual_keyboard_process_internal(void);
enum {
    SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_UP,
    SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_ESCAPE, SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W,
    SDL_SCANCODE_S, SDL_SCANCODE_F1, SDL_SCANCODE_SPACE, SDL_SCANCODE_BACKSPACE,
    SDL_SCANCODE_DELETE, SDL_SCANCODE_E, SDL_SCANCODE_TAB
};
enum {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y};
enum {XINPUT_GAMEPAD_DPAD_UP=1, XINPUT_GAMEPAD_DPAD_DOWN=2,
    XINPUT_GAMEPAD_DPAD_LEFT=4, XINPUT_GAMEPAD_DPAD_RIGHT=8,
    XINPUT_GAMEPAD_START=16, XINPUT_GAMEPAD_BACK=32, SDL_BUTTON_X1=4};
typedef struct {unsigned short wButtons;BYTE bAnalogButtons[8];SHORT sThumbLX,sThumbLY;} XINPUT_GAMEPAD;
struct platform_input_state {unsigned char keys[32],mouse_buttons[8];int mouse_released;};
'''

MAIN = r'''
static void virtual_keyboard_process_internal(void) {
    if(action==1) virtual_keyboard_select();
    if(action==2) virtual_keyboard_cancel();
}
static XINPUT_GAMEPAD poll_key(int key) {
    struct platform_input_state input={0};XINPUT_GAMEPAD pad={0};
    if(key>=0) input.keys[key]=1;
    keyboard_gamepad(&input,&pad);return pad;
}
static void expect_menu_enter(int key) {
    poll_key(-1);
    XINPUT_GAMEPAD pad=poll_key(key);
    assert(pad.bAnalogButtons[XINPUT_GAMEPAD_A]==255);
    assert(!(pad.wButtons&XINPUT_GAMEPAD_START));
}
static void open_keyboard(wchar_t *name,int key) {
    assert(virtual_keyboard_launch(name,32,8));
    /* Launch changes input mode before the first processing frame. */
    XINPUT_GAMEPAD pad=poll_key(key);
    assert(!pad.bAnalogButtons[XINPUT_GAMEPAD_A]);
    assert(!(pad.wButtons&XINPUT_GAMEPAD_START));
    /* Match ui_widgets_process: it calls this only while active. */
    virtual_keyboard_process();
    pad=poll_key(key);
    assert(!pad.bAnalogButtons[XINPUT_GAMEPAD_A]);
    assert(!(pad.wButtons&XINPUT_GAMEPAD_START));
    poll_key(-1);pad=poll_key(key);
    assert(pad.wButtons&XINPUT_GAMEPAD_START);
    assert(!pad.bAnalogButtons[XINPUT_GAMEPAD_A]);
}
int main(int argc,char **argv) {
    assert(argc==3);int test=atoi(argv[1]),key=atoi(argv[2])?SDL_SCANCODE_KP_ENTER:SDL_SCANCODE_RETURN;
    wchar_t name[16]=L"Player";
    assert(virtual_keyboard_initialize());
    expect_menu_enter(key);
    if(test==4) {
        virtual_keyboard_dispose();keyboard_available=0;assert(!virtual_keyboard_initialize());
        assert(!virtual_keyboard_launch(name,sizeof(name),8));
        expect_menu_enter(key);return 0;
    }
    open_keyboard(name,key);
    if(test==0) {action=1;virtual_keyboard_process();assert(virtual_keyboard_last_exit_saved_text());}
    if(test==1) {action=2;virtual_keyboard_process();assert(!virtual_keyboard_last_exit_saved_text());}
    if(test==2) virtual_keyboard_close();
    if(test==3) virtual_keyboard_dispose();
    if(test==5) {
        platform_text_field(TRUE);virtual_keyboard_close();
        assert(poll_key(key).wButtons&XINPUT_GAMEPAD_START);
        platform_text_field(FALSE);
    }
    if(test==6) {
        name[0]=L'N';unique_name=0;action=1;virtual_keyboard_process();
        assert(!virtual_keyboard_last_exit_saved_text());
    }
    if(test==7) {assert(virtual_keyboard_initialize());}
    assert(!virtual_keyboard_active());
    /* Closing must clear typing immediately; no inactive process call. */
    XINPUT_GAMEPAD held=poll_key(key);
    assert(!(held.wButtons&XINPUT_GAMEPAD_START));
    assert(!held.bAnalogButtons[XINPUT_GAMEPAD_A]);
    expect_menu_enter(key);
    /* A second keyboard must arm independently after its opening Enter. */
    if(test!=3) {action=0;open_keyboard(name,key);virtual_keyboard_close();expect_menu_enter(key);}
    return 0;
}
'''


def fixture_source(keyboard=None, inputs=None):
    keyboard = keyboard or (ROOT / "source/interface/virtual_keyboard.c").read_text()
    inputs = inputs or (ROOT / "port/linux/src/xinput_sdl.c").read_text()
    constants = keyboard[keyboard.index("enum\n{"):keyboard.index("/* ---------- macros */")]
    state = c_block(keyboard, keyboard.index("struct virtual_keyboard_globals\n")) + ";\n"
    layout = c_block(keyboard, keyboard.index("static char const virtual_keyboard_layout_table")) + ";\n"
    # Include the production mode flags and all of their transition logic.
    input_mode = inputs[inputs.index("static BOOL text_typing;"):inputs.index("/* the keys held when the game")]
    lifecycle = []
    if "static void virtual_keyboard_set_active" in keyboard:
        lifecycle.append(function(keyboard, "virtual_keyboard_set_active"))
    for name in ("virtual_keyboard_initialize", "virtual_keyboard_dispose", "virtual_keyboard_launch",
                 "virtual_keyboard_active", "virtual_keyboard_last_exit_saved_text",
                 "virtual_keyboard_cancel", "virtual_keyboard_free_space_in_text_buffer",
                 "virtual_keyboard_backspace", "virtual_keyboard_close", "virtual_keyboard_select",
                 "virtual_keyboard_process"):
        lifecycle.append(function(keyboard, name))
    return "\n".join((STUBS, constants, state, layout,
                      "static struct virtual_keyboard_globals virtual_keyboard_globals;",
                      function(inputs, "analog"), input_mode, *lifecycle, MAIN))


class VirtualKeyboardInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang")
        if not compiler:
            raise unittest.SkipTest("clang is required for the production C fixture")
        cls.directory = tempfile.TemporaryDirectory(prefix="halo-keyboard-input-")
        cls.addClassCleanup(cls.directory.cleanup)
        source = Path(cls.directory.name) / "keyboard.c"
        cls.executable = source.with_suffix("")
        source.write_text(fixture_source())
        subprocess.run([compiler, "-std=c11", "-fshort-wchar", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", str(source), "-o", str(cls.executable)], check=True)

    def run_case(self, case):
        for keypad in (0, 1):
            with self.subTest(keypad=keypad):
                subprocess.run([str(self.executable), str(case), str(keypad)], check=True)

    def test_done_restores_enter_without_accepting_held_key(self):
        self.run_case(0)

    def test_cancel_restores_enter(self):
        self.run_case(1)

    def test_external_close_restores_enter(self):
        self.run_case(2)

    def test_dispose_restores_enter(self):
        self.run_case(3)

    def test_failed_launch_keeps_menu_input(self):
        self.run_case(4)

    def test_separate_text_field_keeps_its_typing_mode(self):
        self.run_case(5)

    def test_rejected_profile_name_restores_enter(self):
        self.run_case(6)

    def test_reinitialize_restores_enter(self):
        self.run_case(7)


if __name__ == "__main__":
    unittest.main()
