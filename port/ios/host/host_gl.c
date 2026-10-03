/* GLES services whose pointers or state cannot cross the 32-bit guest ABI. */
#include "ios_host.h"
#include <SDL3/SDL.h>
#include <GLES3/gl32.h>
#include <string.h>
#define GL_FUNCTION(ret,name,args) static ret (GL_APIENTRY *name) args; if(!name)name=(void*)SDL_GL_GetProcAddress(#name)
void host_gl_get_string(uint32_t name,int index,char *buffer,uint32_t size) {
    GL_FUNCTION(const GLubyte *,glGetString,(GLenum));
    GL_FUNCTION(const GLubyte *,glGetStringi,(GLenum,GLuint));
    const GLubyte *text=index<0?glGetString(name):glGetStringi(name,index);
    if(size)SDL_strlcpy(buffer,text?(const char *)text:"",size);
}
int host_gl_has_extension(const char *name) {return SDL_GL_ExtensionSupported(name);}
uint32_t host_gl_read_buffer_word(uint32_t buffer,uint32_t offset) {
    GL_FUNCTION(void,glBindBuffer,(GLenum,GLuint));GL_FUNCTION(void *,glMapBufferRange,(GLenum,GLintptr,GLsizeiptr,GLbitfield));
    GL_FUNCTION(GLboolean,glUnmapBuffer,(GLenum));GL_FUNCTION(void,glGetIntegerv,(GLenum,GLint *));
    GLint previous;uint32_t value=0;glGetIntegerv(GL_COPY_READ_BUFFER_BINDING,&previous);glBindBuffer(GL_COPY_READ_BUFFER,buffer);
    void *p=glMapBufferRange(GL_COPY_READ_BUFFER,offset,4,GL_MAP_READ_BIT);if(p){memcpy(&value,p,4);glUnmapBuffer(GL_COPY_READ_BUFFER);}
    glBindBuffer(GL_COPY_READ_BUFFER,previous);return value;
}
static GLsync fences[8];
void host_gl_fence_frame(uint32_t slot) {
    GL_FUNCTION(void,glDeleteSync,(GLsync));GL_FUNCTION(GLsync,glFenceSync,(GLenum,GLbitfield));
    if(slot>=8)return;if(fences[slot])glDeleteSync(fences[slot]);fences[slot]=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
}
void host_gl_wait_frame(uint32_t slot) {
    GL_FUNCTION(void,glDeleteSync,(GLsync));GL_FUNCTION(GLenum,glClientWaitSync,(GLsync,GLbitfield,GLuint64));
    if(slot>=8 || !fences[slot])return;glClientWaitSync(fences[slot],GL_SYNC_FLUSH_COMMANDS_BIT,1000000000ull);
    glDeleteSync(fences[slot]);fences[slot]=NULL;
}
void host_gl_buffer_write(uint32_t target,uint32_t offset,uint32_t size,const void *data) {
    GL_FUNCTION(void *,glMapBufferRange,(GLenum,GLintptr,GLsizeiptr,GLbitfield));
    GL_FUNCTION(GLboolean,glUnmapBuffer,(GLenum));GL_FUNCTION(void,glBufferSubData,(GLenum,GLintptr,GLsizeiptr,const void *));
    void *p=glMapBufferRange(target,offset,size,GL_MAP_WRITE_BIT|GL_MAP_UNSYNCHRONIZED_BIT|GL_MAP_INVALIDATE_RANGE_BIT);
    if(p){memcpy(p,data,size);glUnmapBuffer(target);}else glBufferSubData(target,offset,size,data);
}
