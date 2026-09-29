#!/usr/bin/env python3
"""Exercise actual preset cycle and persistent config writer, with Retina window stubs."""
from pathlib import Path
import subprocess
from test_macos_hd_resolution import function
ROOT=Path(__file__).resolve().parents[1]
def main():
 out=ROOT/'build/macos-aot/display-cycle-regression';out.mkdir(exist_ok=True)
 config=(ROOT/'port/linux/src/port_config.c').read_text().replace('#include "platform.h"','static void platform_log(const char *format, ...) { (void)format; }')
 platform=(ROOT/'port/linux/src/sdl_platform.c').read_text()
 prefix='#define HALO_MACOS 1\n#include <assert.h>\n#include <unistd.h>\n'
 cycle=r'''
static SDL_Window *platform_window;
static int point_width=640,point_height=360;
static bool cycle_get_size(SDL_Window *w,int *x,int *y) { (void)w;*x=point_width;*y=point_height;return true; }
static bool cycle_get_pixels(SDL_Window *w,int *x,int *y) { (void)w;*x=point_width*2;*y=point_height*2;return true; }
static bool cycle_set_size(SDL_Window *w,int x,int y) { (void)w;point_width=x;point_height=y;return true; }
#define SDL_GetWindowSize cycle_get_size
#define SDL_GetWindowSizeInPixels cycle_get_pixels
#define SDL_SetWindowSize cycle_set_size
'''+function(platform,'platform_cycle_render_resolution')+r'''
int main(void) {
 char directory[]="/tmp/halo-display-cycle-XXXXXX",path[1024];assert(mkdtemp(directory));
 setenv("HALO_CONFIG_ROOT",directory,1);unsetenv("HALO_RESOLUTION");
 snprintf(path,sizeof(path),"%s/config.toml",directory);
 assert(SDL_SaveFile(path,"# keep my comment\n[display]\nresolution = \"1280x720\"\nfullscreen = false\n[audio]\nvolume = 0.4\n",99));
 assert(!strcmp(config_string("display.resolution"),"1280x720"));
 const char *values[]={"1920x1080","640x480","1280x720"};
 const int widths[]={960,320,640},heights[]={540,240,360};
 for(int i=0;i<3;i++) {
  platform_cycle_render_resolution();assert(!strcmp(config_string("display.resolution"),values[i]));
  assert(point_width==widths[i]&&point_height==heights[i]);
  size_t size;char *text=config_read_file(path,&size);assert(text);
  assert(strstr(text,"# keep my comment")&&strstr(text,"volume = 0.4"));
  toml_result_t parsed=toml_parse(text,(int)size);assert(parsed.ok);
  toml_datum_t value=toml_seek(parsed.toptab,"display.resolution");assert(value.type==TOML_STRING&&!strcmp(value.u.s,values[i]));
  toml_free(parsed);free(text);
 }
 assert(!config_write_string("display.resolution","bad\"value"));
 assert(!strcmp(config_string("display.resolution"),"1280x720"));
 assert(config_write_boolean("display.fullscreen",1));assert(config_boolean("display.fullscreen"));
 assert(!strcmp(config_string("display.resolution"),"1280x720"));
 unlink(path);rmdir(directory);puts("Display cycle: exact Retina presets, persistent TOML/comments/unrelated settings, invalid strings and boolean compatibility PASS");
}
'''
 # Preserve the exact literal size without relying on hand-counted characters.
 cycle=cycle.replace(',99));',',strlen("# keep my comment\\n[display]\\nresolution = \\\"1280x720\\\"\\nfullscreen = false\\n[audio]\\nvolume = 0.4\\n")));')
 (out/'fixture.c').write_text(prefix+config+cycle)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-I'+str(ROOT/'port/linux/src'),'-I'+str(ROOT/'port/third_party/tomlc17'),'-I/opt/homebrew/include',str(out/'fixture.c'),str(ROOT/'port/third_party/tomlc17/tomlc17.c'),'-L/opt/homebrew/lib','-lSDL3','-o',str(out/'fixture')],check=True)
 subprocess.run([str(out/'fixture')],check=True)
if __name__=='__main__':main()
