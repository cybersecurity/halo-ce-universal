#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "guest_image.h"
#include "guest_address.h"
#include "recomp_state.h"
#include "host_syscall.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static void unexpected(uint32_t va)
{
	fprintf(stderr, "unexpected guest call %08x\n", va);
	abort();
}
void recomp_unsupported_instruction(uint32_t va) { unexpected(va); }
void recomp_icall_fail_log(uint32_t va) { unexpected(va); }
void recomp_icall_not_code_log(uint32_t va) { unexpected(va); }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return NULL; }
void host_get_tp(void) { g_eax = 0x60002000; g_esp += 4; }
void host_syscall(void)
{
	int64_t result = mac_host_arm64_32_syscall_from_stack(g_esp);
	g_eax = (uint32_t)result;
	g_edx = (uint32_t)((uint64_t)result >> 32);
	g_esp += 4;
}

static void decode(unsigned size){mac_recomp_state_initialize_thread(0x6000f000,0x60002000);MEM32(g_esp)=0;MEM32(g_esp+4)=1;MEM32(g_esp+8)=0x60001000;MEM32(g_esp+12)=size;TEST_network_distributed_handle_message();assert(g_esp==0x6000f004);}
int main(int argc,char **argv){
assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);void *bytes=malloc(n);assert(fread(bytes,1,n,f)==(size_t)n);fclose(f);
struct mac_guest_image image;char error[256];assert(mac_guest_image_load(bytes,n,&image,error,sizeof(error))==0);assert(mac_guest_address_commit(0x60000000,0x10000,PROT_READ|PROT_WRITE)==0);assert(mac_recomp_state_set_image(image.memory_offset,image.code_lo_va,image.code_hi_va)==0);
memset((void *)XBOX_PTR(TEST_distributed_predictions),0,8704);memset((void *)XBOX_PTR(TEST_distributed_statistics),0,12);memset((void *)XBOX_PTR(TEST_distributed_received_times),0xff,8772);
/* Actual protocol4 batch:8-byte outer header,2-byte child length, child
payload excludes its2-byte generic header. Prediction entry is64 bytes. */
uint32_t packet=0x60001000;memset((void *)XBOX_PTR(packet),0,80);MEM16(packet)=((80<<4)|8);MEM8(packet+2)=16;MEM32(packet+4)=100;
MEM16(packet+8)=70;MEM8(packet+10)=1;MEM8(packet+11)=1;MEM32(packet+12)=100;MEM8(packet+16)=1;MEM8(packet+17)=1;
MEMF(packet+32)=12.5f;MEMF(packet+36)=-3.25f;MEMF(packet+40)=0.125f;
decode(80);uint32_t prediction=TEST_distributed_predictions+68;assert(MEM8(prediction)==1);assert(MEMF(prediction+20)==12.5f && MEMF(prediction+24)==-3.25f);assert(MEM32(TEST_distributed_statistics+4)==1);
MEM8(prediction)=0;MEM32(packet+12)=99;decode(80);assert(MEM8(prediction)==0);
MEM32(packet+12)=101;MEM16(packet+8)=71;decode(80);assert(MEM8(prediction)==0);
MEM16(packet+8)=70;MEM8(packet+16)=2;decode(80);assert(MEM8(prediction)==0);
MEM8(packet+16)=1;MEM32(packet+12)=102;decode(80);assert(MEM8(prediction)==1);
puts("Actual translated protocol4 batch/prediction decoder PASS: valid position, stale tick, malformed length, machine ownership, next valid tick");return 0;}
