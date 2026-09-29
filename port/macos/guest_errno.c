#include "guest_errno.h"
#include "guest_address.h"
#include "guest_call.h"
#include "host_services.h"
#ifndef RECOMP_GENERATED_CODE
#define RECOMP_GENERATED_CODE 1
#endif
#include "recomp_types.h"
#include <errno.h>
#include <string.h>

static void (*errno_accessor)(void);
static _Thread_local uint32_t cached_tp, cached_address;
static _Thread_local int discovering;

int mac_guest_errno_install(void (*accessor)(void))
{
 if (!accessor || errno_accessor) return -1;
 errno_accessor = accessor;
 return 0;
}

int mac_guest_errno_set_native(int native_error)
{
 uint32_t tp = mac_host_get_guest_tp();
 int native_saved = errno, result = -1;
 if (!tp || !errno_accessor || discovering) {errno=native_saved;return -1;}
 if (cached_tp != tp || !mac_guest_address_resolve(cached_address, sizeof(int32_t)))
 {
  /* The accessor is a real translated cdecl function. Preserve its bounded
   * scratch stack and every live guest register, including x87/MMX/SSE. */
  uint32_t general[] = {g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp};
  uint32_t saved_seh=g_seh_ebp, saved_fs=g_fs_base, saved_mxcsr=g_mxcsr;
  double saved_fp[8]; memcpy(saved_fp,g_fp_stack,sizeof(saved_fp));
  RecompXmm saved_xmm[]={g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7};
  RecompMmx saved_mm[]={g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7};
  int saved_top=g_fp_top,saved_df=g_df,saved_cmp=g_fp_cmp;
  uint16_t saved_control=g_fp_control_word,saved_status=g_fp_status_word;
  uint8_t saved_empty=g_fp_empty_mask;
  unsigned char scratch[256];
  uint32_t original_stack=general[3], frame, address=0, sentinel=UINT32_MAX;
  if (original_stack < sizeof(scratch) || mac_guest_read(original_stack-sizeof(scratch),scratch,sizeof(scratch))) {errno=native_saved;return -1;}
  frame=((original_stack-32u)&~UINT32_C(15))+12u;
  if (mac_guest_write(frame,&sentinel,sizeof(sentinel))) {errno=native_saved;return -1;}
  discovering=1; g_esp=frame; errno_accessor();
  if (g_esp==frame+4 && mac_host_get_guest_tp()==tp) address=g_eax;
  discovering=0;
  (void)mac_guest_write(original_stack-sizeof(scratch),scratch,sizeof(scratch));
  g_eax=general[0];g_ecx=general[1];g_edx=general[2];g_esp=general[3];
  g_ebx=general[4];g_esi=general[5];g_edi=general[6];g_ebp=general[7];
  g_seh_ebp=saved_seh;g_fs_base=saved_fs;g_mxcsr=saved_mxcsr;
  memcpy(g_fp_stack,saved_fp,sizeof(saved_fp));g_fp_top=saved_top;g_df=saved_df;g_fp_cmp=saved_cmp;
  g_fp_control_word=saved_control;g_fp_status_word=saved_status;g_fp_empty_mask=saved_empty;
  g_xmm0=saved_xmm[0];g_xmm1=saved_xmm[1];g_xmm2=saved_xmm[2];g_xmm3=saved_xmm[3];
  g_xmm4=saved_xmm[4];g_xmm5=saved_xmm[5];g_xmm6=saved_xmm[6];g_xmm7=saved_xmm[7];
  g_mm0=saved_mm[0];g_mm1=saved_mm[1];g_mm2=saved_mm[2];g_mm3=saved_mm[3];
  g_mm4=saved_mm[4];g_mm5=saved_mm[5];g_mm6=saved_mm[6];g_mm7=saved_mm[7];
  if (!address || !mac_guest_address_resolve(address,sizeof(int32_t))) {errno=native_saved;return -1;}
  cached_tp=tp;cached_address=address;
 }
 int32_t linux_error=mac_host_linux_errno(native_error);
 result=mac_guest_write(cached_address,&linux_error,sizeof(linux_error));
 errno=native_saved;
 return result;
}
