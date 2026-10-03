/* Real OS sockets are internal; the game uses PR #12 virtual sockets. */
#define posix_socket_last_error ios_native_posix_socket_last_error
#define posix_socket ios_native_posix_socket
#define posix_socket_close ios_native_posix_socket_close
#define posix_socket_bind ios_native_posix_socket_bind
#define posix_socket_listen ios_native_posix_socket_listen
#define posix_socket_connect ios_native_posix_socket_connect
#define posix_socket_accept ios_native_posix_socket_accept
#define posix_socket_send ios_native_posix_socket_send
#define posix_socket_sendto ios_native_posix_socket_sendto
#define posix_socket_recv ios_native_posix_socket_recv
#define posix_socket_recvfrom ios_native_posix_socket_recvfrom
#define posix_socket_shutdown ios_native_posix_socket_shutdown
#define posix_socket_set_nonblocking ios_native_posix_socket_set_nonblocking
#define posix_socket_set_nodelay ios_native_posix_socket_set_nodelay
#define posix_socket_bytes_available ios_native_posix_socket_bytes_available
#define posix_socket_setsockopt ios_native_posix_socket_setsockopt
#define posix_socket_getsockopt ios_native_posix_socket_getsockopt
#define posix_socket_getsockname ios_native_posix_socket_getsockname
#define posix_socket_getpeername ios_native_posix_socket_getpeername
#define posix_socket_select ios_native_posix_socket_select
#define posix_local_ipv4_address ios_native_posix_local_ipv4_address
#define posix_resolve_ipv4 ios_native_posix_resolve_ipv4
/* Adapt Linux socket helpers to Darwin's sockaddr length byte and socket flags. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x80000
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
static struct sockaddr_storage sockaddr_inward(const void *input,socklen_t size) {
    struct sockaddr_storage address={0};
    if(size>sizeof(address))size=sizeof(address);
    if(input && size>=2){memcpy(&address,input,size);uint16_t family;memcpy(&family,input,2);address.ss_len=size;address.ss_family=family;}
    return address;
}
static void sockaddr_outward(void *address,socklen_t size) {
    if(address && size>=2){uint16_t family=((struct sockaddr *)address)->sa_family;memcpy(address,&family,2);}
}
static int ios_socket(int family,int type,int protocol) {
    int fd=socket(family,type&~SOCK_CLOEXEC,protocol);
    if(fd>=0){int one=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));fcntl(fd,F_SETFD,FD_CLOEXEC);}return fd;
}
static int ios_bind(int fd,const void *p,socklen_t n) {struct sockaddr_storage a=sockaddr_inward(p,n);return bind(fd,(struct sockaddr *)&a,n);}
static int ios_connect(int fd,const void *p,socklen_t n) {struct sockaddr_storage a=sockaddr_inward(p,n);return connect(fd,(struct sockaddr *)&a,n);}
static int ios_accept4(int fd,void *p,socklen_t *n,int flags) {
    (void)flags;int out=accept(fd,p,n);if(out>=0){fcntl(out,F_SETFD,FD_CLOEXEC);if(n)sockaddr_outward(p,*n);}return out;
}
static ssize_t ios_sendto(int fd,const void *p,size_t n,int flags,const void *address,socklen_t size) {
    struct sockaddr_storage a=sockaddr_inward(address,size);return sendto(fd,p,n,flags,(struct sockaddr *)&a,size);
}
static ssize_t ios_recvfrom(int fd,void *p,size_t n,int flags,void *address,socklen_t *size) {
    ssize_t out=recvfrom(fd,p,n,flags,address,size);if(out>=0 && size)sockaddr_outward(address,*size);return out;
}
static int ios_getsockname(int fd,void *p,socklen_t *n) {int r=getsockname(fd,p,n);if(!r)sockaddr_outward(p,*n);return r;}
static int ios_getpeername(int fd,void *p,socklen_t *n) {int r=getpeername(fd,p,n);if(!r)sockaddr_outward(p,*n);return r;}
static ssize_t ios_getrandom(void *p,size_t size,unsigned flags) {(void)flags;arc4random_buf(p,size);return size;}
#define socket ios_socket
#define bind ios_bind
#define connect ios_connect
#define accept4 ios_accept4
#define sendto ios_sendto
#define recvfrom ios_recvfrom
#define getsockname ios_getsockname
#define getpeername ios_getpeername
#define getrandom ios_getrandom
#include "../../linux/src/posix_net.c"

/* Browser rooms use ICE/STUN/TURN; native router forwarding is disabled. */
int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port,
    posix_ulong *address, unsigned short *external_port, char *error, int error_size)
{
    (void)port; (void)preferred_port;
    if (address) *address = 0;
    if (external_port) *external_port = 0;
    if (error && error_size > 0) snprintf(error, (size_t)error_size, "Browser rooms use WebRTC");
    return 0;
}
void posix_upnp_stop_forwarding_udp(unsigned short port) { (void)port; }
