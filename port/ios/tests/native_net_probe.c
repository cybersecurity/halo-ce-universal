/* Real Darwin sockets through the game's 32-bit, Winsock-shaped boundary. */
#include "posix.h"
#include "network_mode.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct address {uint16_t family,port;uint32_t ip;unsigned char padding[8];};
int main(void) {
    assert(!halo_browser_rooms);
    struct address addr={2,0,htonl(INADDR_LOOPBACK),{0}},peer;
    int size=sizeof(addr),type=0,type_size=sizeof(type);
    int listener=posix_socket(2,1,0);assert(listener>=0);
    assert(!posix_socket_bind(listener,&addr,sizeof(addr)));
    assert(!posix_socket_listen(listener,2));
    assert(!posix_socket_getsockname(listener,&addr,&size));assert(addr.family==2 && addr.port);
    assert(!posix_socket_getsockopt(listener,0xffff,0x1008,&type,&type_size) && type==1);
    int client=posix_socket(2,1,0);assert(client>=0);
    assert(!posix_socket_connect(client,&addr,sizeof(addr)));
    size=sizeof(peer);int server=posix_socket_accept(listener,&peer,&size);assert(server>=0 && peer.family==2);
    assert(!posix_socket_set_nodelay(client));assert(posix_socket_send(client,"hello",5,0)==5);
    int read=server,count=1;assert(posix_socket_select(&read,&count,0,0,0,0,1,0,0)==1 && count==1);
    char buffer[20];assert(posix_socket_recv(server,buffer,sizeof(buffer),0)==5 && !memcmp(buffer,"hello",5));
    assert(!posix_socket_set_nonblocking(server,1));assert(posix_socket_recv(server,buffer,sizeof(buffer),0)==-1);assert(posix_socket_last_error()==10035);
    assert(!posix_socket_close(server));assert(!posix_socket_close(client));assert(!posix_socket_close(listener));
    int udp=posix_socket(2,2,0);assert(udp>=0);addr.port=0;assert(!posix_socket_bind(udp,&addr,sizeof(addr)));
    size=sizeof(addr);assert(!posix_socket_getsockname(udp,&addr,&size));
    assert(posix_socket_sendto(udp,"udp",3,0,&addr,sizeof(addr))==3);
    read=udp;count=1;assert(posix_socket_select(&read,&count,0,0,0,0,1,0,0)==1);
    size=sizeof(peer);assert(posix_socket_recvfrom(udp,buffer,sizeof(buffer),0,&peer,&size)==3 && peer.family==2 && peer.port==addr.port && !memcmp(buffer,"udp",3));
    assert(!posix_socket_close(udp));
    puts("PASS: native TCP/UDP, Winsock options/errors, Darwin sockaddr conversion and readiness");
}
