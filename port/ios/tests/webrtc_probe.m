/* Opt-in real WebKit-to-Chromium test; game assets and iOS signing not needed. */
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "../network/room_bridge.h"
#include "../../linux/src/posix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fail;
struct address { uint16_t family, port; uint32_t ip; unsigned char padding[8]; };
int main(int argc,char **argv) { @autoreleasepool {
    if(argc!=3)return 2;
    [NSApplication sharedApplication];
    WKWebView *web=[[WKWebView alloc]initWithFrame:NSMakeRect(0,0,200,100) configuration:[WKWebViewConfiguration new]];
    NSWindow *window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,200,100) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    window.contentView=web;[window orderBack:nil];
    NSMutableString *html=[NSMutableString stringWithString:@"<script>"];
    NSString *root=[NSString stringWithUTF8String:argv[1]];
    for(NSString *name in @[@"port/web/site/net.js",@"port/ios/network/room.js"])
        [html appendString:[NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:name] encoding:NSUTF8StringEncoding error:nil]];
    NSData *json=[NSJSONSerialization dataWithJSONObject:@{@"room":[NSString stringWithUTF8String:argv[2]]} options:0 error:nil];
    [html appendFormat:@"\nHaloNative.join(%@);</script>",[[NSString alloc]initWithData:json encoding:NSUTF8StringEncoding]];
    [web loadHTMLString:html baseURL:[NSURL URLWithString:@"https://halo-native.invalid/"]];
    struct address local={2,0x3412,0,{0}},remote={2,0x7856,0xffffffff,{0}};
    int udp=posix_socket(2,2,0);posix_socket_bind(udp,&local,sizeof(local));posix_socket_set_nonblocking(udp,1);
    __block BOOL busy=NO;__block uint32_t ack=0;__block int connected=0;
    __block NSTimeInterval last_send=0;
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:65];
    unsigned char payload[4096];for(int i=0;i<4096;i++)payload[i]=(unsigned char)(i*37);
    enum { STREAM_SIZE=256*1024 };
    unsigned char *stream_data=malloc(STREAM_SIZE), *stream_reply=malloc(STREAM_SIZE);
    for(int i=0;i<STREAM_SIZE;i++)stream_data[i]=(unsigned char)(i*13+i/4096);
    int stream=-1, sent=0, received_count=0, udp_ok=0;
    while(deadline.timeIntervalSinceNow>0 && !fail){
        if(!web.loading && !busy){
            NSMutableData*out=[NSMutableData dataWithLength:256*1024];uint32_t end;
            size_t count=ios_room_read_output(out.mutableBytes,out.length,&end);out.length=count;
            NSData *request=[NSJSONSerialization dataWithJSONObject:@{@"out":[out base64EncodedStringWithOptions:0],@"ack":@(ack)} options:0 error:nil];
            busy=YES;
            [web evaluateJavaScript:[NSString stringWithFormat:@"HaloNative.exchange(%@)",[[NSString alloc]initWithData:request encoding:NSUTF8StringEncoding]] completionHandler:^(id value,NSError*error){
                busy=NO;if(error){fprintf(stderr,"WebKit: %s\n",error.localizedDescription.UTF8String);fail=1;return;}
                ios_room_ack_output(end-(uint32_t)count+[value[@"sent"] unsignedIntValue]);
                NSData*incoming=[[NSData alloc]initWithBase64EncodedString:value[@"input"][@"data"] options:0];
                if(incoming.length && ios_room_write_input(incoming.bytes,incoming.length))ack=[value[@"input"][@"id"] unsignedIntValue];
                web_shared_state()->net_local_address=[value[@"address"] unsignedIntValue];
                if([value[@"selected"] isKindOfClass:NSDictionary.class])connected=1;
                if([value[@"error"] isKindOfClass:NSString.class]){fprintf(stderr,"Room: %s\n",[value[@"error"] UTF8String]);fail=1;}
            }];
        }
        NSTimeInterval now=NSDate.timeIntervalSinceReferenceDate;
        if(connected && !udp_ok && now-last_send>.5){posix_socket_sendto(udp,payload,sizeof(payload),0,&remote,sizeof(remote));last_send=now;}
        unsigned char received[4096];struct address from;int from_size=sizeof(from);
        int n=posix_socket_recvfrom(udp,received,sizeof(received),0,&from,&from_size);
        if(!udp_ok && n==sizeof(payload) && !memcmp(payload,received,sizeof(payload))){
            puts("PASS: native -> WebKit WebRTC -> Chromium -> native, 4096 exact datagram bytes");
            udp_ok=1;stream=posix_socket(2,1,0);posix_socket_set_nonblocking(stream,1);
            from.port=remote.port;
            if(posix_socket_connect(stream,&from,sizeof(from))<0 && posix_socket_last_error()!=10035){fprintf(stderr,"stream connect failed\n");return 1;}
        }
        if(stream>=0){
            if(sent<STREAM_SIZE){int written=posix_socket_send(stream,stream_data+sent,STREAM_SIZE-sent,0);if(written>0)sent+=written;}
            if(received_count<STREAM_SIZE){int got=posix_socket_recv(stream,stream_reply+received_count,STREAM_SIZE-received_count,0);if(got>0)received_count+=got;}
            if(received_count==STREAM_SIZE){
                if(memcmp(stream_data,stream_reply,STREAM_SIZE)){fprintf(stderr,"FAIL: stream differs\n");return 1;}
                printf("PASS: reliable stream round-trip, %d exact ordered bytes\n",STREAM_SIZE);
                posix_socket_close(stream);posix_socket_close(udp);free(stream_data);free(stream_reply);return 0;
            }
        }
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.008]];
    }
    fprintf(stderr,"FAIL: no native/browser round-trip before timeout (selection=%d)\n",connected);return 1;
}}
