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

enum { _distributed_message_suicide_request=84 };
#include "under_test.inc"
static void setup(void){
 for(int i=0;i<128;i++)handles[i]=NONE;for(int i=0;i<20;i++)unit_handles[i]=NONE;
 memset(players,0,sizeof(players));memset(units,0,sizeof(units));
 for(int i=0;i<2;i++){handles[i]=0x10000+i;unit_handles[i]=0x20000+i;players[i].unit_index=unit_handles[i];players[i].team_index=1;players[i].network_player_data.machine_index=i;units[i].unit.player_index=handles[i];units[i].object.owner_team_index=1;}
 wcscpy(players[0].name,L"Host");wcscpy(players[1].name,L"Quixote");
 unit_handles[2]=0x20002;units[2].unit.player_index=NONE;units[2].object.owner_team_index=2;units[2].definition_index=1;
}

static void request(long machine,long player,long unit,boolean reliable,int size,int count){
 struct suicide_request m={{8,84,count,30},player,unit};network_suicide_handle_message(machine,(word*)&m,size,reliable);
}
int main(int argc,char **argv){CHECK(argc==2,"case");char const *case_name=argv[1];setup();network_suicide_reset();boolean success=FALSE;
 CASE("commands") {
  CHECK(network_suicide_console_command("kill",&success)&&success&&killed==1,"host kill");
  clock_ms+=1000;CHECK(network_suicide_console_command(" ( SUICIDE ) ",&success)&&success&&killed==2,"alias/case/parentheses");
  CHECK(!network_suicide_console_command("killall",&success),"script prefix hijacked");
  CHECK(!network_suicide_console_command("suicide_now",&success),"prefix hijacked");
  CHECK(network_suicide_console_command("kill Quixote",&success)&&!success&&killed==2,"target argument accepted");
  CHECK(network_suicide_console_command("kill)",&success)&&!success,"unbalanced parenthesis");
  role=0;clock_ms+=1000;CHECK(network_suicide_console_command("kill",&success)&&success&&killed==3,"singleplayer");
 }
 else CASE("client") {
  role=1;CHECK(network_suicide_console_command("suicide",&success)&&success&&sent==1&&killed==0,"client killed locally");
  CHECK(wire_size==16&&((struct suicide_request*)wire)->unit_index==players[0].unit_index,"wire identity");
  role=2;request(1,handles[1],players[1].unit_index,TRUE,16,1);CHECK(killed==1&&last_killed==players[1].unit_index,"own death refused");
 }
 else CASE("admission") {
  request(1,handles[0],players[0].unit_index,TRUE,16,1);CHECK(!killed,"wrong stream player");
  request(1,handles[1],players[1].unit_index,FALSE,16,1);CHECK(!killed,"datagram spoof");
  request(NONE,handles[1],players[1].unit_index,TRUE,16,1);CHECK(!killed,"no machine");
  role=1;request(1,handles[1],players[1].unit_index,TRUE,16,1);CHECK(!killed,"client accepted a request");role=2;
  for(int n=0;n<16;n++)request(1,handles[1],players[1].unit_index,TRUE,n,1);
  request(1,handles[1],players[1].unit_index,TRUE,17,1);request(1,handles[1],players[1].unit_index,TRUE,16,2);CHECK(!killed,"malformed accepted");
 }
 else CASE("stale") {
  long old=players[1].unit_index;players[1].unit_index=unit_handles[2];units[2].unit.player_index=handles[1];
  request(1,handles[1],old,TRUE,16,1);CHECK(!killed,"old unit killed respawn");
  request(1,0x30001,unit_handles[2],TRUE,16,1);CHECK(!killed,"stale player accepted");
  units[2].unit.player_index=handles[0];request(1,handles[1],unit_handles[2],TRUE,16,1);CHECK(!killed,"wrong unit backlink");
  units[2].unit.player_index=handles[1];units[2].object.damage_flags=FLAG(_object_dead_bit);request(1,handles[1],unit_handles[2],TRUE,16,1);CHECK(!killed,"dead unit");
  units[2].object.damage_flags=0;players[1].quit_out_of_game=TRUE;request(1,handles[1],unit_handles[2],TRUE,16,1);CHECK(!killed,"departed player");
 }
 else CASE("rate") {
  for(int i=0;i<20;i++)request(1,handles[1],players[1].unit_index,TRUE,16,1);CHECK(killed==1,"request flood");
  clock_ms+=1000;request(1,handles[1],players[1].unit_index,TRUE,16,1);CHECK(killed==2,"rate never resets");
  handles[1]=0x30001;units[1].unit.player_index=handles[1];request(1,handles[1],players[1].unit_index,TRUE,16,1);CHECK(killed==3,"new player inherited rate");
 }
 else CASE("scope") {menu=TRUE;CHECK(network_suicide_console_command("kill",&success)&&!success&&!killed,"menu kill");menu=FALSE;playing=FALSE;request(1,handles[1],players[1].unit_index,TRUE,16,1);CHECK(!killed,"loading request");}
 else return 2;return 0;
}
