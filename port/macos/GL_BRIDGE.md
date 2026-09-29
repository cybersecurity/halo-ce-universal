# Native GL procedure bridge

`tools/macos_gl_bridge.py` reads the desktop `GL_FUNCTIONS` list and the SDL3
OpenGL prototypes. It emits deterministic guest procedure tokens starting at
`0xFE100000` with a `0x10` stride, typed wrappers named `sub_<token>`, and the
name/token/wrapper lookup functions. Host pointers never enter guest memory.

Each generated wrapper decodes 32-bit i686 cdecl stack slots using
`mac_guest_stack_arg32`, preserves float bit patterns, reconstructs guest
`GLdouble`, widens guest 32-bit `GLsizeiptr`/`GLintptr`, writes scalar returns
to `g_eax`, and advances `g_esp` by the cdecl return-address pop. The host must
implement `mac_macos_gl_native_proc()` with the current SDL GL context's proc
resolver and register the generated wrapper lookup with `recomp_lookup_manual`.

`gl_bridge_support.c` resolves guest pointer spans explicitly. Image transfers
account for GL pixel format/type and pack/unpack alignment, row/image length,
and skip state. Buffer and index pointers are treated as offsets only when the
corresponding GL buffer is bound. Shader source vectors are translated from
guest 32-bit pointers into host pointer vectors. Unknown pointer signatures
fail closed. `glGetString` copies each requested string into a persistent guest
allocation provided by `mac_guest_allocate`; it never narrows a host pointer or
reserves a guessed guest VA. `glMapBufferRange` pointer results fail unless a
guest mapping exists.

Example generation and checks on a host with SDL3 headers:

```sh
python3 tools/macos_gl_bridge.py \
  --gl-header port/linux/src/gl.h \
  --sdl-header "$SDL3_INCLUDE_DIR/SDL3/SDL_opengl.h" \
  --sdl-header "$SDL3_INCLUDE_DIR/SDL3/SDL_opengl_glext.h" \
  --output /tmp/macos-gl-tokens.json \
  --c-output /tmp/macos_gl_token_wrappers.c
```

The generated C belongs only in the native AOT target, which supplies its
generated `recomp_types.h`; it is not part of the ordinary 32-bit source build.
