/*
XISO.C

Copies the maps folder out of an Xbox disc image (an "xiso"), for the
native iOS app's first start without game data.

The image's file system is XDVDFS, read as extract-xiso does
(https://github.com/XboxDev/extract-xiso, extract-xiso.c, whose format
handling this follows; its license is below and in
port/third_party/extract-xiso/LICENSE.TXT): 2048-byte sectors; a
volume descriptor at 0x10000 that starts and ends with
"MICROSOFT*XBOX*MEDIA" and gives the root directory's sector and size; and
directories whose entries form a binary tree (each entry: left and right
subtree offsets in 4-byte units, the start sector, the size, attributes, the
name's length and the name, 4-byte aligned). Images made from a whole disc
put the game partition further in, at one of the offsets below.

The native importer validates the complete map set, then writes to a private
staging directory. A failed or cancelled extraction never publishes maps.

Some parts of this code are copyright in@fishtank.com. This product
includes software developed by in <in@fishtank.com>.

 * Copyright (c) 2003 in <in@fishtank.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *    This product includes software developed by in <in@fishtank.com>.
 *
 * 4. Neither the name of "in" nor the email address "in@fishtank.com"
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED `AS IS' AND ANY EXPRESS OR IMPLIED WARRANTIES
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * AUTHOR OR ANY CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "xiso.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

enum { SECTOR = 2048, MAX_DIRECTORY = 4 << 20, MAX_FILES = 256, COPY_SIZE = 1 << 20 };
static const char magic[] = "MICROSOFT*XBOX*MEDIA";
static const uint64_t partitions[] = {0, 0x0FD90000, 0x02080000, 0x18300000};
static const char *required[] = {"ui.map", "a10.map", "a30.map", "a50.map", "b30.map", "b40.map",
    "c10.map", "c20.map", "c40.map", "d20.map", "d40.map"};
struct entry { char name[256]; uint32_t sector, size; };
struct image { int fd; uint64_t length, partition; char *error; size_t error_size; };

static int fail(struct image *image, const char *format, ...) {
    va_list args; va_start(args, format);
    if (image->error && image->error_size) vsnprintf(image->error, image->error_size, format, args);
    va_end(args); return 0;
}
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static int read_at(struct image *image, uint64_t offset, void *out, size_t size) {
    if (offset > image->length || size > image->length-offset) return 0;
    unsigned char *p=out;
    while (size) {
        ssize_t count=pread(image->fd,p,size,(off_t)offset);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) return 0;
        p+=count; offset+=(uint64_t)count; size-=(size_t)count;
    }
    return 1;
}
static int cache_header(struct image *image, const unsigned char *header, const char *name, char build[33]) {
    if (memcmp(header,"daeh",4) || memcmp(header+2044,"toof",4) || u32(header+4)!=5)
        return fail(image,"%s is not an original Xbox Halo map. PC, Custom Edition, and MCC images are unsupported.",name);
    memcpy(build,header+64,32); build[32]=0;
    if (strcmp(build,"01.01.14.2342") && strcmp(build,"01.10.12.2276"))
        return fail(image,"Unsupported Xbox map build in %s. Use Halo: Combat Evolved PAL or NTSC-US.",name);
    return 1;
}
int xiso_maps_ready(const char *maps, char *error, size_t error_size) {
    struct image image={.error=error,.error_size=error_size};
    char path[2048], expected[33]={0};
    for (size_t i=0;i<sizeof(required)/sizeof(*required);i++) {
        if (snprintf(path,sizeof(path),"%s/%s",maps,required[i]) >= (int)sizeof(path))
            return fail(&image,"Game data path is too long.");
        image.fd=open(path,O_RDONLY|O_NOFOLLOW);
        struct stat st; unsigned char header[2048]; char build[33];
        int ok=image.fd>=0 && !fstat(image.fd,&st) && S_ISREG(st.st_mode) && st.st_size>=2048;
        if (ok) {image.length=(uint64_t)st.st_size;ok=read_at(&image,0,header,sizeof(header));}
        if (image.fd>=0) close(image.fd);
        if (!ok) return fail(&image,"Game data is missing or incomplete. Choose your Halo XISO to import it.");
        if (!cache_header(&image,header,required[i],build)) return 0;
        if (i && strcmp(build,expected)) return fail(&image,"Game maps contain mixed Xbox releases. Import one complete disc image.");
        strcpy(expected,build);
    }
    return 1;
}
struct walk { struct image *image; const unsigned char *table; unsigned char *visited;
    uint32_t size; struct entry *files; int count, directories; };
static int walk_tree(struct walk *walk, uint32_t offset, unsigned depth) {
    if (depth>64 || offset+14>walk->size || (offset&3) || walk->visited[offset/4])
        return fail(walk->image,"The disc image has a damaged directory tree.");
    walk->visited[offset/4]=1;
    const unsigned char *p=walk->table+offset;
    unsigned left=p[0]|p[1]<<8, right=p[2]|p[3]<<8, count=p[13];
    if (left==0xffff) return 1;
    if (!count || offset+14+count>walk->size) return fail(walk->image,"The disc image has a damaged filename.");
    if (left && !walk_tree(walk,left*4,depth+1)) return 0;
    if (!!(p[12]&16)==walk->directories) {
        char name[256];
        for (unsigned i=0;i<count;i++) {
            unsigned char c=p[14+i];
            if (c<32 || c>126 || c=='/' || c=='\\' || c==':')
                return fail(walk->image,"The disc image contains an unsafe filename.");
            name[i]=(char)tolower(c);
        }
        name[count]=0;
        if (!strcmp(name,".") || !strcmp(name,"..")) return fail(walk->image,"The disc image contains an unsafe filename.");
        if (walk->directories || (count>4 && !strcmp(name+count-4,".map"))) {
            if (walk->count>=MAX_FILES) return fail(walk->image,"The disc image has too many map files.");
            for (int i=0;i<walk->count;i++) if (!strcmp(walk->files[i].name,name))
                return fail(walk->image,"The disc image contains duplicate filenames.");
            struct entry *entry=&walk->files[walk->count++];
            strcpy(entry->name,name); entry->sector=u32(p+4); entry->size=u32(p+8);
            uint64_t start=walk->image->partition+(uint64_t)entry->sector*SECTOR;
            if (start>walk->image->length || entry->size>walk->image->length-start)
                return fail(walk->image,"The disc image is incomplete or truncated.");
        }
    }
    return !right || walk_tree(walk,right*4,depth+1);
}
static int directory(struct image *image, uint32_t sector, uint32_t size, int directories,
    struct entry *files, int *count) {
    if (!size || size>MAX_DIRECTORY) return fail(image,"The disc image has a damaged directory size.");
    unsigned char *table=malloc(size), *visited=calloc((size+3)/4,1);
    int ok=0;
    if (!table || !visited) {fail(image,"Not enough memory to inspect the disc image.");goto done;}
    if (!read_at(image,image->partition+(uint64_t)sector*SECTOR,table,size)) {
        fail(image,"Could not read the disc image directory. The image may be incomplete.");goto done;
    }
    struct walk walk={image,table,visited,size,files,0,directories};
    ok=walk_tree(&walk,0,0); *count=walk.count;
done: free(table);free(visited);return ok;
}
int xiso_extract_maps(const char *image_path, const char *destination,
    xiso_progress_proc progress, void *context, char *error, size_t error_size) {
    struct image image={.fd=-1,.error=error,.error_size=error_size};
    struct entry *files=calloc(MAX_FILES,sizeof(*files));
    unsigned char *buffer=NULL, header[2048];
    char partial[2048], final[2048], path[2400], expected[33]={0};
    struct stat st; int ok=0,count=0,found=0;
    uint32_t root_sector=0,root_size=0; uint64_t total=0,done=0;
    if (!files) {fail(&image,"Not enough memory to import the disc image.");goto cleanup;}
    image.fd=open(image_path,O_RDONLY);
    if (image.fd<0 || fstat(image.fd,&st) || !S_ISREG(st.st_mode)) {
        fail(&image,"Could not open the disc image. Download it in Files first, then try again.");goto cleanup;
    }
    image.length=(uint64_t)st.st_size;
    for (size_t i=0;i<sizeof(partitions)/sizeof(*partitions);i++) {
        if (!read_at(&image,partitions[i]+0x10000,header,sizeof(header))) continue;
        if (!memcmp(header,magic,20) && !memcmp(header+0x7ec,magic,20)) {
            image.partition=partitions[i];root_sector=u32(header+20);root_size=u32(header+24);found=1;break;
        }
    }
    if (!found) {fail(&image,"This is not an Xbox XISO. Choose an uncompressed Halo: Combat Evolved .iso or .xiso file.");goto cleanup;}
    if (!directory(&image,root_sector,root_size,1,files,&count)) goto cleanup;
    found=0;
    for (int i=0;i<count;i++) if (!strcmp(files[i].name,"maps")) {root_sector=files[i].sector;root_size=files[i].size;found=1;break;}
    if (!found) {fail(&image,"This disc image has no Halo maps folder.");goto cleanup;}
    if (!directory(&image,root_sector,root_size,0,files,&count)) goto cleanup;
    for (size_t r=0;r<sizeof(required)/sizeof(*required);r++) {
        found=0;for (int i=0;i<count;i++) if (!strcmp(files[i].name,required[r])) found=1;
        if (!found) {fail(&image,"This is not a complete Halo: Combat Evolved disc: %s is missing.",required[r]);goto cleanup;}
    }
    for (int i=0;i<count;i++) {
        char build[33];
        if (files[i].size<2048 || !read_at(&image,image.partition+(uint64_t)files[i].sector*SECTOR,header,sizeof(header))) {
            fail(&image,"The disc image contains an incomplete map: %s.",files[i].name);goto cleanup;
        }
        if (!cache_header(&image,header,files[i].name,build)) goto cleanup;
        if (i && strcmp(expected,build)) {fail(&image,"The disc contains mixed map releases. Use one original Xbox disc image.");goto cleanup;}
        strcpy(expected,build);total+=files[i].size;
    }
    struct statvfs space;
    if (!statvfs(destination,&space) && total>(uint64_t)space.f_bavail*space.f_frsize) {
        fail(&image,"Not enough storage. Free at least %.2f GB for the extracted game maps.",(double)total/1e9);goto cleanup;
    }
    if (snprintf(partial,sizeof(partial),"%s/maps.partial",destination)>=(int)sizeof(partial) ||
        snprintf(final,sizeof(final),"%s/maps",destination)>=(int)sizeof(final)) {fail(&image,"Import path is too long.");goto cleanup;}
    if (!lstat(final,&st) || errno!=ENOENT) {fail(&image,"The import destination already contains maps.");goto cleanup;}
    if (mkdir(partial,0700)) {fail(&image,"Could not create the import folder.");goto cleanup;}
    buffer=malloc(COPY_SIZE);
    if (!buffer) {fail(&image,"Not enough memory to copy the game maps.");goto cleanup;}
    for (int i=0;i<count;i++) {
        if (progress && !progress(context,files[i].name,done,total)) {fail(&image,"Import cancelled.");goto cleanup;}
        snprintf(path,sizeof(path),"%s/%s",partial,files[i].name);
        int output=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        if (output<0) {fail(&image,"Could not write %s. Check available storage.",files[i].name);goto cleanup;}
        uint64_t start=image.partition+(uint64_t)files[i].sector*SECTOR, remaining=files[i].size;
        int copied=1;
        while (remaining && copied) {
            size_t chunk=remaining>COPY_SIZE?COPY_SIZE:(size_t)remaining;
            if (!read_at(&image,start,buffer,chunk)) {fail(&image,"Could not read %s. The image may be incomplete.",files[i].name);copied=0;break;}
            size_t written=0;
            while (written<chunk) {
                ssize_t n=write(output,buffer+written,chunk-written);
                if (n<0 && errno==EINTR) continue;
                if (n<=0) {fail(&image,"Could not write %s. Check available storage.",files[i].name);copied=0;break;}
                written+=(size_t)n;
            }
            start+=chunk;remaining-=chunk;done+=chunk;
            if (copied && progress && !progress(context,files[i].name,done,total)) {fail(&image,"Import cancelled.");copied=0;}
        }
        if (copied && fsync(output)) {fail(&image,"Could not save %s. Check available storage.",files[i].name);copied=0;}
        if (close(output) && copied) {fail(&image,"Could not finish saving %s.",files[i].name);copied=0;}
        if (!copied) goto cleanup;
    }
    if (rename(partial,final)) {fail(&image,"Could not finish the game import.");goto cleanup;}
    ok=1;
cleanup:
    if (image.fd>=0) close(image.fd);
    free(buffer);free(files);return ok;
}
