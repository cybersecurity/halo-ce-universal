#pragma once
#include <stdint.h>
void halo_debug_present(void);
void halo_debug_install_menu(void);
void halo_debug_initialize(void);
int halo_debug_is_presented(void);
void halo_debug_use_program(uint32_t program);
double halo_debug_draw_begin(void);
void halo_debug_draw_end(double started);
void halo_debug_shader_source(uint32_t shader,int count,const char *const *sources,const int *lengths);
void halo_debug_attach(uint32_t program,uint32_t shader);
double halo_debug_begin(void);
void halo_debug_end(uint32_t object,int program,double started);
void halo_debug_forget(uint32_t object,int program);
int halo_metalfx_enabled(void);
void halo_metalfx_set_enabled(int enabled);
int halo_metalfx_supported(void);
int host_ios_metalfx_present(unsigned int texture,unsigned int width,unsigned int height,unsigned int output_width,unsigned int output_height);
unsigned int host_ios_render_height(void);

void halo_debug_replay_shader_report(void);
void halo_debug_test_export(void);
