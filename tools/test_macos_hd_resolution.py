#!/usr/bin/env python3
"""Compile actual resolution/pointer/projection source into a bounded fixture."""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def function(text, name):
    start = re.search(r"(?:static )?(?:BOOL|void) " + name + r"\(", text).start()
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

def main():
    platform = (ROOT / "port/linux/src/sdl_platform.c").read_text()
    renderer = (ROOT / "port/linux/src/d3d8_gl.c").read_text()
    camera = (ROOT / "source/render/render_cameras.c").read_text()
    projection = re.search(r"field_of_view_tangent = tangent\(camera->vertical_field_of_view.*?projection_y_scale =.*?;", camera, re.S).group()
    source = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define HALO_MACOS 1
#define HALO_ANDROID 1
#define SCREEN_HEIGHT 480
#define SCREEN_MAXIMUM_WIDTH 1920
#define TRUE 1
#define FALSE 0
typedef int BOOL;
static const char *preset;
static const char *config_string(const char *key) { (void)key; return preset; }
static long config_integer(const char *key) { (void)key; return 0; }
static BOOL platform_screen_mode(long *w,long *h) { *w=1920;*h=1200;return TRUE; }
static long logical_width;
static long halo_screen_width(void) { return logical_width; }
struct render_target_entry { struct { long width,height,gl_width,gl_height; } target; } target;
struct { int back_buffer; } device;
static struct render_target_entry *render_target_get(void *p) { (void)p;return &target; }
static int point_w,point_h,pixel_w,pixel_h;
static void platform_video_window_size(int *w,int *h) { *w=point_w;*h=point_h; }
static void platform_video_drawable_size(int *w,int *h) { *w=pixel_w;*h=pixel_h; }
'''
    source += function(platform, "platform_render_resolution") + "\n"
    source += function(renderer, "screen_mode_choose") + "\n"
    source += function(renderer, "ui_point_from_window") + "\n"
    source += r'''
static float projection_x(long width) {
 struct { float vertical_field_of_view; } value={1.0f}, *camera=&value;
 float field_of_view_tangent, projection_x_scale, projection_y_scale;
 float half_bounds_width=1,half_bounds_height=1,viewport_height=480,viewport_width=(float)width;
#define tangent tanf
'''+projection+r'''
 assert(fabsf(projection_y_scale-1.0f/tanf(0.5f))<0.00001f);
 return projection_x_scale;
}
int main(void) {
 const char *presets[]={"1280x720","1920x1080","640x480","invalid"};
 const int widths[]={1280,1920,640,1280}, heights[]={720,1080,480,720};
 for(int i=0;i<4;i++) {
  long w,h;float scale[2];short x,y;
  preset=presets[i];assert(platform_render_resolution(&w,&h));
  assert(w==widths[i]&&h==heights[i]);
  screen_mode_choose(&logical_width,scale);
  assert(lroundf(logical_width*scale[0])==w);
  assert(lroundf(480*scale[1])==h);
  target.target.width=logical_width;target.target.height=480;
  target.target.gl_width=w;target.target.gl_height=h;
  point_w=w/2;point_h=h/2;pixel_w=w;pixel_h=h;
  ui_point_from_window(point_w/2.0f,point_h/2.0f,&x,&y);
  assert(x==320&&y==240);
  if(h!=480) assert(projection_x(logical_width)<projection_x(640)*0.76f);
  /* Letterboxed fullscreen: pointer center must stay centered in the UI. */
  point_w=1710;point_h=1073;pixel_w=3420;pixel_h=2146;
  ui_point_from_window(point_w/2.0f,point_h/2.0f,&x,&y);
  assert(x==320&&y==240);
 }
 preset="native";long w,h;assert(!platform_render_resolution(&w,&h));
 puts("HD resolution: exact720/1080targets, wider actual projection, Retina/fullscreen pointer center and native fallback PASS");
}
'''
    output = ROOT / "build/macos-aot/hd-resolution-regression"
    output.mkdir(exist_ok=True)
    (output / "fixture.c").write_text(source)
    subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", str(output/"fixture.c"), "-lm", "-o", str(output/"fixture")], check=True)
    subprocess.run([str(output/"fixture")], check=True)

if __name__ == "__main__":
    main()
