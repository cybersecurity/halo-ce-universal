#!/usr/bin/env python3
"""Compile the actual UI callback/invoker bodies with controlled profile services."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'source/interface/ui_widget_event_handler_functions.c'


def function_body(text, name):
    import re
    matches = list(re.finditer(r'\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', text))
    if len(matches) != 1:
        raise RuntimeError(f'expected one definition of {name}, found {len(matches)}')
    start = matches[0].start()
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    text = SOURCE.read_text()
    callback = function_body(text, 'new_campaign_if_no_custom_player_profiles_exist')
    classifier = function_body(text, 'ui_widget_event_handler_function_is_branch_condition')
    invoker = function_body(text, 'ui_widget_event_handler_function_invoke')
    unit = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#define HALO_MACOS 1
#define TRUE 1
#define FALSE 0
#define NONE -1
#define NUMBEROF(a) (sizeof(a)/sizeof((a)[0]))
#define match_vassert(...) ((void)0)
typedef unsigned char boolean;
typedef uint16_t word;
struct widget_instance { int unused; };
struct event_record { int unused; };
typedef boolean (*handler)(struct widget_instance *,struct event_record *,boolean *);
static struct { handler functions[102]; const char *names[102]; } event_handler_function_list;
static int profile_count, name_opened, warnings, errors;
static void player_profiles_enumerate_available_to_local_player_index(int player, word *count, long *index, boolean defaults)
{ assert(player==NONE && *count==1 && !defaults); *count=(word)profile_count; *index=42; }
static boolean new_campaign_chosen(struct widget_instance *widget,struct event_record *event,boolean *deleted)
{ (void)widget;(void)event;(void)deleted;name_opened++;return TRUE; }
static void console_warning(const char *format,...){ assert(format);warnings++; }
static void error(int level,const char *message){ assert(level==2 && message);errors++; }
static boolean unrelated_false(struct widget_instance *w,struct event_record *e,boolean *d)
{ (void)w;(void)e;(void)d;return FALSE; }
'''
    unit += '\nstatic boolean ' + callback
    unit += '\nboolean ' + classifier
    unit += '\nboolean ' + invoker
    unit += r'''
int main(void)
{
 struct widget_instance widget={0};struct event_record event={0};boolean deleted=FALSE;
 for(unsigned int i=0;i<102;i++){event_handler_function_list.functions[i]=unrelated_false;event_handler_function_list.names[i]="unrelated";}
 event_handler_function_list.functions[101]=new_campaign_if_no_custom_player_profiles_exist;
 event_handler_function_list.names[101]="new game if no plyr profiles";
 assert(ui_widget_event_handler_function_is_branch_condition(101));
 assert(!ui_widget_event_handler_function_is_branch_condition(0));
 assert(!ui_widget_event_handler_function_is_branch_condition(102));
 assert(!ui_widget_event_handler_function_is_branch_condition(UINT16_MAX));
 profile_count=0;
 assert(!ui_widget_event_handler_function_invoke(&widget,&event,101,&deleted));
 assert(name_opened==1 && !deleted && !warnings && !errors);
 profile_count=1;name_opened=0;
 assert(ui_widget_event_handler_function_invoke(&widget,&event,101,&deleted));
 assert(!name_opened && !warnings && !errors);
 assert(!ui_widget_event_handler_function_invoke(&widget,&event,0,&deleted));
 assert(warnings==1 && !errors);
 assert(!ui_widget_event_handler_function_invoke(&widget,&event,102,&deleted));
 assert(warnings==1 && errors==1);
 puts("UI condition: zero profiles opens naming and retains FALSE; existing profile TRUE; unrelated failure warning retained; bounds checked");
}
'''
    with tempfile.TemporaryDirectory(prefix='halo-ui-condition-') as directory:
        source = Path(directory) / 'condition.c'
        executable = Path(directory) / 'condition-test'
        source.write_text(unit)
        subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-UNDEBUG', str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
