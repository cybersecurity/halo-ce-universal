#include "input_bindings.h"
#include <string.h>
#include <ctype.h>
static SDL_Scancode key_name(const char *name)
{
 static const struct { const char *name; SDL_Scancode key; } names[] = {
  {"SPACE",SDL_SCANCODE_SPACE},{"TAB",SDL_SCANCODE_TAB},{"ENTER",SDL_SCANCODE_RETURN},
  {"ESCAPE",SDL_SCANCODE_ESCAPE},{"BACKSPACE",SDL_SCANCODE_BACKSPACE},
  {"LEFTCTRL",SDL_SCANCODE_LCTRL},{"CTRL",SDL_SCANCODE_LCTRL},
  {"LEFTSHIFT",SDL_SCANCODE_LSHIFT},{"SHIFT",SDL_SCANCODE_LSHIFT},
  {"LEFTALT",SDL_SCANCODE_LALT},{"ALT",SDL_SCANCODE_LALT},
  {"UP",SDL_SCANCODE_UP},{"DOWN",SDL_SCANCODE_DOWN},{"LEFT",SDL_SCANCODE_LEFT},{"RIGHT",SDL_SCANCODE_RIGHT}};
 unsigned int i;
 if (name[0] && !name[1]) {
  if (name[0]>='A' && name[0]<='Z') return (SDL_Scancode)(SDL_SCANCODE_A+name[0]-'A');
  if (name[0]>='1' && name[0]<='9') return (SDL_Scancode)(SDL_SCANCODE_1+name[0]-'1');
  if (name[0]=='0') return SDL_SCANCODE_0;
 }
 for(i=0;i<sizeof(names)/sizeof(names[0]);i++) if(!strcmp(name,names[i].name)) return names[i].key;
 return SDL_SCANCODE_UNKNOWN;
}
int input_binding_parse(const char *text, struct input_binding *binding)
{
 struct input_binding result = {{0},0,0,0};
 if (!text || !binding) return 0;
 while (*text) {
  char token[32]; unsigned int n=0; SDL_Scancode key;
  while (*text==' ' || *text=='\t') text++;
  while (*text && *text!=',') { if(n+1>=sizeof(token)) return 0; token[n++]=(char)toupper((unsigned char)*text++); }
  while(n && (token[n-1]==' ' || token[n-1]=='\t')) n--;
  token[n]=0;
  if (!n) return 0;
  if(!strcmp(token,"WHEEL")) result.wheel=1;
  else if(!strncmp(token,"MOUSE",5) && token[5]>='1' && token[5]<='5' && !token[6]) result.mouse_mask|=(unsigned char)(1u<<(token[5]-'0'));
  else { key=key_name(token); if(key==SDL_SCANCODE_UNKNOWN || result.key_count==8) return 0; result.keys[result.key_count++]=key; }
  if (*text==',') { text++; if(!*text) return 0; }
 }
 *binding=result; return 1;
}
int input_binding_down(const struct input_binding *binding, const unsigned char *keys,
 const unsigned char *buttons, int captured)
{
 unsigned int i;
 for(i=0;i<binding->key_count;i++) if(keys[binding->keys[i]]) return 1;
 if(captured) for(i=1;i<=5;i++) if((binding->mouse_mask&(1u<<i)) && buttons[i]) return 1;
 return 0;
}
