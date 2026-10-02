"""Run the production connection poll across a browser scheduling pause."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / "source/networking/network_connection.c").read_text()


def function(name):
    match = re.search(r"^(?:static )?boolean " + name + r"\([^;]*?\)\s*\{", SOURCE, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[match.start():end]


BOUNDARY = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int boolean;
typedef unsigned char byte;
#define TRUE 1
#define FALSE 0
#define DATAGRAM_MAXIMUM_SIZE 1024
#define RELIABLE_MESSAGE_MAXIMUM_SIZE 1024
#define MILLISECONDS_PER_SECOND 1000
#define IPV4_ADDRESS_LENGTH 4
#define NETWORK_CONNECTION_MAXIMUM_CLIENTS 16
#define MAXIMUM_SKIPPED_DATAGRAMS_PER_IDLE 64
typedef unsigned short message_header;
#define FLAG(b) (1U<<(b))
#define TEST_FLAG(f,b) ((f)&FLAG(b))
#define SET_FLAG(f,b,v) ((f)=(v)?(f)|FLAG(b):(f)&~FLAG(b))
#define match_assert(f,l,c) assert(c)
#define match_vassert(f,l,c,...) assert(c)
#define error(...) ((void)0)
#define csmemcpy memcpy
enum { _connection_going_stale_bit, _connection_create_server_bit,
 _connection_create_clientside_client_bit, _connection_create_serverside_client_bit, _connection_closed_bit };
enum { _error_silent, _transport_error_none, _network_connection_traffic_event_datagram_received,
 _network_connection_traffic_event_stream_bytes_received };
enum { _transport_result_operation_would_block=-1, _transport_error_connection_lost=-2,
 _transport_error_endpoint_io=-3 };
struct transport_address { short address_length; union { unsigned long long_words[1]; } address; };
struct transport_endpoint { int pending; };
struct circular_queue { int count; };
struct network_connection { unsigned long flags,last_keep_alive_time;
 struct transport_endpoint *reliable_endpoint,*unreliable_endpoint;
 struct circular_queue *reliable_incoming_queue,*unreliable_incoming_queue; };
struct network_server_connection { struct network_connection connection;
 struct network_connection *client_list[NETWORK_CONNECTION_MAXIMUM_CLIENTS]; };
static unsigned long now=30000;
static int global_connection_dont_timeout;
static unsigned long system_milliseconds(void) { return now; }
static long circular_queue_free_space(struct circular_queue *q) { return 1024-q->count; }
static boolean circular_queue_queue_data(struct circular_queue *q, void *b, long n) { (void)b; q->count+=n; return TRUE; }
static boolean endpoint_readable(struct transport_endpoint *e, int timeout) { (void)timeout; return e->pending; }
static boolean endpoint_connected(struct transport_endpoint *e) { (void)e; return TRUE; }
static boolean endpoint_blocking(struct transport_endpoint *e) { (void)e; return FALSE; }
static long read_endpoint(struct transport_endpoint *e, void *b, long n) {
 (void)n; if (!e->pending) return _transport_result_operation_would_block;
 e->pending=0; *(byte *)b=1; return 1; }
static long read_from_endpoint(struct transport_endpoint *e, void *b, long n, struct transport_address *a) {
 (void)a; return read_endpoint(e,b,n); }
static short get_endpoint_address(struct transport_endpoint *e, struct transport_address *a) {
 (void)e; a->address.long_words[0]=1; return _transport_error_none; }
static void network_connection_get_address(struct network_connection *c,
 struct transport_address *reliable, struct transport_address *unreliable) {
 (void)c; if (reliable) reliable->address.long_words[0]=1;
 if (unreliable) unreliable->address.long_words[0]=1; }
static boolean network_connection_flush_reliable(struct network_connection *c)
{ (void)c; return TRUE; }
static long network_connection_datagram_size(byte const *buffer) { (void)buffer; return 0; }
static const char *transport_error_to_string(short error) { (void)error; return "test"; }
static void network_connection_log_traffic_event(int event, long size, struct network_connection *c) {
 (void)event; (void)size; (void)c; }
static boolean network_connection_idle_server_reliable_endpoint(struct network_server_connection *c,
 struct network_connection **new_client) { (void)c; (void)new_client; return TRUE; }
'''
FUNCTIONS = function("network_connection_idle_client_reliable_endpoint") + "\n" + function("network_connection_idle")
TESTS = r'''
int main(void) {
 struct transport_endpoint endpoint={1}; struct circular_queue queue={0};
 struct network_connection c={FLAG(_connection_create_clientside_client_bit),1000,&endpoint,NULL,&queue,NULL};
 /* RTC has queued a keepalive during a worker pause longer than 15s. */
#ifdef HALO_WEB
 assert(network_connection_idle(&c,15000,NULL));
 assert(c.last_keep_alive_time==now && queue.count==1 && !TEST_FLAG(c.flags,_connection_going_stale_bit));
#else
 assert(!network_connection_idle(&c,15000,NULL)); assert(endpoint.pending && !queue.count);
#endif
 endpoint.pending=0; c.last_keep_alive_time=now-16000;
 assert(!network_connection_idle(&c,15000,NULL));
 c.last_keep_alive_time=now-6000; assert(network_connection_idle(&c,15000,NULL));
 assert(TEST_FLAG(c.flags,_connection_going_stale_bit));
 global_connection_dont_timeout=1; c.last_keep_alive_time=now-16000;
 assert(network_connection_idle(&c,15000,NULL) && c.last_keep_alive_time==now);
 puts("Connection pause: queued keepalive, real timeout, stale indication and native ordering passed");
}
'''

with tempfile.TemporaryDirectory(prefix="halo-connection-pause-") as directory:
    for web in (False, True):
        source, binary = Path(directory) / "test.c", Path(directory) / "test"
        source.write_text(BOUNDARY + FUNCTIONS + TESTS)
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-sign-compare", "-fsanitize=address,undefined",
                        *(["-DHALO_WEB=1"] if web else []), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
