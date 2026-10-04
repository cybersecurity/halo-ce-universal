/* Darwin packs stack arguments; the Android guest bridge widens each slot. */
#include <GLES3/gl32.h>
#include <string.h>
static void bridge_glCopyImageSubData(GLuint a0, GLenum a1, GLint a2, GLint a3, GLint a4, GLint a5,
                                      GLuint a6, GLenum a7, long long a8, long long a9,
                                      long long a10, long long a11, long long a12, long long a13,
                                      long long a14) {
    glCopyImageSubData(a0, a1, a2, a3, a4, a5, a6, a7, (GLint)a8, (GLint)a9, (GLint)a10, (GLint)a11,
                       (GLsizei)a12, (GLsizei)a13, (GLsizei)a14);
}
static void bridge_glTexImage2D(GLenum a0, GLint a1, GLint a2, GLsizei a3, GLsizei a4, GLint a5,
                                GLenum a6, GLenum a7, long long a8) {
    glTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, (const void *)a8);
}
static void bridge_glTexImage3D(GLenum a0, GLint a1, GLint a2, GLsizei a3, GLsizei a4, GLsizei a5,
                                GLint a6, GLenum a7, long long a8, long long a9) {
    glTexImage3D(a0, a1, a2, a3, a4, a5, a6, a7, (GLenum)a8, (const void *)a9);
}
static void bridge_glTexSubImage2D(GLenum a0, GLint a1, GLint a2, GLint a3, GLsizei a4, GLsizei a5,
                                   GLenum a6, GLenum a7, long long a8) {
    glTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7, (const void *)a8);
}
static void bridge_glCompressedTexImage3D(GLenum a0, GLint a1, GLenum a2, GLsizei a3, GLsizei a4,
                                          GLsizei a5, GLint a6, GLsizei a7, long long a8) {
    glCompressedTexImage3D(a0, a1, a2, a3, a4, a5, a6, a7, (const void *)a8);
}
static void bridge_glBlitFramebuffer(GLint a0, GLint a1, GLint a2, GLint a3, GLint a4, GLint a5,
                                     GLint a6, GLint a7, long long a8, long long a9) {
    glBlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, (GLbitfield)a8, (GLenum)a9);
}
void *host_gl_bridge(const char *name) {
    if (!strcmp(name, "glCopyImageSubData"))
        return (void *)bridge_glCopyImageSubData;
    if (!strcmp(name, "glTexImage2D"))
        return (void *)bridge_glTexImage2D;
    if (!strcmp(name, "glTexImage3D"))
        return (void *)bridge_glTexImage3D;
    if (!strcmp(name, "glTexSubImage2D"))
        return (void *)bridge_glTexSubImage2D;
    if (!strcmp(name, "glCompressedTexImage3D"))
        return (void *)bridge_glCompressedTexImage3D;
    if (!strcmp(name, "glBlitFramebuffer"))
        return (void *)bridge_glBlitFramebuffer;
    return 0;
}
