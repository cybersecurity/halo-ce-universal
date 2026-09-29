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
#include "host_services.h"
#include "host_imports.h"
#include "posix_host_imports.h"
#include "guest_errno.h"
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
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
extern void TEST___get_tp(void);
recomp_func_t recomp_lookup(uint32_t va) { return va == VA___get_tp ? TEST___get_tp : NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return NULL; }

int fixture_file_errors;
uint32_t mac_guest_host_import_esp(void){return g_esp;}
uint32_t mac_guest_host_import_arg32(uint32_t i){return MEM32(g_esp+4+i*4);}
uint64_t mac_guest_host_import_arg64_words(uint32_t i){return (uint64_t)mac_guest_host_import_arg32(i)|((uint64_t)mac_guest_host_import_arg32(i+1)<<32);}
void mac_guest_host_import_return32(uint32_t value){g_eax=value;g_esp+=4;}
void mac_guest_host_import_return64(int64_t value){g_edx=(uint64_t)value>>32;mac_guest_host_import_return32(value);}
void mac_guest_host_import_return_void(void){g_esp+=4;}
void mac_guest_import_dispatch_token(uint32_t token){if(token==TOKEN_host_get_tp){mac_guest_host_import_return32(mac_host_get_guest_tp());return;}if(token==TOKEN_posix_stat){mac_macos_posix_host_import_bridge("posix_stat")();return;}unexpected(token);}
static uint32_t call(void(*f)(void),uint32_t arg,unsigned popped){mac_recomp_state_initialize_thread(0x6000f000,0x60002000);MEM32(g_esp)=0;MEM32(g_esp+4)=arg;f();assert(g_esp==0x6000f004+popped);return g_eax;}
struct thread_case {uint32_t tp,stack,seed;int error;};
static void *errno_thread(void *opaque){
 struct thread_case *c=opaque;unsigned char scratch[256];
 mac_host_set_guest_tp(c->tp);mac_recomp_state_initialize_thread(c->stack,c->tp);
 memset(scratch,(int)c->seed,sizeof(scratch));memcpy((void *)XBOX_PTR(c->stack-256),scratch,sizeof(scratch));
 g_eax=c->seed;g_edx=c->seed+1;g_ebx=c->seed+2;g_fp_stack[0]=3.125;g_xmm0.q[0]=0x1122334455667788ULL;g_mm7.q=0x8877665544332211ULL;
 errno=EACCES;assert(mac_guest_errno_set_native(c->error)==0);
 assert(MEM32(c->tp+28)==(uint32_t)mac_host_linux_errno(c->error));
 assert(g_eax==c->seed&&g_edx==c->seed+1&&g_ebx==c->seed+2&&g_esp==c->stack&&g_fp_stack[0]==3.125&&g_xmm0.q[0]==0x1122334455667788ULL&&g_mm7.q==0x8877665544332211ULL&&errno==EACCES);
 assert(!memcmp((void *)XBOX_PTR(c->stack-256),scratch,sizeof(scratch)));
 for(unsigned i=0;i<100;i++){assert(mac_guest_errno_set_native(c->error)==0);assert(MEM32(c->tp+28)==(uint32_t)mac_host_linux_errno(c->error));}
 return NULL;
}
int main(int argc,char **argv){assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);void *bytes=malloc(n);assert(fread(bytes,1,n,f)==(size_t)n);fclose(f);
struct mac_guest_image image;char error[256];assert(mac_guest_image_load(bytes,n,&image,error,sizeof(error))==0);assert(mac_guest_address_commit(0x60000000,0x10000,PROT_READ|PROT_WRITE)==0);assert(mac_recomp_state_set_image(image.memory_offset,image.code_lo_va,image.code_hi_va)==0);
mac_host_set_guest_tp(0x60002000);uint32_t error_address=call(TEST___errno_location,0,0);assert(error_address==0x6000201c);
strcpy((char *)XBOX_PTR(0x60005000),"/tmp/halo-errno-fixture-missing-unique");memset((void *)XBOX_PTR(0x60006000),0,300);strcpy((char *)XBOX_PTR(0x60006008),(char *)XBOX_PTR(0x60005000));
/* Reproduce stale guest errno before installing the native writer. */
MEM32(error_address)=9;assert(call(TEST_GetFileAttributesA,0x60005000,4)==UINT32_MAX);assert(MEM32(error_address)==9);assert(call(TEST_GetLastError,0,0)==6);fixture_file_errors=0;assert((call(TEST_file_exists,0x60006000,0)&255)==0&&fixture_file_errors==1);
assert(mac_guest_errno_install(TEST___errno_location)==0);MEM32(error_address)=9;assert(call(TEST_GetFileAttributesA,0x60005000,4)==UINT32_MAX);assert(MEM32(error_address)==2);assert(call(TEST_GetLastError,0,0)==2);fixture_file_errors=0;assert((call(TEST_file_exists,0x60006000,0)&255)==0&&fixture_file_errors==0);
mac_recomp_state_initialize_thread(0x6000f000,0x60002000);g_eax=11;g_ecx=22;g_edx=33;g_ebp=44;g_seh_ebp=55;errno=EBADF;assert(mac_guest_errno_set_native(EAGAIN)==0);assert(MEM32(error_address)==11&&errno==EBADF&&g_eax==11&&g_ecx==22&&g_edx==33&&g_ebp==44&&g_seh_ebp==55&&g_esp==0x6000f000);
mac_host_set_guest_tp(0x60003000);assert(mac_guest_errno_set_native(ENOENT)==0);assert(MEM32(0x6000301c)==2&&MEM32(error_address)==11);mac_host_set_guest_tp(0);assert(mac_guest_errno_set_native(EBADF)==-1);assert(MEM32(0x6000301c)==2);
pthread_t threads[2];struct thread_case cases[]={{0x60004000,0x6000b000,71,ENOENT},{0x60007000,0x6000d000,83,ENOTDIR}};
for(unsigned i=0;i<2;i++)assert(!pthread_create(&threads[i],NULL,errno_thread,&cases[i]));for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));assert(MEM32(0x6000401c)==2&&MEM32(0x6000701c)==20);
puts("Actual translated FileExists/native stat errno PASS: stale9 reproduced, missing2 corrected, no spurious file_error, Darwin EAGAIN→Linux11, register/stack/native errno preserved, two concurrent guest threads/TLS switch/init guard");return 0;}
