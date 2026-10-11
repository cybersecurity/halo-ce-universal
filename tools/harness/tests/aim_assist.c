#define HALO_ANDROID 1
#include "harness.h"
#include <string.h>
typedef unsigned short word;
enum { _game_connection_local, _game_connection_network_client, _game_connection_network_server };
enum { _distributed_message_aim_assist_policy = 83, _distributed_to_clients_reliably };
struct distributed_message_header { word header; byte type,count; long game_time; };
#define GET_MESSAGE_TYPE(h) (((h)>>2)&3)
static int connection=2,playing=1,menu=0,block=0,preference=1;
static unsigned long changes,mouse_aimed_ms,stick_aimed_ms,touch_aimed_ms;
static int mouse_lock,sends,unreliable;
static boolean network_connection_last_read_was_unreliable(void){return unreliable;}
static byte wire[32];static word wire_size;
static int game_connection(void){return connection;}
static int game_in_progress(void){return playing;}
static int main_menu_is_active(void){return menu;}
int config_boolean(char const *name){return !strcmp(name,"input.mouse_aim_assist")?preference:block;}
static unsigned long config_changes(void){return changes;}
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
static void distributed_send(void *data,byte type,int count,word size,int destination){
 memcpy(wire,data,size);struct distributed_message_header *h=(void*)wire;h->header=8;h->type=type;h->count=count;wire_size=size;sends++;
}
static void distributed_send_to_machine_reliably(long machine,void *data,byte type,int count,word size){distributed_send(data,type,count,size,0);}
#include "policy.inc"
#include "input.inc"
static void receive(int from,int value,int count,int size){
 struct aim_assist_policy_message m={{8,83,count,0},value};
 network_aim_assist_handle_message(from,(word*)&m,size);
}
int main(int argc,char **argv){CHECK(argc==2,"case");char const *case_name=argv[1];network_aim_assist_reset();
 CASE("allowed") {CHECK(!halo_linux_mouse_aiming(0),"enabled preference lost");preference=0;changes++;CHECK(!halo_linux_mouse_aiming(0),"allowed default changed");mouse_aimed_ms=1;CHECK(halo_linux_mouse_aiming(0),"local disabled ignored");CHECK(!halo_linux_mouse_aiming(1),"other controller affected");}
 else CASE("devices") {block=1;CHECK(halo_linux_mouse_aiming(0),"keyboard default bypass");mouse_aimed_ms=20;stick_aimed_ms=10;CHECK(halo_linux_mouse_aiming(0),"mouse preference bypass");stick_aimed_ms=30;CHECK(!halo_linux_mouse_aiming(0),"controller blocked");CHECK(!halo_linux_mouse_aiming(1),"second controller blocked");mouse_aimed_ms=40;CHECK(halo_linux_mouse_aiming(0),"switch back bypass");touch_aimed_ms=50;CHECK(!halo_linux_mouse_aiming(0),"touch blocked");mouse_aimed_ms=60;CHECK(halo_linux_mouse_aiming(0),"mouse after touch bypass");}
 else CASE("scope") {block=1;connection=0;CHECK(!halo_linux_mouse_aiming(0),"singleplayer affected");connection=2;menu=1;CHECK(!network_aim_assist_block_mouse(),"main menu affected");menu=0;playing=0;CHECK(!network_aim_assist_block_mouse(),"loading affected");}
 else CASE("replication") {block=1;network_aim_assist_update();CHECK(sends==1&&wire_size==12,"wire layout");network_aim_assist_update();CHECK(sends==1,"unchanged spam");connection=1;block=0;network_aim_assist_handle_message(NONE,(word*)wire,wire_size);CHECK(network_aim_assist_block_mouse(),"client preference beat host");connection=2;network_aim_assist_send_policy(7);CHECK(sends==2,"late join policy missing");block=0;network_aim_assist_update();CHECK(sends==3,"changed policy missing");connection=1;network_aim_assist_handle_message(NONE,(word*)wire,wire_size);CHECK(!network_aim_assist_block_mouse(),"Allowed not restored");}
 else CASE("admission") {connection=1;receive(NONE,0,1,12);CHECK(!network_aim_assist_block_mouse(),"valid policy");unreliable=1;receive(NONE,1,1,12);CHECK(!network_aim_assist_block_mouse(),"datagram spoof");unreliable=0;receive(3,1,1,12);CHECK(!network_aim_assist_block_mouse(),"client spoof");receive(NONE,1,0,12);CHECK(!network_aim_assist_block_mouse(),"count accepted");receive(NONE,2,1,12);CHECK(!network_aim_assist_block_mouse(),"invalid policy accepted");for(int n=0;n<12;n++)receive(NONE,1,1,n);CHECK(!network_aim_assist_block_mouse(),"truncation accepted");receive(NONE,1,1,13);CHECK(!network_aim_assist_block_mouse(),"trailing bytes accepted");connection=2;receive(NONE,1,1,12);connection=1;CHECK(!network_aim_assist_block_mouse(),"host accepted policy");}
 else CASE("reset") {connection=1;receive(NONE,0,1,12);CHECK(!network_aim_assist_block_mouse(),"setup");network_aim_assist_reset();CHECK(network_aim_assist_block_mouse(),"previous host rule retained");receive(NONE,0,1,12);CHECK(!network_aim_assist_block_mouse(),"Allowed inaccessible");}
 else return 2;return 0;
}
