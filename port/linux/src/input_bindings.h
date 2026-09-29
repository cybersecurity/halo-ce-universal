#ifndef HALO_INPUT_BINDINGS_H
#define HALO_INPUT_BINDINGS_H
#include <SDL3/SDL_scancode.h>
struct input_binding { SDL_Scancode keys[8]; unsigned char key_count, mouse_mask, wheel; };
/* Comma separated physical key names; empty disables an action. */
int input_binding_parse(const char *text, struct input_binding *binding);
int input_binding_down(const struct input_binding *binding, const unsigned char *keys,
 const unsigned char *buttons, int captured);
#endif
