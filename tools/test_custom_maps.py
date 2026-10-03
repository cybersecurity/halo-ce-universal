"""Compile production Xbox-v5 discovery and menu adapters with synthetic maps.

Only platform file I/O, unrelated engine services and authored tag lookup are
stubbed. AddressSanitizer covers the fixed-size save and dynamic caption paths.
No game data, application launch, or optional CE conversion is required.
"""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STOCK = ("beavercreek", "sidewinder", "damnation", "ratrace", "prisoner", "hangemhigh",
         "chillout", "carousel", "boardingaction", "bloodgulch", "wizard", "putput", "longest")


def c_block(source, start):
    opening = source.index("{", start)
    depth = 0
    tokens = re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
                         source[opening:], re.S)
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:opening + token.end()]
    raise ValueError("Unclosed C block")


def function(source, name):
    match = re.search(r"(?m)^(?:static\s+)?[\w *]+\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", source)
    if not match:
        raise ValueError(f"Production definition not found: {name}")
    return c_block(source, match.start())


def header(name="downrush", version=5, kind=1, length=8192, build="01.10.12.2276",
           tag_offset=2048, tag_size=0x24):
    result = bytearray(2048)
    result[:4] = b"daeh"
    struct.pack_into("<II", result, 4, version, length)
    struct.pack_into("<II", result, 16, tag_offset, tag_size)
    result[32:32 + len(name)] = name.encode("ascii")
    result[64:64 + len(build)] = build.encode("ascii")
    struct.pack_into("<H", result, 96, kind)
    result[-4:] = b"toof"
    return result


CSERIES = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef unsigned char byte;
typedef unsigned char boolean;
typedef unsigned short word;
typedef float real;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define NUMBEROF(a) (sizeof(a)/sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define PIN(a,b,c) MIN(MAX((a),(b)),(c))
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define csstrlen strlen
#define csstrncmp strncmp
#define csstrncpy strncpy
#define csmemcpy memcpy
#define match_assert(file,line,test) assert(test)
#define match_vassert(file,line,test,message) assert(test)
'''

XTL = r'''
#pragma once
typedef void *HANDLE;
typedef struct { unsigned long dwFileAttributes; char cFileName[260]; } WIN32_FIND_DATAA;
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define GENERIC_READ 1
#define OPEN_EXISTING 1
#define FILE_ATTRIBUTE_DIRECTORY 16
HANDLE CreateFileA(const char *, int, int, void *, int, int, void *);
int ReadFile(HANDLE, void *, unsigned long, unsigned long *, void *);
int CloseHandle(HANDLE);
HANDLE FindFirstFileA(const char *, WIN32_FIND_DATAA *);
int FindNextFileA(HANDLE, WIN32_FIND_DATAA *);
'''

FILES = r'''
#include <dirent.h>
#include <sys/stat.h>
static char directory[512];
static int handles;
const char *cache_files_map_directory(void) {return directory;}
struct handle {int kind; FILE *file; DIR *dir;};
HANDLE CreateFileA(const char *name,int access,int share,void *security,int creation,int flags,void *template) {
    struct handle *h=calloc(1,sizeof(*h)); h->kind=1; h->file=fopen(name,"rb");
    if (!h->file) {free(h); return INVALID_HANDLE_VALUE;} handles++; return h;
}
int ReadFile(HANDLE handle,void *data,unsigned long size,unsigned long *read,void *overlapped) {
    struct handle *h=handle; *read=fread(data,1,size,h->file); return !ferror(h->file);
}
int CloseHandle(HANDLE handle) {
    struct handle *h=handle; if(h->kind==1) fclose(h->file); else closedir(h->dir);
    free(h); handles--; return 1;
}
int FindNextFileA(HANDLE handle,WIN32_FIND_DATAA *out) {
    struct handle *h=handle; struct dirent *entry;
    while((entry=readdir(h->dir))) {
        size_t length=strlen(entry->d_name);
        if(length<4 || strcasecmp(entry->d_name+length-4,".map")) continue;
        char path[1024]; struct stat info; snprintf(path,sizeof(path),"%s%s",directory,entry->d_name);
        if(stat(path,&info)) continue;
        out->dwFileAttributes=S_ISDIR(info.st_mode)?FILE_ATTRIBUTE_DIRECTORY:0;
        snprintf(out->cFileName,sizeof(out->cFileName),"%s",entry->d_name); return 1;
    } return 0;
}
HANDLE FindFirstFileA(const char *pattern,WIN32_FIND_DATAA *out) {
    struct handle *h=calloc(1,sizeof(*h)); h->dir=opendir(directory);
    if(!h->dir || !FindNextFileA(h,out)) {if(h->dir) closedir(h->dir); free(h); return INVALID_HANDLE_VALUE;}
    handles++; return h;
}
static void set_directory(const char *path) {snprintf(directory,sizeof(directory),"%s/",path);}
static char *stock[]={
    "levels\\test\\beavercreek\\beavercreek", "levels\\test\\sidewinder\\sidewinder",
    "levels\\test\\damnation\\damnation", "levels\\test\\ratrace\\ratrace",
    "levels\\test\\prisoner\\prisoner", "levels\\test\\hangemhigh\\hangemhigh",
    "levels\\test\\chillout\\chillout", "levels\\test\\carousel\\carousel",
    "levels\\test\\boardingaction\\boardingaction", "levels\\test\\bloodgulch\\bloodgulch",
    "levels\\test\\wizard\\wizard", "levels\\test\\putput\\putput", "levels\\test\\longest\\longest"
};
'''

SCANNER_MAIN = r'''
int main(int argc,char **argv) {
    assert(argc>=3);
    if(!strcmp(argv[1],"header")) {
        assert(argc==4); unsigned char data[2048]; FILE *file=fopen(argv[2],"rb"); assert(file);
        assert(fread(data,1,sizeof(data),file)==sizeof(data)); fclose(file);
        printf("%d\n",native_map_header_valid(data,argv[3])); return 0;
    }
    set_directory(argv[2]); short count=123;
    if(!strcmp(argv[1],"invalid_stock")) {
        native_multiplayer_map_list(NULL,13,&count); assert(count==0);
        native_multiplayer_map_list(stock,-1,&count); assert(count==0);
        native_multiplayer_map_list(stock,129,&count); assert(count==0);
    }
    char **names=native_multiplayer_map_list(stock,13,&count);
    assert(count>=13 && count<=HALO_CUSTOM_MAP_LIMIT && handles==0);
    for(short i=0;i<13;i++) assert(names[i]==stock[i]);
    for(short i=13;i<count;i++) {
        assert(strlen(names[i])<32 && native_map_is_custom(names[i]));
        if(i>13) assert(strcasecmp(names[i-1],names[i])<0);
    }
    if(!strcmp(argv[1],"immutable")) {
        assert(argc==4); set_directory(argv[3]); short repeated=0;
        assert(native_multiplayer_map_list(stock,13,&repeated)==names && repeated==count && handles==0);
    }
    char label[40]; memset(label,'x',sizeof(label));
    native_map_display_name("levels\\test\\h1pb_prisoner",label,5);
    assert(!strcmp(label,"H1pb") && label[5]=='x');
    native_map_display_name(NULL,label,sizeof(label)); assert(!label[0]);
    assert(!native_map_is_custom(NULL) && !native_map_is_custom("LEVELS\\TEST\\PRISONER\\PRISONER"));
    assert(native_map_is_custom("levels\\test\\h1pb_prisoner\\h1pb_prisoner"));
    for(short i=0;i<count;i++) puts(names[i]);
    return 0;
}
'''

MENU_STUBS = r'''
#define ROW_TEXT_LENGTH 64
#define MAP_ROWS 11
#define MAXIMUM_ROWS 32
#define _ui_widget_type_bitmap 0
#define _ui_widget_type_text_box 1
#define _ui_widget_type_spinner_list 2
#define selected_list_item_index selected_index
static struct {short map_first,map_chosen;} multiplayer;
static char const *const *multiplayer_map_names;
static short multiplayer_map_count;
static struct {char *multiplayer_levels[13];} event_handler_functions;
struct event_record {short type,controller_index;};
struct ui_widget_definition {short type,child_count; struct {long index;} text_label_string_list; short string_list_index;};
static struct ui_widget_definition definitions[8];
static struct ui_widget_definition *ui_widget_definition_get(long index) {assert(index>=0 && index<8); return &definitions[index];}
static int fail_allocation;
static void *ui_widget_realloc(void *p,size_t size,const char *file,long line) {return fail_allocation?NULL:realloc(p,size);}
static wchar_t *ustrncpy(wchar_t *dest,const wchar_t *src,size_t size) {
    size_t i=0; for(;i<size && src[i];i++) dest[i]=src[i]; for(;i<size;i++) dest[i]=0; return dest;
}
static unsigned long ustrlen(const wchar_t *text) {unsigned long n=0;while(text[n])n++;return n;}
static int wide_equals(const wchar_t *a,const wchar_t *b) {while(*a && *a==*b){a++;b++;}return *a==*b;}
static wchar_t const *const stock_text[]={L"Battle Creek",L"Sidewinder",L"Damnation",L"Rat Race",L"Prisoner",L"Hang 'Em High",L"Chill Out",L"Derelict",L"Boarding Action",L"Blood Gulch",L"Wizard",L"Chiron TL34",L"Longest"};
static int stock_lookups;
static long tag_loaded(long group,const char *name) {return 1;}
static wchar_t *unicode_string_list_get_string(long tag,short index) {
    assert(index>=0 && index<13); stock_lookups++; return (wchar_t *)stock_text[index];
}
static void profile_name_show(struct widget_instance *widget) {}
static char last_used[256],main_map[256],stage_map[256],server_map[256],saved_copy[256];
static int server_present,local_game,region_supported=1,unavailable,save_count;
static boolean saved_game_file_retrieve_last_used_multiplayer_map(char *out) {memcpy(out,last_used,256);return last_used[0]!=0;}
static void saved_game_file_remember_last_used_multiplayer_map(const char *name) {
    /* The real API reads the entire field, even for short registry strings. */
    memcpy(saved_copy,name,sizeof(saved_copy)); save_count++;
    for(size_t i=strlen(saved_copy)+1;i<sizeof(saved_copy);i++) assert(saved_copy[i]==0);
}
static void *global_network_game_server_get(void) {return server_present?(void *)1:NULL;}
static boolean network_game_is_splitscreen_local(void) {return local_game;}
static boolean cache_files_map_plays_multiplayer(const char *name,char *build) {strcpy(build,"test");return region_supported;}
static void cache_files_show_multiplayer_unavailable(const char *name,const char *build) {unavailable++;}
static void main_set_multiplayer_map_name(const char *name) {snprintf(main_map,sizeof(main_map),"%s",name);}
static void game_engine_override_map_name(const char *name) {snprintf(stage_map,sizeof(stage_map),"%s",name);}
static void network_game_server_change_map_name(void *server,const char *name) {snprintf(server_map,sizeof(server_map),"%s",name);}
struct advertised_game {char map_name[128];};
struct network_game {struct {char name[128];} map;};
static struct network_game network_game;
static struct network_game *network_game_get_game(void) {return &network_game;}
static void error(int level,const char *message) {assert(0);}
static struct widget_instance *widget_instance_get_nth_child(struct widget_instance *widget,long n) {
    struct widget_instance *child=widget->child; while(n-- && child)child=child->next;return child;
}
static long displayed[3];
static void spinner_list_3wide_determine_displayed_item_indices(struct widget_instance *widget,long *out) {memcpy(out,displayed,sizeof(displayed));}
static struct widget_instance *ui_widget_port_error_text_box;
static const wchar_t *ui_widget_port_error_text;
#define SPINNER_EXTRA_DESCRIPTION_BASE 20000
static const wchar_t *kills_to_win_extra_descriptions[]={L"extra"};
static void *widget_memory_pool;
static void *pool_resize_pointer(void *pool,void *pointer,size_t size,const char *file,long line) {return realloc(pointer,size);}
'''

MENU_MAIN = r'''
static void free_text(struct widget_instance *widget) {free(widget->parameters.text_box.text);widget->parameters.text_box.text=NULL;}
int main(int argc,char **argv) {
    assert(argc==2); set_directory(argv[1]); memcpy(event_handler_functions.multiplayer_levels,stock,sizeof(stock));
    char const *const *names;short last; short count=ui_widget_port_multiplayer_maps(&names,&last);
    assert(count==16 && last==0 && !strcmp(names[13],"atlas") && !strcmp(names[14],"downrush") && !strcmp(names[15],"h1pb_prisoner"));
    strcpy(last_used,"levels\\test\\downrush\\downrush");
    assert(ui_widget_port_multiplayer_maps(&names,&last)==16 && last==14);
    server_present=1;
    assert(!ui_widget_port_multiplayer_map_choose(-1) && !ui_widget_port_multiplayer_map_choose(16) && save_count==0);
    assert(ui_widget_port_multiplayer_map_choose(15));
    assert(!strcmp(main_map,"h1pb_prisoner") && !strcmp(stage_map,main_map) && !strcmp(server_map,main_map) && !strcmp(saved_copy,main_map));
    region_supported=0; assert(!ui_widget_port_multiplayer_map_choose(14) && unavailable==1 && save_count==1);
    local_game=1; assert(ui_widget_port_multiplayer_map_choose(14));local_game=0;region_supported=1;
    assert(ui_widget_port_multiplayer_map_choose(4) && !strcmp(saved_copy,stock[4]));

    /* The Xbox selector uses its generated list, including the same custom IDs. */
    struct event_widget wrapper={0},screen={0},selector={0}; boolean deleted=0;
    wrapper.definition_tag_index=0;wrapper.child=&screen;screen.definition_tag_index=1;screen.child=&selector;
    selector.definition_tag_index=2;definitions[0].child_count=1;definitions[1].child_count=3;
    definitions[2].type=2;definitions[2].child_count=3;
    assert(multiplayer_level_list_initialize(&selector,NULL,&deleted));
    assert(selector.generated_count==16 && selector.data3C.selected_index==14);
    selector.data3C.selected_index=15;assert(multiplayer_level_select(&wrapper,NULL,&deleted));
    assert(!strcmp(saved_copy,"h1pb_prisoner") && !strcmp(main_map,saved_copy));

    /* Actual PC scrolling and row update, including the end of the list. */
    struct widget_instance list={0},rows[12]={0},row_text[11]={0},description={0},preview[3]={0};
    char row_names[12][24];
    for(int i=0;i<12;i++) {
        snprintf(row_names[i],sizeof(row_names[i]),i<11?"list_item_%d":"buttons",i);
        rows[i].name=row_names[i];rows[i].next=i<11?&rows[i+1]:NULL;
        if(i<11){rows[i].child=&row_text[i];row_text[i].name="list_item_text";}
    }
    preview[0].name="mp_map_right_name";preview[1].name="mp_map_right_pic";preview[2].name="mp_map_right_data";
    preview[0].next=&preview[1];preview[1].next=&preview[2];description.child=preview;
    list.child=rows;list.parameters.list.extended_description=&description;
    assert(map_list_initialize(&list) && multiplayer.map_chosen==14 && multiplayer.map_first==5);
    map_list_update(&list); assert(wide_equals(preview[0].parameters.text_box.text,L"Downrush"));
    assert(preview[1].animation.current_frame_index==19 && preview[0].parameters.text_box.string_list_index==HALO_CUSTOM_MAP_TEXT);
    assert(wide_equals(preview[2].parameters.text_box.text,L"Community map"));
    list.focused_child=&rows[10];map_list_update(&list);assert(multiplayer.map_chosen==15);
    assert(wide_equals(row_text[10].parameters.text_box.text,L"H1pb Prisoner"));
    list.focused_child=&rows[11];map_list_update(&list);assert(multiplayer.map_chosen==15);
    multiplayer.map_first=0;focus_row(&list,4);map_list_update(&list);
    assert(multiplayer.map_chosen==4 && preview[0].parameters.text_box.string_list_index==4 && preview[1].animation.current_frame_index==4);
    render_caption(&preview[0],&definitions[3]);assert(wide_equals(preview[0].parameters.text_box.text,L"Prisoner"));
    /* Stock rendering allocated only18 bytes: custom caption must resize it. */
    multiplayer.map_first=5;focus_row(&list,10);map_list_update(&list);
    int lookups=stock_lookups;render_caption(&preview[0],&definitions[3]);
    assert(stock_lookups==lookups && wide_equals(preview[0].parameters.text_box.text,L"H1pb Prisoner"));
    wchar_t caption[64];map_row_text(11,caption);assert(caption[0]==0);

    /* Browser/lobby stock basenames and aliases never share stock indices. */
    struct advertised_game advertised;strcpy(advertised.map_name,"levels\\test\\h1pb_prisoner\\h1pb_prisoner");
    game_map_name(&advertised,caption);assert(wide_equals(caption,L"H1pb Prisoner"));
    strcpy(advertised.map_name,"PRISONER");game_map_name(&advertised,caption);assert(wide_equals(caption,L"Prisoner"));
    preview[0].name="lobby_map_name";preview[1].name="lobby_map_pic";
    lobby_map_show(&description,"levels\\test\\h1pb_prisoner\\h1pb_prisoner");
    assert(preview[1].animation.current_frame_index==19 && preview[0].parameters.text_box.string_list_index==HALO_CUSTOM_MAP_TEXT);
    lobby_map_show(&description,"prisoner");assert(preview[1].animation.current_frame_index==4 && preview[0].parameters.text_box.string_list_index==4);

    /* Xbox three-card captions and lobby; preserve the retail fallback13. */
    struct widget_instance xbox_list={0},cards[3]={0},card_labels[3][3]={0};
    xbox_list.definition_tag_index=2;xbox_list.child=cards;xbox_list.parameters.list.list_items=(void *)names;xbox_list.parameters.list.number_of_items=count;
    definitions[4].child_count=3;definitions[5].type=1;definitions[6].type=0;
    for(int i=0;i<3;i++) {
        cards[i].definition_tag_index=4;cards[i].child=card_labels[i];cards[i].next=i<2?&cards[i+1]:NULL;
        for(int j=0;j<3;j++){card_labels[i][j].definition_tag_index=j==1?6:5;card_labels[i][j].next=j<2?&card_labels[i][j+1]:NULL;}
    }
    displayed[0]=4;displayed[1]=15;displayed[2]=16;
    mp_level_select_list_update_displayed_items(&xbox_list);
    assert(card_labels[0][0].parameters.text_box.string_list_index==4 && card_labels[0][1].animation.current_frame_index==4);
    assert(wide_equals(card_labels[1][0].parameters.text_box.text,L"H1pb Prisoner") && card_labels[1][1].animation.current_frame_index==13);
    assert(wide_equals(card_labels[2][0].parameters.text_box.text,L"Unknown Map"));
    fail_allocation=1;displayed[0]=15;mp_level_select_list_update_displayed_items(&xbox_list);fail_allocation=0;
    assert(card_labels[0][0].parameters.text_box.string_list_index==HALO_CUSTOM_MAP_TEXT);
    struct widget_instance xbox_name={0},xbox_picture={0};xbox_name.type=1;
    strcpy(network_game.map.name,"levels\\test\\h1pb_prisoner\\h1pb_prisoner");
    multiplayer_game_set_text_box_for_map_name(&xbox_name);multiplayer_game_set_bitmap_for_map(&xbox_picture);
    assert(wide_equals(xbox_name.parameters.text_box.text,L"H1pb Prisoner") && xbox_picture.animation.current_frame_index==13);
    strcpy(network_game.map.name,stock[4]);multiplayer_game_set_text_box_for_map_name(&xbox_name);multiplayer_game_set_bitmap_for_map(&xbox_picture);
    assert(xbox_name.parameters.text_box.string_list_index==4 && xbox_picture.animation.current_frame_index==4);
    for(int i=0;i<11;i++) free_text(&row_text[i]);free_text(&preview[0]);free_text(&preview[2]);free_text(&xbox_name);
    for(int i=0;i<3;i++){free_text(&card_labels[i][0]);free_text(&card_labels[i][2]);}
    assert(handles==0);return 0;
}
'''

CACHE_STUBS = r'''
#define CACHE_FILE_HEADER_SIGNATURE 0x68656164UL
#define CACHE_FILE_FOOTER_SIGNATURE 0x666F6F74UL
enum {_scenario_type_solo, _scenario_type_multiplayer, _scenario_type_main_menu};
struct cached_map_file {long last_modification_date;};
static struct cached_map_file cached_maps[6];
static struct {short open_map_file_index;} cache_file_globals={NONE};
static struct cached_map_file *cached_map_file_get(short index) {assert(index>=0 && index<6);return &cached_maps[index];}
static long cached_map_file_get_size(short index) {
    assert(index>=0 && index<6);return index<=1?0x11600000:index==2?0x02300000:HALO_PORT_MULTIPLAYER_CACHE_SIZE;
}
static int CompareFileTime(const long *a,const long *b) {return *a>*b?1:*a<*b?-1:0;}
'''

CACHE_MAIN = r'''
int main(void) {
    assert(sizeof(struct cache_file_header)==2048);
    struct cache_file_header header={0};
    header.header_signature=CACHE_FILE_HEADER_SIGNATURE;header.footer_signature=CACHE_FILE_FOOTER_SIGNATURE;
    header.version=5;strcpy(header.name,"weld");header.reserved60[0]=1;
    header.file_length=60972544;assert(cache_file_header_verify(&header,"weld",FALSE));
    assert(cached_map_files_find_free_map(header.file_length,_scenario_type_multiplayer)>=3);
    header.file_length=HALO_PORT_MULTIPLAYER_CACHE_SIZE;
    assert(cache_file_header_verify(&header,"boundary",FALSE));
    assert(cached_map_files_find_free_map(header.file_length,_scenario_type_multiplayer)>=3);
    header.file_length++;assert(!cache_file_header_verify(&header,"too-large",FALSE));
    header.reserved60[0]=0;assert(cache_file_header_verify(&header,"solo",FALSE));
    assert(cached_map_files_find_free_map(header.file_length,_scenario_type_solo)<2);
    header.reserved60[0]=1;header.file_length=0x11600000;
    assert(!cache_file_header_verify(&header,"direct-precache",FALSE));
    header.file_length=8192;header.version=7;assert(!cache_file_header_verify(&header,"pc",FALSE));
    return 0;
}
'''


def fixture_sources():
    cache = (ROOT / "source/cache/cache_files.c").read_text()
    table = cache[cache.index("static struct\n{\n\tchar const *build;"):cache.index("/* the region of a build")]
    region = ("static struct {struct {char build[32];} header;} cache_file_globals;\n" +
              table + function(cache, "cache_files_build_region"))
    common = '#include "cseries.h"\n#include "xtl.h"\n#include "halo_port_capacity.h"\n' + region + FILES + '\n#include "custom_maps.c"\n'
    menu = (ROOT / "port/linux/game/menu_functions.c").read_text()
    events = (ROOT / "source/interface/ui_widget_event_handler_functions.c").read_text()
    data = (ROOT / "source/interface/ui_widget_game_data_input_functions.c").read_text()
    renderer = (ROOT / "source/interface/ui_widget.c").read_text()
    widget = c_block(menu, menu.index("struct widget_instance\n")) + ";\n"
    event_widget = (c_block(events, events.index("struct widget_instance\n")) + ";\n").replace("widget_instance", "event_widget")
    functions = []
    for name in ("multiplayer_map_remember", "ui_widget_port_multiplayer_maps", "ui_widget_port_multiplayer_map_choose",
                 "multiplayer_level_list_initialize", "multiplayer_level_select"):
        functions.append(function(events, name).replace("widget_instance", "event_widget"))
    for name in ("descendant", "named", "text_set_length", "text_set", "string_get", "focused_row", "focus_row",
                 "rows_update", "list_scroll", "map_name_text", "map_caption_set", "map_row_text",
                 "map_list_initialize", "map_list_update", "game_map_name", "lobby_map_show"):
        functions.append(function(menu, name))
    for name in ("custom_map_text", "mp_level_select_list_update_displayed_items",
                 "multiplayer_game_set_text_box_for_map_name", "multiplayer_game_set_bitmap_for_map"):
        functions.append(function(data, name))
    # Compile the exact renderer's authored-string loading path, before drawing.
    render = function(renderer, "widget_instance_render_text_box")
    begin = render.index("\tif (definition->text_label_string_list.index")
    end = render.index("\ttext = &widget->parameters.text_box.text;", begin)
    functions.append("static void render_caption(struct widget_instance *widget, struct ui_widget_definition *definition)\n{\n" + render[begin:end] + "}\n")
    cache_header = c_block(cache, cache.index("struct cache_file_header\n")) + ";\n"
    cache_header = re.sub(r"\blong\b", "int32_t", cache_header.replace("unsigned long", "uint32_t"))
    slots = (ROOT / "source/cache/cache_files_windows.c").read_text()
    cache_fixture = ('#include "cseries.h"\n#include "halo_port_capacity.h"\n' + cache_header + CACHE_STUBS +
                     function(cache, "cache_file_header_verify") + function(slots, "cached_map_files_find_free_map") + CACHE_MAIN)
    return {"scanner": common + SCANNER_MAIN,
            "menus": common + widget + event_widget + MENU_STUBS + "\n\n".join(functions) + MENU_MAIN,
            "cache": cache_fixture}


class NativeCustomMaps(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang")
        if not compiler:
            raise unittest.SkipTest("clang is required for production C fixtures")
        cls.temp = tempfile.TemporaryDirectory(prefix="halo-v5-community-")
        cls.folder = Path(cls.temp.name)
        (cls.folder / "cache").mkdir()
        (cls.folder / "cseries.h").write_text("#pragma once\n" + CSERIES)
        (cls.folder / "xtl.h").write_text(XTL)
        (cls.folder / "cache/cache_files.h").write_text("char const *cache_files_map_directory(void);\nchar const *cache_files_build_region(char const *);\n")
        cls.binaries = {}
        for name, source in fixture_sources().items():
            path = cls.folder / f"{name}.c"
            path.write_text(source)
            binary = cls.folder / f"test-{name}"
            result = subprocess.run([compiler, "-std=gnu11", "-fshort-wchar", "-Wall", "-Wextra", "-Werror",
                                     "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-multichar", "-Wno-sign-compare",
                                     "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DHALO_PORT_MAXIMUM_NETWORK_PLAYERS=128",
                                     "-I", str(cls.folder), "-iquote", str(ROOT / "port/linux/include"),
                                     "-I", str(ROOT / "port/linux/game"), str(path), "-o", str(binary)], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stderr)
            cls.binaries[name] = binary

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_fixture(self, fixture, *args):
        result = subprocess.run([str(self.binaries[fixture]), *map(str, args)], cwd=self.folder,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.splitlines()

    def valid(self, data, filename="downrush.map"):
        path = self.folder / "header.bin"
        path.write_bytes(data)
        return self.run_fixture("scanner", "header", path, filename) == ["1"]

    def test_header_versions_regions_bounds_and_names(self):
        for build in ("01.10.12.2276", "01.01.14.2342", "01.08.15.1749"):
            self.assertTrue(self.valid(header(build=build)))
        self.assertTrue(self.valid(header(), "DOWNRUSH.MAP"))
        self.assertTrue(self.valid(header(length=128 * 1024 * 1024)))
        self.assertTrue(self.valid(header(name="a" * 31), "a" * 31 + ".map"))
        for kwargs in ({"version": 7}, {"version": 609}, {"kind": 0}, {"kind": 2},
                       {"build": "unsupported"}, {"length": 2047}, {"length": 128 * 1024 * 1024 + 1},
                       {"tag_offset": 2047}, {"tag_offset": 8193}, {"tag_offset": 0xFFFFFFF0},
                       {"tag_size": 0x23}, {"tag_size": 8192}, {"tag_size": 0xFFFFFFFF},
                       {"name": ""}, {"name": "../downrush"}, {"name": "bad:name"}):
            with self.subTest(kwargs=kwargs):
                self.assertFalse(self.valid(header(**kwargs)))
        self.assertFalse(self.valid(header(), "alias.map"))
        for offset in (0, 0x7FC, 32, 64):
            broken = header()
            broken[offset:offset + (32 if offset in (32, 64) else 4)] = b"x" * (32 if offset in (32, 64) else 4)
            self.assertFalse(self.valid(broken))

    def populate(self, directory, names):
        directory.mkdir()
        for name in names:
            (directory / f"{name}.map").write_bytes(header(name) + b"\0" * 6144)

    def test_discovery_order_aliases_invalid_files_and_immutable_lifetime(self):
        with tempfile.TemporaryDirectory(dir=self.folder) as temp:
            root = Path(temp); maps = root / "maps"
            self.populate(maps, ("h1pb_prisoner", "downrush", "atlas", "prisoner"))
            (maps / "ATLAS.MAP").write_bytes(header("atlas"))
            for name, data in {"v7.map": header("v7", version=7), "ui.map": header("ui", kind=2),
                               "bad.map": b"short", "other.map": header("mismatch"),
                               "wrongbuild.map": header("wrongbuild", build="unsupported"),
                               "partial.map.part": header("partial")}.items():
                (maps / name).write_bytes(data)
            (maps / "directory.map").mkdir()
            (maps / "broken.map").symlink_to(maps / "missing.map")
            second = root / "second"; self.populate(second, ("late",))
            before = {p.name: p.read_bytes() for p in maps.iterdir() if p.is_file()}
            names = self.run_fixture("scanner", "immutable", maps, second)
            self.assertEqual(names[:13], [f"levels\\test\\{name}\\{name}" for name in STOCK])
            self.assertEqual(names[13:], ["atlas", "downrush", "h1pb_prisoner"])
            self.assertEqual(before, {p.name: p.read_bytes() for p in maps.iterdir() if p.is_file()})

    def test_capacity_and_missing_directory_fallback(self):
        with tempfile.TemporaryDirectory(dir=self.folder) as temp:
            maps = Path(temp) / "maps"
            self.populate(maps, [f"custom_{i:03}" for i in range(150)])
            self.assertEqual(len(self.run_fixture("scanner", "invalid_stock", maps)), 128)
            self.assertEqual(len(self.run_fixture("scanner", "scan", maps / "missing")), 13)

    def test_production_pc_and_xbox_selectors_captions_and_saved_identity(self):
        with tempfile.TemporaryDirectory(dir=self.folder) as temp:
            maps = Path(temp) / "maps"; self.populate(maps, ("atlas", "downrush", "h1pb_prisoner"))
            self.run_fixture("menus", maps)

    def test_direct_cache_header_and_multiplayer_slot_capacity_agree(self):
        self.run_fixture("cache")


if __name__ == "__main__":
    unittest.main()
