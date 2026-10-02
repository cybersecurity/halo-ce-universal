'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const vm = require('node:vm');
const { createHash } = require('node:crypto');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '../..');

test('vendored streaming SHA256 matches native SHA256 across padding and read boundaries', () => {
  const context = { Uint8Array, ArrayBuffer };
  context.self = context;
  vm.runInNewContext(fs.readFileSync(path.join(root, 'port/web/site/vendor/sha256.js'), 'utf8'), context);
  for (const length of [0, 1, 55, 56, 63, 64, 65, 127, 128, 129, 1048643]) {
    const bytes = Uint8Array.from({ length }, (_, i) => (i * 79 + 17) & 255);
    const expected = createHash('sha256').update(bytes).digest('hex');
    for (const chunk of [1, 317, 1024 * 1024]) {
      const digest = context.sha256.create();
      for (let at = 0; at < length; at += chunk) digest.update(bytes.subarray(at, at + chunk));
      assert.equal(digest.hex(), expected, `${length} bytes in ${chunk}-byte blocks`);
    }
  }
  assert.equal(context.sha256('abc'), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
});

test('the upload recorder stops allocating backing stores after warming its reusable arena and shadows', () => {
  const allocations = [];
  class Bytes extends Uint8Array {
    constructor(...args) {
      super(...args);
      if (typeof args[0] === 'number' && args[0]) allocations.push(args[0]);
    }
  }
  class GL {
    bindBuffer() {} bufferData() {} bufferSubData() {} uniform4fv() {} drawArrays() {}
  }
  const gl = Object.assign(new GL(), { ARRAY_BUFFER: 34962, STREAM_DRAW: 35040 });
  const context = { Uint8Array: Bytes, ArrayBuffer,
    WebGLRenderingContext: { prototype: GL.prototype }, WebGL2RenderingContext: { prototype: {} } };
  vm.runInNewContext(fs.readFileSync(path.join(root, 'port/web/stream-batch.js'), 'utf8'), context);
  const controller = context.HaloStreamBatch.install(gl);
  const buffers = [{}, {}, {}], payload = new Uint8Array(8192), uniform = new Float32Array(768);
  for (const buffer of buffers) { gl.bindBuffer(gl.ARRAY_BUFFER, buffer); gl.bufferData(gl.ARRAY_BUFFER, 16 * 1024 * 1024, gl.STREAM_DRAW); }
  const frame = tick => {
    gl.bindBuffer(gl.ARRAY_BUFFER, buffers[tick % buffers.length]);
    for (let draw = 0; draw < 100; draw++) {
      gl.bufferSubData(gl.ARRAY_BUFFER, draw * payload.length, payload);
      gl.uniform4fv({}, uniform);
      gl.drawArrays(4, 0, 3);
    }
    controller.flush();
  };
  for (let tick = 0; tick < 3; tick++) frame(tick);
  assert.ok(allocations.reduce((sum, size) => sum + size, 0) < 24 * 1024 * 1024,
    'three partially used 16 MiB slots must not allocate full CPU mirrors or per-draw snapshots');
  const warmed = allocations.length;
  for (let tick = 3; tick < 120; tick++) frame(tick);
  assert.equal(allocations.length, warmed, 'continued rendering reuses every backing store');
});

test('browser resolution changes reuse color/depth textures and their framebuffers', () => {
  const source = fs.readFileSync(path.join(root, 'port/linux/src/d3d8_gl.c'), 'utf8');
  const target = source.slice(source.indexOf('static struct render_target_entry *render_target_get('),
    source.indexOf('\nstruct xgpu_render_target *xgpu_render_target_find'));
  const start = source.indexOf('static GLuint framebuffer_get(GLuint color, GLuint depth)\n{');
  const framebuffer = source.slice(start, source.indexOf('\n/* the pixels per unit', start));
  const program = `
#include <assert.h>
#include <stdlib.h>
#define HALO_WEB 1
#define SCREEN_HEIGHT 480
#define GL_TEXTURE_2D 1
#define GL_TEXTURE_MAX_LEVEL 2
#define GL_DEPTH24_STENCIL8 3
#define GL_DEPTH_STENCIL 4
#define GL_UNSIGNED_INT_24_8 5
#define GL_RGBA8 6
#define GL_BGRA 7
#define GL_UNSIGNED_BYTE 8
#define GL_COLOR_ATTACHMENT0 9
#define GL_NONE 0
#define GL_FRAMEBUFFER 10
#define GL_DEPTH_STENCIL_ATTACHMENT 11
#define GL_FRAMEBUFFER_COMPLETE 12
typedef unsigned int GLuint, GLenum;
typedef int GLint, GLsizei, BOOL;
typedef struct { unsigned long Data, width, height; BOOL depth; } D3DSurface;
struct xgpu_render_target { unsigned long data, width, height, gl_width, gl_height; BOOL depth; float scale[2]; GLuint texture; };
struct render_target_entry { struct render_target_entry *next, *next_in_bucket; struct xgpu_render_target target; unsigned long last_rendered; };
struct framebuffer_entry { struct framebuffer_entry *next; GLuint color, depth, framebuffer; };
static struct render_target_entry *render_targets, *buckets[16];
static struct framebuffer_entry *framebuffers;
static float screen_scale[2] = {1, 1};
static unsigned textures, fbos, uploads;
static struct render_target_entry **render_target_bucket(unsigned long data) { return &buckets[data % 16]; }
static long halo_screen_width(void) { return 640; }
static void surface_dimensions(const D3DSurface *s, unsigned long *w, unsigned long *h, BOOL *d) { *w=s->width; *h=s->height; *d=s->depth; }
static void glGenTextures(int n, GLuint *id) { assert(n==1); *id=++textures; }
static void glBindTexture(GLenum t, GLuint id) { (void)t; (void)id; }
static void glTexParameteri(GLenum t, GLenum p, GLint v) { (void)t; (void)p; (void)v; }
static void glTexImage2D(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum format, GLenum type, const void *data) {
  (void)t; (void)l; (void)f; (void)b; (void)format; (void)type; (void)data; assert(w>0 && h>0); uploads++;
}
static void xgpu_gl_state_invalidate(void) {}
static void glGenFramebuffers(int n, GLuint *id) { assert(n==1); *id=++fbos; }
static void glBindFramebuffer(GLenum t, GLuint id) { (void)t; (void)id; }
static void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint id, GLint l) { (void)t; (void)a; (void)tt; (void)id; (void)l; }
static void glDrawBuffers(int n, const GLenum *buffer) { assert(n==1); (void)buffer; }
static GLenum glCheckFramebufferStatus(GLenum t) { (void)t; return GL_FRAMEBUFFER_COMPLETE; }
static void platform_log(const char *fmt, GLuint c, GLuint d) { (void)fmt; (void)c; (void)d; assert(0); }
${target}
${framebuffer}
int main(void) {
  D3DSurface color={16,640,480,0}, depth={32,640,480,1}, small={48,128,128,0};
  struct render_target_entry *c=render_target_get(&color), *d=render_target_get(&depth), *s=render_target_get(&small);
  GLuint framebuffer=framebuffer_get(c->target.texture,d->target.texture);
  assert(render_target_get(NULL)==NULL);
  for (int i=0; i<200; i++) {
    screen_scale[0]=screen_scale[1]=0.5f+i*0.01f;
    assert(render_target_get(&color)==c && render_target_get(&depth)==d);
    assert(render_target_get(&small)==s && s->target.gl_width==128);
    assert(c->target.gl_width==(unsigned long)(640*screen_scale[0]+0.5f));
    assert(c->target.gl_height==(unsigned long)(480*screen_scale[1]+0.5f));
    assert(framebuffer_get(c->target.texture,d->target.texture)==framebuffer);
  }
  assert(textures==3 && fbos==1 && uploads==403);
  while (render_targets) { struct render_target_entry *next=render_targets->next; free(render_targets); render_targets=next; }
  while (framebuffers) { struct framebuffer_entry *next=framebuffers->next; free(framebuffers); framebuffers=next; }
  return 0;
}
`;
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'halo-web-memory-'));
  try {
    const input = path.join(directory, 'test.c'), output = path.join(directory, 'test');
    fs.writeFileSync(input, program);
    execFileSync('cc', ['-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', input, '-o', output], { stdio: 'pipe' });
    execFileSync(output, [], { stdio: 'pipe' });
  } finally { fs.rmSync(directory, { recursive: true, force: true }); }
});
