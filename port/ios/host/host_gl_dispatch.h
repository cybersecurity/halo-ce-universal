/* Include after GL declarations. Driver choice is fixed for the process. */
#pragma once
#include "host_graphics.h"
#define HALO_GL_PROC(name) ({static __typeof__(&name) fn; if(!fn)fn=(__typeof__(&name))halo_graphics_proc(#name); fn;})
#define glBindFramebuffer HALO_GL_PROC(glBindFramebuffer)
#define glBindTexture HALO_GL_PROC(glBindTexture)
#define glBlitFramebuffer HALO_GL_PROC(glBlitFramebuffer)
#define glCheckFramebufferStatus HALO_GL_PROC(glCheckFramebufferStatus)
#define glDisable HALO_GL_PROC(glDisable)
#define glEnable HALO_GL_PROC(glEnable)
#define glFinish HALO_GL_PROC(glFinish)
#define glFramebufferTexture2D HALO_GL_PROC(glFramebufferTexture2D)
#define glGenFramebuffers HALO_GL_PROC(glGenFramebuffers)
#define glGetIntegerv HALO_GL_PROC(glGetIntegerv)
#define glGetProgramiv HALO_GL_PROC(glGetProgramiv)
#define glGetShaderiv HALO_GL_PROC(glGetShaderiv)
#define glGetString HALO_GL_PROC(glGetString)
#define glIsEnabled HALO_GL_PROC(glIsEnabled)
