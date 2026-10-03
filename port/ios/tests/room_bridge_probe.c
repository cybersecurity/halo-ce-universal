#include "../network/room_bridge.h"
#include "../../linux/src/posix.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int host_ios_room_poll(unsigned int *command);
struct address { uint16_t family, port; uint32_t ip; unsigned char padding[8]; };
int main(void) {
    struct web_shared_state *s=web_shared_state();
    struct address local={2,0x3412,0,{0}}, remote={2,0x7856,0x0302010a,{0}};
    s->net_local_address=0x0402010a;
    int socket=posix_socket(2,2,0);
    assert(socket>=0 && posix_socket_bind(socket,&local,sizeof(local))==0);
    assert(posix_socket_set_nonblocking(socket,1)==0);
    /* Counter overflow and physical ring wrap are independent. */
    s->net_out_read=s->net_out_write=(int32_t)0xfffffff0u;
    assert(posix_socket_sendto(socket,"hello",5,0,&remote,sizeof(remote))==5);
    unsigned char output[128];uint32_t end;
    size_t size=ios_room_read_output(output,sizeof(output),&end);
    assert(size==32 && end==16);
    struct web_packet_header header;memcpy(&header,output,sizeof(header));
    assert(header.kind==WEB_PACKET_DATAGRAM && header.length==5);
    assert(!memcmp(output+sizeof(header),"hello",5));
    assert(!ios_room_ack_output(end+4));
    assert(ios_room_ack_output(end));
    assert(ios_room_read_output(output,sizeof(output),&end)==0);
    header.source_ip=remote.ip;header.destination_ip=(uint32_t)s->net_local_address;
    header.source_port=remote.port;header.destination_port=local.port;
    memcpy(output,&header,sizeof(header));
    s->net_in_read=s->net_in_write=(int32_t)0xfffffff0u;
    assert(ios_room_write_input(output,size));
    char received[16];struct address from;int from_size=sizeof(from);
    assert(posix_socket_recvfrom(socket,received,sizeof(received),0,&from,&from_size)==5);
    assert(!memcmp(received,"hello",5) && from.ip==remote.ip);
    assert((uint32_t)s->net_in_read==16);
    /* A malformed batch must never publish even its valid first frame. */
    uint32_t before=(uint32_t)s->net_in_write;
    assert(!ios_room_write_input(output,23));
    header.length=100;memcpy(output,&header,sizeof(header));
    assert(!ios_room_write_input(output,size));
    assert((uint32_t)s->net_in_write==before);
    header.length=5;memcpy(output,&header,sizeof(header));
    s->net_in_write=s->net_in_read+WEB_NET_IN_BYTES;
    assert(!ios_room_write_input(output,size));
    uint32_t command[4]={3,2,remote.ip,7},copy[4];
    for(int i=0;i<64;i++) assert(ios_room_queue_command(command));
    assert(!ios_room_queue_command(command));
    for(int i=0;i<64;i++){assert(host_ios_room_poll(copy));assert(!memcmp(command,copy,16));}
    assert(!host_ios_room_poll(copy));
    assert(posix_socket_close(socket)==0);
    puts("PASS: native browser sockets, wraparound, framing, backpressure, control ordering");
}
