#!/usr/bin/env python3
"""Generate typed guest-to-Darwin bridges; translate pointers, never GL offsets."""
import re
from pathlib import Path
from guest_gl_stubs import gles_functions, prototypes, split_parameter, FLOAT_TYPES, WIDE_TYPES

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/ios/host'
OUT.mkdir(parents=True,exist_ok=True)
lines=['/* Generated typed bridges. */','#include "ios_host.h"','#include "guest_host.h"','#include "posix.h"',
       '#include <SDL3/SDL.h>','#include <GLES3/gl32.h>','#include <GLES2/gl2ext.h>','#include <string.h>']
table=[]

def declarations(path,pattern):
    text=re.sub(r'/\*.*?\*/','',path.read_text(),flags=re.S)
    text=re.sub(r'__attribute__\s*\(\(.*?\)\)','',text)
    return re.findall(r'^([\w \t*]+?)\b('+pattern+r')\s*\(([^;]*?)\)\s*;',text,re.M|re.S)

for header,pattern in [(ROOT/'port/runtime/guest/runtime/guest_host.h',r'host_\w+'),(ROOT/'port/linux/src/posix.h',r'posix_\w+')]:
    for ret,name,params in declarations(header,pattern):
        ret=' '.join(ret.split());params=' '.join(params.split())
        decl=[];args=[]
        for index,param in enumerate([] if params=='void' else params.split(',')):
            kind,_=split_parameter(param);arg=f'a{index}'
            if '*' in kind:
                decl.append(f'uint64_t {arg}')
                opaque=name in ('posix_directory_next','posix_directory_close') and index==0
                args.append(f'({kind})'+(f'(uintptr_t){arg}' if opaque else f'host_pointer({arg})'))
            else:
                decl.append(f'{kind} {arg}');args.append(arg)
        imported='hostposix_'+name[6:] if name.startswith('posix_') else name
        bridge='ios_bridge_'+name
        return_type='uint32_t' if '*' in ret else ret
        lines.append(f'static {return_type} {bridge}({", ".join(decl) or "void"}) {{')
        call=f'{name}({", ".join(args)})'
        if '*' in ret:call=f'(uint32_t)(uintptr_t){call}'
        lines.append(('    ' if ret=='void' else '    return ')+call+';\n}')
        table.append((imported,bridge))

gl=ROOT/'build/ios/gl_include'
protos=prototypes(str(gl/'GLES3/gl32.h'),str(gl/'GLES2/gl2ext.h'))
for name in gles_functions(str(ROOT/'port/linux/src/gl.h')):
    if name=='glGetString':continue
    ret,params=protos[name]
    plist=[] if params=='void' else [split_parameter(p) for p in params.split(',')]
    decl=[];args=[];integer_index=0
    for index,(kind,_) in enumerate(plist):
        arg=f'a{index}';pointer='*' in kind;base=kind.replace('const','').strip()
        if base in FLOAT_TYPES and not pointer:
            decl.append(f'{kind} {arg}');args.append(arg);continue
        on_stack=integer_index>=8;integer_index+=1
        if pointer:
            decl.append(f'uint64_t {arg}')
            offset=name in ('glVertexAttribPointer','glVertexAttribIPointer','glDrawElements','glDrawElementsBaseVertex') and index==len(plist)-1
            # BaseVertex's final parameter is an integer; its indices are #3.
            offset=offset or (name=='glDrawElementsBaseVertex' and index==3)
            args.append(f'({kind})'+(f'(uintptr_t){arg}' if offset else f'host_pointer({arg})'))
        elif base in WIDE_TYPES or on_stack:
            decl.append(f'long long {arg}');args.append(f'({kind}){arg}')
        else:decl.append(f'{kind} {arg}');args.append(arg)
    bridge='ios_bridge_'+name
    lines.append(f'static {ret} {bridge}({", ".join(decl) or "void"}) {{')
    signature=', '.join(t for t,_ in plist) or 'void'
    lines.append(f'    typedef {ret} (GL_APIENTRY *Function)({signature});')
    lines.append(f'    static Function function; if(!function) function=(Function)SDL_GL_GetProcAddress("{name}");')
    lines.append(f'    if(!function) host_fatal("OpenGL ES entry point unavailable: {name}");')
    if name=='glBindFramebuffer':
        lines.append('    if(!a1) a1=host_ios_default_framebuffer();')
    if name=='glShaderSource':
        lines.extend(['    const uint64_t *raw=host_pointer(a2); const GLchar *strings[16];',
                      '    if(a1<0 || a1>16) host_fatal("invalid shader string count");',
                      '    for(int i=0;i<a1;i++) strings[i]=host_pointer(raw[i]);'])
        args[2]='strings'
    lines.append(('    ' if ret=='void' else '    return ')+f'function({", ".join(args)});\n'+'}')
    table.append(('hostgl_'+name,bridge))
lines.extend(['void *host_resolve_import(const char *name) {',
              '    static const struct {const char *name; void *function;} table[]={'])
lines.extend(f'        {{"{name}",(void *){fn}}},' for name,fn in table)
lines.extend(['    };','    for(unsigned i=0;i<sizeof(table)/sizeof(table[0]);i++) if(!strcmp(name,table[i].name))return table[i].function;',
              '    return NULL;','}'])
(OUT/'bridges.c').write_text('\n'.join(lines)+'\n')
# Use Linux numbers explicitly: Darwin SYS_* constants have different values.
(OUT/'guest_syscall_numbers.h').write_text((ROOT/'port/runtime/guest/libc/arch/arm64_32/bits/syscall.h.in').read_text())
