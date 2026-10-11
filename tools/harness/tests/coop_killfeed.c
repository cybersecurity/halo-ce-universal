#include "harness.h"
#include <stdarg.h>
#include <wchar.h>
#include <math.h>
#define HALO_PORT_MAXIMUM_NETWORK_PLAYERS 128
#define HALO_PORT_MAXIMUM_OBJECTS_PER_MAP 8192
#define MIN(a,b) ((a)<(b)?(a):(b))
typedef unsigned short word;
typedef struct {real alpha,red,green,blue;} real_argb_color;
#define SET_FLAG(v,b,on) ((v)=((v)&~(1u<<(b)))|((on)?1u<<(b):0))
enum { _game_connection_local, _game_connection_network_client, _game_connection_network_server };
enum { _distributed_message_social_request=96,_distributed_message_social_line,_distributed_message_social_scores };
enum { _distributed_to_clients, _distributed_to_clients_reliably };
enum { _object_dead_bit, _object_outside_of_map_bit, _damage_no_statistics_bit, _damage_headshot_bit, _damage_chain_reaction_bit };
struct distributed_message_header { word header; byte type,count; long game_time; };
#define GET_MESSAGE_TYPE(h) (((h)>>2)&3)
static void build_message_header(word *h,word size,byte type,byte flags){*h=(size<<4)|(type<<2)|flags;}
struct player_datum { wchar_t name[13]; boolean quit_out_of_game; long unit_index; short team_index; struct {short machine_index;} network_player_data; };
struct object_datum {long definition_index; struct {unsigned long flags,damage_flags;short owner_team_index;} object;};
struct unit_datum {long definition_index; struct {unsigned long flags,damage_flags;short owner_team_index;} object; struct {long player_index;}unit;};
struct damage_data {long definition_index,owner_player_index,owner_object_index;unsigned long flags;};
struct damage_definition {short category;};
static struct damage_effect_definition {struct damage_definition damage;} damage_def;
static struct player_datum players[128];static long handles[128];
static struct unit_datum units[20];static long unit_handles[20];
static int role=2;static boolean coop=TRUE,playing=TRUE,menu=FALSE;
static long percent=100;static unsigned long clock_ms=1000;static long tick=30;
static int killed,cleared,shown,sent,chatted,warned;static long last_killed;
static char shown_text[256],chat_text[256];static byte wire[2048];static word wire_size;
static void *player_data=(void*)1;
struct data_iterator {long datum_index;int next;};
static void data_iterator_new(struct data_iterator *it,void *array){it->next=0;}
static void *data_iterator_next(struct data_iterator *it){while(it->next<128){int n=it->next++;if(handles[n]!=NONE){it->datum_index=handles[n];return &players[n];}}return NULL;}
static struct player_datum *player_try_and_get(long index){int n=DATUM_INDEX_TO_ABSOLUTE_INDEX(index);return index!=NONE&&n<128&&handles[n]==index?&players[n]:NULL;}
static struct unit_datum *unit_try_and_get(long index){int n=DATUM_INDEX_TO_ABSOLUTE_INDEX(index);return index!=NONE&&n<20&&unit_handles[n]==index?&units[n]:NULL;}
static struct object_datum *object_try_and_get(long index){return (struct object_datum*)unit_try_and_get(index);}
static long distributed_player_from_byte(byte n){return n<128?handles[n]:NONE;}
static long local_player_get_player_index(int n){return handles[n];}
static int game_connection(void){return role;}
static boolean network_coop_active(void){return coop;}
static boolean game_in_progress(void){return playing;}
static boolean main_menu_is_active(void){return menu;}
static boolean console_is_active(void){return FALSE;}
static long game_time_get(void){return tick;}
static unsigned long system_milliseconds(void){return clock_ms;}
static long config_integer(char const *name){return percent;}
static boolean game_team_is_enemy(short a,short b){return a>=0&&b>=0&&a!=b;}
static char const *tags[]={"characters\\cyborg\\cyborg","characters\\grunt\\grunt","characters\\elite\\elite","characters\\hunter\\hunter","characters\\floodcombat elite\\floodcombat elite","vehicles\\wraith\\wraith","weapons\\plasma grenade\\explosion","weapons\\frag grenade\\explosion","globals\\falling","globals\\maximum distance"};
static char const *tag_get_name(long index){return tags[index];}
static struct damage_effect_definition *damage_effect_definition_get(long index){return &damage_def;}
static void unit_kill(long index){killed++;last_killed=index;}
static void chat_close(void){cleared++;}
static void chat_clear(void){chat_close();}
static void chat_note_line(char const *text){chatted++;snprintf(chat_text,sizeof(chat_text),"%s",text);}
static void console_warning(char const *text,...){warned++;}
static void terminal_printf(real_argb_color const *color,char const *format,...){va_list args;va_start(args,format);vsnprintf(shown_text,sizeof(shown_text),format,args);va_end(args);shown++;}
static void telnet_console_print(char const *text){snprintf(shown_text,sizeof(shown_text),"%s",text);shown++;}
static void distributed_send(void *message,byte type,byte count,word size,int direction){memcpy(wire,message,size);wire_size=size;sent++;}
boolean network_distributed_client_send_reliably(void *message,word size){memcpy(wire,message,size);wire_size=size;sent++;return TRUE;}

enum { _object_die_act_of_god_bit=6, _object_die_act_of_god_silent_bit, _object_die_act_of_god_no_statistics_bit };
static void distributed_send_notice(char const *s){snprintf(shown_text,sizeof(shown_text),"%s",s);sent++;shown++;}
#include "under_test.inc"
static void setup(void){
 for(int i=0;i<128;i++)handles[i]=NONE;for(int i=0;i<20;i++)unit_handles[i]=NONE;
 memset(players,0,sizeof(players));memset(units,0,sizeof(units));
 for(int i=0;i<2;i++){handles[i]=0x10000+i;unit_handles[i]=0x20000+i;players[i].unit_index=unit_handles[i];players[i].team_index=1;players[i].network_player_data.machine_index=i;units[i].unit.player_index=handles[i];units[i].object.owner_team_index=1;}
 wcscpy(players[0].name,L"Host");wcscpy(players[1].name,L"Quixote");
 unit_handles[2]=0x20002;units[2].unit.player_index=NONE;units[2].object.owner_team_index=2;units[2].definition_index=1;
}

int main(int argc,char **argv){CHECK(argc==2,"case");char const *case_name=argv[1];setup();network_killfeed_reset();struct damage_data d={0,NONE,unit_handles[2],0};
 CASE("causes") {
  network_killfeed_note_death(unit_handles[1],&d);CHECK(strstr(shown_text,"Quixote was killed by Grunt"),"AI name");
  network_killfeed_reset();units[2].definition_index=4;network_killfeed_note_death(unit_handles[1],&d);CHECK(strstr(shown_text,"Combat Flood"),"flood");
  network_killfeed_reset();d.definition_index=6;network_killfeed_note_death(unit_handles[1],&d);CHECK(strstr(shown_text,"Plasma Grenade"),"plasma");
  network_killfeed_reset();d.definition_index=7;network_killfeed_note_death(unit_handles[1],&d);CHECK(strstr(shown_text,"Frag Grenade"),"frag");
  network_killfeed_reset();d.definition_index=0;units[2].definition_index=5;damage_def.damage.category=9;network_killfeed_note_death(unit_handles[1],&d);CHECK(strstr(shown_text,"run over by Wraith Tank"),"vehicle");
  network_killfeed_reset();damage_def.damage.category=1;network_killfeed_note_death(unit_handles[1],&d);CHECK(strstr(shown_text,"died of fall"),"fall");
  network_killfeed_reset();d.definition_index=9;network_killfeed_note_death(unit_handles[1],&d);CHECK(strstr(shown_text,"fell out of the world"),"world");
 }
 else CASE("players") {
  d.owner_player_index=handles[0];network_killfeed_note_death(unit_handles[1],&d);CHECK(strstr(shown_text,"Host")&&strstr(shown_text,"team kill"),"teamkiller hidden");
  network_killfeed_reset();players[0].team_index=2;network_killfeed_note_death(unit_handles[1],&d);CHECK(!strstr(shown_text,"team kill"),"enemy called teamkiller");
  network_killfeed_reset();d.owner_player_index=handles[1];network_killfeed_note_death(unit_handles[1],&d);CHECK(strstr(shown_text,"killed themselves"),"suicide");
  network_killfeed_reset();d.owner_player_index=NONE;units[1].object.damage_flags=FLAG(_object_die_act_of_god_bit);network_killfeed_note_death(unit_handles[1],&d);CHECK(!strcmp(shown_text,"Quixote died"),"script death falsely labelled fall");
 }
 else CASE("dedup") {
  for(int i=0;i<10;i++)network_killfeed_note_death(unit_handles[1],&d);CHECK(sent==1,"duplicate death");network_killfeed_reset();network_killfeed_note_death(unit_handles[1],&d);CHECK(sent==2,"revert suppresses new death");
  unit_handles[1]=0x40001;players[1].unit_index=unit_handles[1];network_killfeed_note_death(unit_handles[1],&d);CHECK(sent==3,"unit generation reused");
 }
 else CASE("scope") {
  role=1;network_killfeed_note_death(unit_handles[1],&d);CHECK(!sent,"client authored notice");role=2;coop=FALSE;network_killfeed_note_death(unit_handles[1],&d);CHECK(!sent,"competitive duplicated");coop=TRUE;
  network_killfeed_note_death(unit_handles[2],&d);CHECK(!sent,"AI death announced");d.flags=FLAG(_damage_no_statistics_bit);network_killfeed_note_death(unit_handles[1],&d);CHECK(!sent,"script silent death");
 }
 else CASE("stale") {
  network_killfeed_note_death(0x30001,&d);CHECK(!sent,"stale unit");players[1].unit_index=unit_handles[0];network_killfeed_note_death(unit_handles[1],&d);CHECK(!sent,"stale player backlink");
  players[1].unit_index=unit_handles[1];players[1].quit_out_of_game=TRUE;network_killfeed_note_death(unit_handles[1],&d);CHECK(!sent,"quit player");network_killfeed_note_death(NONE,&d);network_killfeed_note_death(unit_handles[1],NULL);
 }
 else CASE("direct") {
  network_killfeed_note_death(unit_handles[1],NULL);CHECK(!strcmp(shown_text,"Quixote died")&&sent==1,"direct death omitted");
  network_killfeed_note_death(unit_handles[1],NULL);CHECK(sent==1,"duplicate direct death");
  network_killfeed_reset();network_killfeed_note_death(unit_handles[1],&d);network_killfeed_note_death(unit_handles[1],NULL);CHECK(sent==2,"damage and direct duplicate");
  network_killfeed_reset();d.flags=FLAG(_damage_no_statistics_bit);network_killfeed_note_death(unit_handles[1],&d);network_killfeed_note_death(unit_handles[1],NULL);CHECK(sent==2,"no-statistics death announced by fallback");
  for(int bit=_object_die_act_of_god_silent_bit;bit<=_object_die_act_of_god_no_statistics_bit;bit++){network_killfeed_reset();units[1].object.damage_flags=FLAG(bit);network_killfeed_note_death(unit_handles[1],NULL);CHECK(sent==2,"silent script death announced");}
 }
 else CASE("fallback") {
  d.definition_index=NONE;d.owner_object_index=NONE;players[1].name[1]='|';players[1].name[2]='\n';network_killfeed_note_death(unit_handles[1],&d);CHECK(strstr(shown_text,"an unknown cause")&&!strchr(shown_text,'|')&&!strchr(shown_text,'\n'),"unsafe/unknown text");
 }
 else return 2;return 0;
}
