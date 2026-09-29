#include "../../linux/src/input_bindings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
 struct input_binding binding;
 unsigned char keys[SDL_SCANCODE_COUNT]={0}, buttons[6]={0};
 assert(input_binding_parse(" e , R, Mouse2",&binding));
 keys[SDL_SCANCODE_E]=1; assert(input_binding_down(&binding,keys,buttons,0));
 keys[SDL_SCANCODE_E]=0; buttons[2]=1;
 assert(!input_binding_down(&binding,keys,buttons,0));
 assert(input_binding_down(&binding,keys,buttons,1));
 assert(input_binding_parse("Tab,Wheel",&binding) && binding.wheel);
 assert(input_binding_parse("",&binding) && !input_binding_down(&binding,keys,buttons,1));
 assert(!input_binding_parse("Mouse0",&binding));
 assert(!input_binding_parse("Space,",&binding));
 assert(!input_binding_parse("unknown",&binding));
 assert(!input_binding_parse("A,B,C,D,E,F,G,H,I",&binding));
 puts("input_bindings_test: ok");
}
