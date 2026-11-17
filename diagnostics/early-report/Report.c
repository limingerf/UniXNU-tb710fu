/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Terminal early-XNU report. Captures no invented console lines. The immutable
 * report is one OneSend LMS1 envelope / OS frame. QR encoder is upstream Mu.
 */
#include "Report.h"
#include <Library/QrEncoderLib.h>
#include "Font.h"

#ifndef DIAG_BUILD_LABEL
#define DIAG_BUILD_LABEL "source-unbound"
#endif

#ifdef TB_REPORT_HOST_TEST
TB_REPORT diag_state = {0}; /* Native host format may be Mach-O, not ELF. */
#else
TB_REPORT diag_state __attribute__((section(".data.state"))) = {0};
#endif
UINT8 diag_stack[16384] __attribute__((aligned(16)));
static UINT8 arena[524288];
static UINTN arena_used;
static UINT8 frame[752];
static CHAR8 text[620];
static UINTN text_used;
UINT8 *diag_test_frame = frame;
CHAR8 *diag_test_text = text;
UINTN diag_test_text_length;
UINTN diag_test_heap_used;
EFI_GRAPHICS_OUTPUT_BLT_PIXEL *diag_test_bitmap;
EFI_STATUS diag_test_qr_status;
UINT64 diag_report_phase;
UINT64 diag_last_stage;
UINT64 diag_live_inputs[3]; /* Original x0, x1, and kernel SP. */
UINT64 diag_live_phase;
UINT64 diag_export_mode;
UINT64 diag_live_controls[4]; /* Original SCTLR, TCR, TTBR0, TTBR1. */
UINT64 diag_guard_status;
UINT64 diag_guard_saved_tpidr_el1,diag_guard_saved_tpidrro_el0;
volatile UINT64 diag_guard_test_active;
volatile UINT64 diag_guard_test_seen;
volatile UINT64 diag_replay_draws;
UINT64 diag_replay_ready;
UINT8 diag_replay_frames[4][752];
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL diag_replay_pixels[4][121*121];
static CHAR8 last_message[112];
static TB_REPORT first_state;

VOID *memcpy(VOID *o, CONST VOID *v, UINTN n) {
  UINT8 *a=o; CONST UINT8 *b=v;
  for (UINTN i=0;i<n;i++) a[i]=b[i];
  return o;
}
VOID *memset(VOID *o,int c,UINTN n) {
  UINT8 *a=o;for(UINTN i=0;i<n;i++)a[i]=(UINT8)c;return o;
}
VOID *CopyMem(VOID *o,CONST VOID *i,UINTN n){return memcpy(o,i,n);}
VOID *SetMem32(VOID *o,UINTN n,UINT32 v){UINT32 *p=o;for(UINTN i=0;i<n/4;i++)p[i]=v;return o;}
VOID *AllocatePool(UINTN n) {
  n=(n+15)&~(UINTN)15;
  if(n>sizeof(arena)-arena_used)return NULL;
  VOID *p=arena+arena_used;arena_used+=n;return p;
}
VOID *AllocateZeroPool(UINTN n){VOID *p=AllocatePool(n);if(p)memset(p,0,n);return p;}
VOID *AllocateCopyPool(UINTN n,CONST VOID *d){VOID *p=AllocatePool(n);if(p)memcpy(p,d,n);return p;}
VOID FreePool(VOID *p){(VOID)p;} /* Arena reset before each serialized snapshot. */

static VOID append(CONST CHAR8 *p){while(*p&&text_used<sizeof(text)-1)text[text_used++]=*p++;text[text_used]=0;}
static VOID append_char(CHAR8 c){CHAR8 value[2]={c,0};append(value);}
static VOID append_build_label(VOID){append("build ");append(DIAG_BUILD_LABEL);append("\n");}
static VOID number(UINT64 v){CONST CHAR8 h[]="0123456789abcdef";for(int s=60;s>=0;s-=4){CHAR8 b[2]={h[(v>>s)&15],0};append(b);}}
static VOID number48(UINT64 v){CONST CHAR8 h[]="0123456789abcdef";for(int s=44;s>=0;s-=4){CHAR8 b[2]={h[(v>>s)&15],0};append(b);}}
static VOID decimal(UINTN v){CHAR8 digits[24];UINTN n=0;do{digits[n++]=(CHAR8)('0'+v%10);v/=10;}while(v&&n<sizeof(digits));while(n)append_char(digits[--n]);}
static VOID field(CONST CHAR8 *name,UINT64 v){append(name);append("=");number(v);append("\n");}
static UINT32 crc(CONST UINT8 *p,UINTN n){UINT32 c=~0U;for(UINTN i=0;i<n;i++){c^=p[i];for(int k=0;k<8;k++)c=(c>>1)^((0U-(c&1U))&0xedb88320U);}return ~c;}
static VOID le16(UINT8 *p,UINT16 v){p[0]=v;p[1]=v>>8;}
static VOID le32(UINT8 *p,UINT32 v){for(int i=0;i<4;i++)p[i]=v>>(i*8);}

static VOID make_frame(BOOLEAN live) {
  CONST CHAR8 *name=live?"TB710FU-live-stage.log":"TB710FU-early-fault.log";
  CONST CHAR8 mime[]="text/plain";
  UINT8 envelope[720];UINTN nl=0,ml=sizeof(mime)-1;
  while(name[nl])nl++;
  UINTN total=26+nl+ml+text_used;
  memset(envelope,0,sizeof(envelope));memset(frame,0,sizeof(frame));
  memcpy(envelope,"LMS1",4);envelope[4]=2;
  le16(envelope+6,nl);le16(envelope+8,ml);
  le32(envelope+10,text_used);le32(envelope+14,crc((UINT8*)text,text_used));
  le32(envelope+18,text_used);le32(envelope+22,crc((UINT8*)text,text_used));
  memcpy(envelope+26,name,nl);memcpy(envelope+26+nl,mime,ml);
  memcpy(envelope+26+nl+ml,text,text_used);
  frame[0]='O';frame[1]='S';frame[2]=2;
  le32(frame+4,crc(envelope,total)^0xb710f001U);le32(frame+8,0);
  le32(frame+12,1);le16(frame+16,720);le32(frame+20,total);
  le32(frame+24,crc(envelope,total));memcpy(frame+28,envelope,total);
  le32(frame+748,crc(frame,748));
}

static VOID pixel(TB_REPORT *s,UINTN x,UINTN y,UINT32 c){
  if(x<s->Width&&y<s->Height)*(volatile UINT32*)(UINTN)(s->Framebuffer+y*s->RowBytes+x*4)=c;
}
static VOID draw_text(TB_REPORT *s,CONST CHAR8 *t,UINTN x,UINTN y,UINTN scale){
  UINTN origin=x;
  while(*t){UINT8 c=(UINT8)*t++;
    if(c=='\n'){x=origin;y+=21*scale;continue;}
    if(c<32||c>126)c='?';
    if(x+8*scale>=s->Width){x=origin;y+=21*scale;}
    for(UINTN r=0;r<19;r++)for(UINTN col=0;col<8;col++)
      for(UINTN yy=0;yy<scale;yy++)for(UINTN xx=0;xx<scale;xx++)
        pixel(s,x+col*scale+xx,y+r*scale+yy,(mEarlyFont[c-32][r]&(128>>col))?0xffeeeeee:0xff000000);
    x+=8*scale;
  }
}

BOOLEAN diag_walk_t1_4k(UINT64 root_pa,UINT64 tcr,UINT64 va,
                        DIAG_READ_DESCRIPTOR read_descriptor,VOID *context,
                        DIAG_WALK_RESULT *result){
  if(!result)return FALSE;
  memset(result,0,sizeof(*result));result->Status=DIAG_WALK_UNSUPPORTED;
  if(!read_descriptor||((tcr>>16)&0x3f)!=26||((tcr>>30)&3)!=2||
     (va>>38)!=0x3ffffffULL)return FALSE;
  UINT64 root=root_pa&0x0000fffffffff000ULL;
  UINT64 l1_pa=root+(((va>>30)&0xff)*8);
  if(!read_descriptor(context,l1_pa,&result->L1Descriptor)){
    result->Status=DIAG_WALK_READ_FAILED;return FALSE;
  }
  UINT64 l1=result->L1Descriptor;
  BOOLEAN table_writable=TRUE,table_executable=TRUE;
  if((l1&3)==1){
    result->Level=1;result->Physical=(l1&0x0000ffffc0000000ULL)|(va&0x3fffffff);
  }else if((l1&3)==3){
    table_writable=(l1&(1ULL<<61))==0;
    table_executable=(l1&(1ULL<<59))==0;
    UINT64 l2_table=l1&0x0000fffffffff000ULL;
    UINT64 l2_pa=l2_table+(((va>>21)&0x1ff)*8);
    if(!read_descriptor(context,l2_pa,&result->L2Descriptor)){
      result->Status=DIAG_WALK_READ_FAILED;return FALSE;
    }
    UINT64 l2=result->L2Descriptor;
    if((l2&3)==1){
      result->Level=2;result->Physical=(l2&0x0000ffffffe00000ULL)|(va&0x1fffff);
    }else if((l2&3)==3){
      table_writable=table_writable&&(l2&(1ULL<<61))==0;
      table_executable=table_executable&&(l2&(1ULL<<59))==0;
      UINT64 l3_table=l2&0x0000fffffffff000ULL;
      UINT64 l3_pa=l3_table+(((va>>12)&0x1ff)*8);
      if(!read_descriptor(context,l3_pa,&result->L3Descriptor)){
        result->Status=DIAG_WALK_READ_FAILED;return FALSE;
      }
      UINT64 l3=result->L3Descriptor;
      if((l3&3)!=3){result->Status=DIAG_WALK_INVALID_L3;return FALSE;}
      result->Level=3;result->Physical=(l3&0x0000fffffffff000ULL)|(va&0xfff);
    }else{result->Status=DIAG_WALK_INVALID_L2;return FALSE;}
  }else{result->Status=DIAG_WALK_INVALID_L1;return FALSE;}
  UINT64 leaf=result->Level==1?l1:(result->Level==2?result->L2Descriptor:result->L3Descriptor);
  result->Writable=table_writable&&(leaf&(1ULL<<7))==0;
  result->Executable=table_executable&&(leaf&(1ULL<<53))==0;
  result->AccessFlag=(leaf&(1ULL<<10))!=0;
  result->Status=DIAG_WALK_OK;
  return TRUE;
}

#ifndef TB_REPORT_HOST_TEST
extern UINT8 diag_image_base[] __attribute__((visibility("hidden")));
extern UINT8 diag_target_switch[] __attribute__((visibility("hidden")));
extern UINT8 diag_target_kernel_resume[] __attribute__((visibility("hidden")));
extern UINT8 diag_target_kernel_vectors[] __attribute__((visibility("hidden")));
extern UINT8 diag_target_report_resume[] __attribute__((visibility("hidden")));
extern UINT8 diag_target_set_mmu_ttb[] __attribute__((visibility("hidden")));
extern UINT8 diag_code_end[] __attribute__((visibility("hidden")));
extern UINT8 diag_data_start[] __attribute__((visibility("hidden")));
extern UINT8 diag_image_end[] __attribute__((visibility("hidden")));
extern UINT8 diag_stack_end[] __attribute__((visibility("hidden")));
extern UINT8 diag_vectors[] __attribute__((visibility("hidden")));
#define DIAG_SWITCH_MARKER_PA 0x93ffff000ULL
static UINT64 diag_guard_l1[512] __attribute__((aligned(4096)));
static UINT64 diag_guard_l2[1024] __attribute__((aligned(4096)));
static UINT64 diag_guard_l3[1024] __attribute__((aligned(4096)));
static UINT64 diag_guard_root_pa,diag_guard_old_ttbr0,diag_guard_old_tcr,diag_guard_old_vbar;
static BOOLEAN diag_guard_active;

static BOOLEAN diag_read_descriptor(VOID *context,UINT64 pa,UINT64 *value){
  TB_REPORT *s=context;
  if(!s||!value||s->WindowBytes<8||pa<s->PhysicalBase||pa-s->PhysicalBase>s->WindowBytes-8)return FALSE;
  UINT64 va=s->VirtualBase+(pa-s->PhysicalBase),par;
  __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1"
                   : "=r"(par) : "r"(va) : "memory");
  if((par&1)||(par&0x0000fffffffff000ULL)!=(pa&~0xfffULL))return FALSE;
  *value=*(volatile CONST UINT64*)(UINTN)va;
  return TRUE;
}

static CHAR8 diag_check_mapping(TB_REPORT *s,UINT64 root,UINT64 tcr,UINT64 va,
                                BOOLEAN executable,BOOLEAN writable,
                                DIAG_WALK_RESULT *result){
  memset(result,0,sizeof(*result));result->Status=DIAG_WALK_UNSUPPORTED;
  if(!s->WindowBytes||va<s->VirtualBase||va-s->VirtualBase>=s->WindowBytes)return 'O';
  if(!diag_walk_t1_4k(root,tcr,va,diag_read_descriptor,s,result)){
    if(result->Status==DIAG_WALK_READ_FAILED)return 'R';
    if(result->Status==DIAG_WALK_INVALID_L1||result->Status==DIAG_WALK_INVALID_L2||
       result->Status==DIAG_WALK_INVALID_L3)return 'N';
    return 'U';
  }
  UINT64 expected=s->PhysicalBase+(va-s->VirtualBase);
  if(result->Physical!=expected)return 'P';
  if(!result->AccessFlag||result->Executable!=executable||result->Writable!=writable)return 'M';
  return '+';
}

static UINT64 diag_guard_physical(TB_REPORT *s,UINT64 va){
  if(va<(UINT64)(UINTN)diag_image_base)return 0;
  return s->PayloadPa+(va-(UINT64)(UINTN)diag_image_base);
}
static UINT64 diag_guard_page_descriptor(UINT64 source,UINT64 pa,
                                         BOOLEAN executable,BOOLEAN writable);

static BOOLEAN diag_guard_copy_range(TB_REPORT *s,UINT64 root,UINT64 tcr,
                                     UINT64 start,UINT64 end,
                                     UINT64 physical_start,UINTN table_group,
                                     BOOLEAN executable,BOOLEAN writable){
  start&=~0xfffULL;end=(end+0xfff)&~0xfffULL;
  if(table_group>1)return FALSE;
  for(UINT64 va=start;va<end;va+=0x1000){
    DIAG_WALK_RESULT result;
    if(!diag_walk_t1_4k(root,tcr,va,diag_read_descriptor,s,&result)||
       result.Level<1||result.Level>3||!result.AccessFlag)return FALSE;
    UINT64 expected=physical_start+(va-start);
    if(result.Physical!=expected||(executable&&!result.Executable)||
       (writable&&!result.Writable))return FALSE;
    UINT64 source=result.Level==1?result.L1Descriptor:
                  result.Level==2?result.L2Descriptor:result.L3Descriptor;
    if(result.Physical!=expected)return FALSE;
    UINT64 descriptor=diag_guard_page_descriptor(source,expected,executable,writable);
    UINTN index=table_group*512+(UINTN)((expected>>12)&0x1ff);
    if(diag_guard_l3[index]&&diag_guard_l3[index]!=descriptor)return FALSE;
    diag_guard_l3[index]=descriptor;
  }
  return TRUE;
}

static VOID diag_guard_clean_table(volatile UINT64 *table){
  UINT64 ctr,line;
  __asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));line=4ULL<<((ctr>>16)&15);
  for(UINT64 at=(UINT64)(UINTN)table;at<(UINT64)(UINTN)table+4096;at+=line)
    __asm__ volatile("dc cvac,%0"::"r"(at):"memory");
}

static UINT64 diag_guard_page_descriptor(UINT64 source,UINT64 pa,
                                         BOOLEAN executable,BOOLEAN writable){
  CONST UINT64 attributes=(0x7ULL<<2)|(1ULL<<5)|(3ULL<<8)|
                          (1ULL<<10)|(1ULL<<11)|(1ULL<<50);
  UINT64 permission=writable?0:(1ULL<<7);
  UINT64 execute_disable=(executable?0:(1ULL<<53))|(1ULL<<54);
  return (pa&0x0000fffffffff000ULL)|(source&attributes)|permission|execute_disable|3;
}

static BOOLEAN diag_guard_prepare(TB_REPORT *s,UINT64 source_root,UINT64 tcr){
  diag_guard_status=3;
  UINT64 t0sz=tcr&0x3f,tg0=(tcr>>14)&3;
  UINT64 image=(UINT64)(UINTN)diag_image_base;
  UINT64 code_end=(UINT64)(UINTN)diag_data_start;
  UINT64 data_start=(UINT64)(UINTN)diag_data_start;
  UINT64 data_end=((UINT64)(UINTN)diag_image_end+0x3fff)&~0x3fffULL;
  if(t0sz!=26||tg0!=0||data_start!=code_end||code_end<=image||data_end<=data_start||
     !s->PayloadPa||data_end-image>0x100000)return FALSE;
  UINT64 pa_start=s->PayloadPa,pa_end=pa_start+(data_end-image);
  UINT64 marker=DIAG_SWITCH_MARKER_PA;
  if(marker<s->PhysicalBase||marker-s->PhysicalBase+0x1000>s->WindowBytes)return FALSE;
  UINT64 marker_va=s->VirtualBase+(marker-s->PhysicalBase);
  if(pa_end<=pa_start||pa_end>(1ULL<<38)||
     (pa_start>>30)!=((pa_end-1)>>30)||(pa_start>>21)!=((pa_end-1)>>21))return FALSE;
  UINTN image_l1=(UINTN)((pa_start>>30)&0xff),marker_l1=(UINTN)((marker>>30)&0xff);
  UINTN framebuffer_l1=(UINTN)((s->Framebuffer>>30)&0xff);
  if(image_l1==marker_l1||
     (s->Framebuffer&&(image_l1==framebuffer_l1||marker_l1==framebuffer_l1)))return FALSE;
  memset(diag_guard_l1,0,sizeof(diag_guard_l1));
  memset(diag_guard_l2,0,sizeof(diag_guard_l2));
  memset(diag_guard_l3,0,sizeof(diag_guard_l3));
  diag_guard_root_pa=diag_guard_physical(s,(UINT64)(UINTN)diag_guard_l1);
  UINT64 l2_pa=diag_guard_physical(s,(UINT64)(UINTN)diag_guard_l2);
  UINT64 l3_pa=diag_guard_physical(s,(UINT64)(UINTN)diag_guard_l3);
  if((diag_guard_root_pa|l2_pa|l3_pa)&0xfff)return FALSE;
  diag_guard_l1[image_l1]=(l2_pa&0x0000fffffffff000ULL)|3;
  diag_guard_l1[marker_l1]=((l2_pa+0x1000)&0x0000fffffffff000ULL)|3;
  diag_guard_l2[(pa_start>>21)&0x1ff]=(l3_pa&0x0000fffffffff000ULL)|3;
  diag_guard_l2[512+((marker>>21)&0x1ff)]=((l3_pa+0x1000)&0x0000fffffffff000ULL)|3;
  if(!source_root||
     !diag_guard_copy_range(s,source_root,tcr,image,code_end,pa_start,0,TRUE,FALSE)||
     !diag_guard_copy_range(s,source_root,tcr,data_start,data_end,
                            pa_start+(data_start-image),0,FALSE,TRUE)||
     !diag_guard_copy_range(s,source_root,tcr,marker_va,marker_va+0x1000,
                            marker,1,FALSE,TRUE))return FALSE;
  diag_guard_clean_table(diag_guard_l1);
  diag_guard_clean_table(diag_guard_l2);
  diag_guard_clean_table(diag_guard_l2+512);
  diag_guard_clean_table(diag_guard_l3);
  diag_guard_clean_table(diag_guard_l3+512);
  __asm__ volatile("dsb sy" ::: "memory");
  diag_guard_status=1;
  return TRUE;
}

static BOOLEAN diag_guard_translate(UINT64 va,UINT64 expected,BOOLEAN writable){
  UINT64 par;
  if(writable)
    __asm__ volatile("at s1e1w,%1\n\tisb\n\tmrs %0,par_el1":"=r"(par):"r"(va):"memory");
  else
    __asm__ volatile("at s1e1r,%1\n\tisb\n\tmrs %0,par_el1":"=r"(par):"r"(va):"memory");
  return !(par&1)&&(par&0x0000fffffffff000ULL)==(expected&~0xfffULL);
}

static VOID diag_guard_restore_roots(VOID){
  __asm__ volatile("dsb sy\nmsr ttbr0_el1,%0\nmsr tcr_el1,%1\nisb\ntlbi vmalle1\ndsb sy\nisb"
                   ::"r"(diag_guard_old_ttbr0),"r"(diag_guard_old_tcr):"memory");
}

BOOLEAN diag_guard_activate(VOID){
  if(diag_guard_status!=1||!diag_guard_root_pa)return FALSE;
  __asm__ volatile("mrs %0,ttbr0_el1\nmrs %1,tcr_el1\nmrs %2,vbar_el1"
                   :"=r"(diag_guard_old_ttbr0),"=r"(diag_guard_old_tcr),"=r"(diag_guard_old_vbar));
  __asm__ volatile("dsb sy\nmsr ttbr0_el1,%0\nisb\ntlbi vmalle1\ndsb sy\nisb"
                   ::"r"(diag_guard_root_pa):"memory");
  UINT64 vector=diag_guard_physical(&diag_state,(UINT64)(UINTN)diag_vectors);
  UINT64 state=diag_guard_physical(&diag_state,(UINT64)(UINTN)&diag_state);
  UINT64 stack=diag_guard_physical(&diag_state,(UINT64)(UINTN)diag_stack_end-16);
  if((vector&0x7ff)||
     !diag_guard_translate(vector,vector,0)||
     !diag_guard_translate((UINT64)(UINTN)diag_target_report_resume,
                           diag_guard_physical(&diag_state,(UINT64)(UINTN)diag_target_report_resume),0)||
     !diag_guard_translate(state,state,TRUE)||!diag_guard_translate(stack,stack,TRUE)||
     !diag_guard_translate(DIAG_SWITCH_MARKER_PA,DIAG_SWITCH_MARKER_PA,TRUE)){
    diag_guard_restore_roots();diag_guard_status=3;return FALSE;
  }
  __asm__ volatile("msr vbar_el1,%0\nisb"::"r"(vector):"memory");
  __asm__ volatile("mrs %0,tpidr_el1\nmrs %1,tpidrro_el0"
                   :"=r"(diag_guard_saved_tpidr_el1),"=r"(diag_guard_saved_tpidrro_el0));
  diag_guard_test_seen=0;diag_guard_test_active=1;
  __asm__ volatile("brk #0xb712" ::: "x16","x17","memory");
  diag_guard_test_active=0;
  __asm__ volatile("msr tpidr_el1,%0\nmsr tpidrro_el0,%1"
                   ::"r"(diag_guard_saved_tpidr_el1),"r"(diag_guard_saved_tpidrro_el0):"memory");
  if(diag_guard_test_seen!=1){
    __asm__ volatile("msr vbar_el1,%0\nisb"::"r"(diag_guard_old_vbar):"memory");
    diag_guard_restore_roots();diag_guard_status=3;return FALSE;
  }
  diag_guard_active=TRUE;diag_guard_status=2;
  return TRUE;
}

VOID diag_guard_deactivate(VOID){
  if(!diag_guard_active)return;
  diag_guard_restore_roots();diag_guard_active=FALSE;
}

static VOID append_map_record(TB_REPORT *s,UINT64 root,UINT64 tcr,CHAR8 key,
                              UINT64 va,BOOLEAN executable,BOOLEAN writable){
  DIAG_WALK_RESULT result;
  CHAR8 status=diag_check_mapping(s,root,tcr,va,executable,writable,&result);
  append_char(key);append("=");number48(va);number(result.L1Descriptor);
  number(result.L2Descriptor);number(result.L3Descriptor);append_char(status);append("\n");
}

static UINTN diag_scan_range(TB_REPORT *s,UINT64 root,UINT64 tcr,UINT64 start,
                             UINT64 end,BOOLEAN executable,BOOLEAN writable,
                             UINT64 *first_bad,CHAR8 *first_status,
                             DIAG_WALK_RESULT *bad_result){
  UINTN good=0;
  start&=~0xfffULL;end=(end+0xfff)&~0xfffULL;
  for(UINT64 va=start;va<end;va+=0x1000){
    DIAG_WALK_RESULT result;
    CHAR8 status=diag_check_mapping(s,root,tcr,va,executable,writable,&result);
    if(status=='+')good++;
    else if(first_bad&&!*first_bad){
      *first_bad=va;if(first_status)*first_status=status;
      if(bad_result)*bad_result=result;
    }
  }
  return good;
}

static VOID append_range_result(TB_REPORT *s,UINT64 root,UINT64 tcr,CHAR8 key,
                                UINT64 start,UINT64 end,BOOLEAN executable,
                                BOOLEAN writable,BOOLEAN *path_reported){
  UINT64 first_bad=0;CHAR8 first_status=0;DIAG_WALK_RESULT result;
  UINTN pages=(UINTN)(((end+0xfff)&~0xfffULL)-(start&~0xfffULL))/0x1000;
  UINTN good=diag_scan_range(s,root,tcr,start,end,executable,writable,
                             &first_bad,&first_status,&result);
  append_char(key);append("=");decimal(good);append("/");decimal(pages);append("\n");
  if(first_bad){
    if(!*path_reported){
      append_char(key);append("!");number48(first_bad);number(result.L1Descriptor);
      number(result.L2Descriptor);number(result.L3Descriptor);
      append_char(first_status);append("\n");*path_reported=TRUE;
    }
  }
}

static VOID append_page_table_snapshot(TB_REPORT *s){
  UINT64 root=diag_live_inputs[0],tcr=diag_live_controls[1];
  diag_guard_prepare(s,diag_live_controls[3],tcr);
  if(diag_guard_status==1)diag_guard_activate();
  append(diag_export_mode?"XNU TTBR1 MAP SNAPSHOT (export pause)\n":
                          "XNU TTBR1 MAP SNAPSHOT\n");append_build_label();
  append("STAGE=47\n");
  append("ROOT=");number48(root);append("\n");append("T=");number(tcr);append("\n");
  append("O0=");number48(diag_live_controls[2]);append("\n");
  append("O1=");number48(diag_live_controls[3]);append("\n");
  append("G=");decimal(diag_guard_status);append("\n");
  append_map_record(s,root,tcr,'H',(UINT64)(UINTN)diag_target_switch,TRUE,FALSE);
  append_map_record(s,root,tcr,'R',(UINT64)(UINTN)diag_target_kernel_resume,TRUE,FALSE);
  append_map_record(s,root,tcr,'S',diag_live_inputs[2],FALSE,TRUE);
  append_map_record(s,root,tcr,'V',(UINT64)(UINTN)diag_target_kernel_vectors,TRUE,FALSE);
  append_map_record(s,root,tcr,'C',(UINT64)(UINTN)diag_target_report_resume,TRUE,FALSE);
  append_map_record(s,root,tcr,'N',(UINT64)(UINTN)diag_target_set_mmu_ttb,TRUE,FALSE);
  BOOLEAN path_reported=FALSE;
  append_range_result(s,root,tcr,'X',(UINT64)(UINTN)diag_image_base,
                      (UINT64)(UINTN)diag_code_end,TRUE,FALSE,&path_reported);
  append_range_result(s,root,tcr,'D',(UINT64)(UINTN)diag_data_start,
                      ((UINT64)(UINTN)diag_image_end+0x3fff)&~0x3fffULL,FALSE,TRUE,&path_reported);
}
#endif

/* This path does not invent an exception. A QR already on screen survives a
 * later spin without needing an exception handler or a watchdog. */
VOID diag_prepare_live_frame(TB_REPORT *s,CONST CHAR8 *message){
  text_used=0;arena_used=0;diag_live_phase=1;
#ifndef TB_REPORT_HOST_TEST
  if(diag_last_stage==47){
    append_page_table_snapshot(s);
  }else{
#endif
  append(diag_export_mode?"XNU VM CHECKPOINT (intentional export pause)\n":"XNU LIVE CHECKPOINT (continues)\n");
  append_build_label();append(message);append("\n");
  field("STAGE",diag_last_stage);field("ARG0",diag_live_inputs[0]);
  field("ARG1",diag_live_inputs[1]);field("KERNEL_SP",diag_live_inputs[2]);
  field("BOOTARGS_PA",s->BootArgsPa);field("FB_PA",s->Framebuffer);
  field("SCTLR",diag_live_controls[0]);field("TCR",diag_live_controls[1]);
  field("OLD_TTBR0",diag_live_controls[2]);field("OLD_TTBR1",diag_live_controls[3]);
  if(diag_guard_status)field("SWITCH_GUARD",diag_guard_status);
  append("last observed stage, not a fault PC or full console log\n");
  if(diag_last_stage==47)append("before marker helper call; QR is captured before the TTBR1 switch\n");
  if(diag_last_stage==48)append("marked switch helper returned after the TTBR1 write and ISB\n");
  if(diag_last_stage==49)append("first post-switch call reached; the marked helper returned\n");
  if(diag_last_stage==54)append("preemption re-enabled; post-mapping checkpoint\n");
  if(diag_last_stage==55)append("arm_vm_init returned; ramoops initialization begins\n");
  if(diag_last_stage==56)append("ramoops initialization returned\n");
  if(diag_last_stage>=57&&diag_last_stage<89)append("pre-scheduler path; CDC worker has not been reached\n");
  if(diag_last_stage==89)append("handing control to machine_startup; BSD/CDC not yet observed\n");
#ifndef TB_REPORT_HOST_TEST
  }
#endif
  diag_test_text_length=text_used;make_frame(TRUE);diag_live_phase=2;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bitmap=NULL;
  diag_test_qr_status=QrEncodeData(QrAutoVersion,QrECLevel_M,QrByteMode,0,frame,sizeof(frame),121,&bitmap);
  diag_test_bitmap=bitmap;diag_test_heap_used=arena_used;diag_live_phase=3;
}

/* One stable file/session, four distinct systematic sequence numbers. For K=1
 * every sequence carries the same source block; no extra fake bytes or logs. */
VOID diag_prepare_replay(TB_REPORT *s,CONST CHAR8 *message){
  diag_replay_ready=0;diag_prepare_live_frame(s,message);
  for(UINTN i=0;i<4;i++){
    if(i){
      arena_used=0;le32(frame+8,(UINT32)i);le32(frame+748,crc(frame,748));
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bitmap=NULL;
      diag_test_qr_status=QrEncodeData(QrAutoVersion,QrECLevel_M,QrByteMode,0,frame,sizeof(frame),121,&bitmap);
      diag_test_bitmap=bitmap;diag_test_heap_used=arena_used;
    }
    if(diag_test_qr_status||!diag_test_bitmap)return;
    memcpy(diag_replay_frames[i],frame,sizeof(frame));
    memcpy(diag_replay_pixels[i],diag_test_bitmap,sizeof(diag_replay_pixels[i]));
    diag_replay_ready++;
  }
}

BOOLEAN diag_export_option(CONST CHAR8 *args){
  CONST CHAR8 option[]="tb-report-export=1";
  for(UINTN i=0;i<1024&&args[i];i++){
    if(i&&args[i-1]!=' ')continue;
    UINTN j=0;while(j<sizeof(option)-1&&i+j<1024&&args[i+j]==option[j])j++;
    if(j==sizeof(option)-1&&i+j<1024&&(!args[i+j]||args[i+j]==' '))return TRUE;
  }
  return FALSE;
}

#ifndef TB_REPORT_HOST_TEST
extern UINT8 diag_image_base[] __attribute__((visibility("hidden")));
static VOID diag_apply_relocations(VOID);
static UINT64 live_tables[3072] __attribute__((aligned(4096)));
static VOID map_gop(UINT64 table_pa,UINT64 tcr){
  __asm__ volatile("dsb sy\nmsr ttbr0_el1,%0\nmsr tcr_el1,%1\nisb\ntlbi vmalle1\ndsb sy\nisb"::"r"(table_pa),"r"(tcr):"memory");
}
static VOID replay_wait(VOID){
  UINT64 frequency,start,now;
  __asm__ volatile("mrs %0,cntfrq_el0\nmrs %1,cntpct_el0":"=r"(frequency),"=r"(start));
  if(!frequency||frequency>10000000000ULL)return;
  do{__asm__ volatile("isb\nmrs %0,cntpct_el0":"=r"(now));}while(now-start<frequency/2);
}
static VOID diag_merge_guard_gop_map(TB_REPORT *s,UINT64 fb){
  if(!diag_guard_active)return;
  UINT64 image_pa=diag_guard_root_pa,marker_pa=DIAG_SWITCH_MARKER_PA;
  UINTN image_l1=(UINTN)((image_pa>>30)&511),marker_l1=(UINTN)((marker_pa>>30)&511);
  UINTN image_l2=(UINTN)((image_pa>>21)&511),marker_l2=(UINTN)((marker_pa>>21)&511);
  UINTN fb_l1=(UINTN)((fb>>30)&511);
  if(image_l1==marker_l1||image_l1==fb_l1||marker_l1==fb_l1)return;
  UINT64 table_pa=s->PayloadPa+(UINT64)(UINTN)live_tables-(UINT64)(UINTN)diag_image_base;
  live_tables[image_l1]=(table_pa+8192)|3;
  live_tables[marker_l1]=(table_pa+12288)|3;
  live_tables[1024+image_l2]=(table_pa+16384)|3;
  live_tables[1536+marker_l2]=(table_pa+20480)|3;
  for(UINTN i=0;i<512;i++){
    live_tables[2048+i]=diag_guard_l3[i];
    live_tables[2560+i]=diag_guard_l3[512+i];
  }
  diag_guard_clean_table(live_tables);
  diag_guard_clean_table(live_tables+1024);
  diag_guard_clean_table(live_tables+1536);
  diag_guard_clean_table(live_tables+2048);
  diag_guard_clean_table(live_tables+2560);
  __asm__ volatile("dsb sy" ::: "memory");
}
/* Encoding and waiting use the original roots. Only short drawing intervals
 * install a temporary TTBR0 GOP mapping, then restore it before every wait. */
VOID diag_live_text(TB_REPORT *s,CONST CHAR8 *message){
  UINTN n=0;while(message[n]&&n<sizeof(last_message)-1){last_message[n]=message[n];n++;}last_message[n]=0;
  if(!diag_last_stage&&s->BootArgsPa>=s->PhysicalBase&&
     s->BootArgsPa-s->PhysicalBase+0x46c<=s->WindowBytes){
    CONST CHAR8 *args=(CONST CHAR8*)(UINTN)(s->BootArgsPa-s->PhysicalBase+s->VirtualBase+0x6c);
    diag_export_mode=diag_export_option(args);
  }
  BOOLEAN qr=diag_last_stage>=47&&diag_last_stage<=89;
  UINTN scale=s->Height>128?(s->Height-128)/121:0;
  UINTN horizontal=s->Width>32?(s->Width-32)/121:0;
  if(scale>horizontal)scale=horizontal;
  if(scale>8)scale=8;
  UINTN side=scale*121,rows=qr?112+side:48;
  UINT64 old_tcr,old_ttbr,table_pa,fb=s->Framebuffer,bytes=s->RowBytes*rows;
  if(!fb||s->Width<320||s->Width>8192||s->Height<320||s->Height>8192||!scale||
     s->RowBytes<s->Width*4||s->RowBytes>65536||rows>s->Height||
     fb>~0ULL-bytes||((fb>>30)!=((fb+bytes-1)>>30)))return;
  table_pa=s->PayloadPa+(UINT64)(UINTN)live_tables-(UINT64)(UINTN)diag_image_base;
  memset(live_tables,0,sizeof(live_tables));
  live_tables[(fb>>30)&511]=(table_pa+4096)|3;
  for(UINT64 pa=fb&~0x1fffffULL;pa<fb+bytes;pa+=0x200000)
    live_tables[512+((pa>>21)&511)]=pa|0x6000000000070dULL;
  diag_merge_guard_gop_map(s,fb);
  UINT64 line;
  __asm__ volatile("mrs %0,ctr_el0":"=r"(line));line=4ULL<<((line>>16)&15);
  for(UINT64 at=(UINT64)(UINTN)live_tables;at<(UINT64)(UINTN)live_tables+sizeof(live_tables);at+=line)
    __asm__ volatile("dc cvac,%0"::"r"(at):"memory");
  __asm__ volatile("mrs %0,tcr_el1\nmrs %1,ttbr0_el1":"=r"(old_tcr),"=r"(old_ttbr));
  __asm__ volatile("mrs %0,sctlr_el1":"=r"(diag_live_controls[0]));
  diag_live_controls[1]=old_tcr;diag_live_controls[2]=old_ttbr;
  __asm__ volatile("mrs %0,ttbr1_el1":"=r"(diag_live_controls[3]));
  UINT64 temporary=(old_tcr&~0xffffULL)|0x3519;
  map_gop(table_pa,temporary);
  for(UINTN y=0;y<rows;y++)for(UINTN x=0;x<s->Width;x++)pixel(s,x,y,0xff000000);
  draw_text(s,message,16,4,s->Width>=1600?2:1);
  if(qr)draw_text(s,"PREPARING FINAL CHECKPOINT TRANSFER ",16,52,1);
  map_gop(old_ttbr,old_tcr);
  if(!qr)return;
  diag_apply_relocations();diag_prepare_replay(s,message);
  diag_merge_guard_gop_map(s,fb);
  if(diag_last_stage==47&&diag_guard_status==2){
    __asm__ volatile("mrs %0,ttbr0_el1\nmrs %1,tcr_el1":"=r"(old_ttbr),"=r"(old_tcr));
    temporary=(old_tcr&~0xffffULL)|0x3519;
  }
  if(diag_replay_ready!=4){
    map_gop(table_pa,temporary);draw_text(s,"QR ENCODE FAILED",16,52,1);map_gop(old_ttbr,old_tcr);return;
  }
  diag_replay_draws=0;
  do{
    UINTN index=diag_replay_draws%4;
    map_gop(table_pa,temporary);
    draw_text(s,diag_export_mode?"PAUSED EXPORT - ONESEND - 500ms     ":"ONESEND REPLAY - 2s THEN CONTINUE   ",16,52,1);
    for(UINTN y=0;y<121;y++)for(UINTN x=0;x<121;x++){
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL b=diag_replay_pixels[index][y*121+x];
      UINT32 color=0xff000000U|((UINT32)b.Red<<16)|((UINT32)b.Green<<8)|b.Blue;
      for(UINTN yy=0;yy<scale;yy++)for(UINTN xx=0;xx<scale;xx++)pixel(s,16+x*scale+xx,80+y*scale+yy,color);
    }
    draw_text(s,index==0?"FRAME 1/4":index==1?"FRAME 2/4":index==2?"FRAME 3/4":"FRAME 4/4",16,80+side+4,1);
    map_gop(old_ttbr,old_tcr);diag_replay_draws++;replay_wait();
  }while(diag_export_mode||diag_replay_draws<4);
  map_gop(table_pa,temporary);
  draw_text(s,"KERNEL CONTINUES; LAST SNAPSHOT     ",16,52,1);
  map_gop(old_ttbr,old_tcr);diag_live_phase=0;
}
#endif

VOID diag_build_report(TB_REPORT *s){
  UINT64 previous_phase=diag_report_phase;
  if(!previous_phase)memcpy(&first_state,s,sizeof(*s));
  diag_report_phase=1;
  text_used=0;arena_used=0;
  BOOLEAN checkpoint=((s->Esr>>26)==0x3c)&&((s->Esr&0xffff)==0xb710);
  BOOLEAN before_vm=((s->Esr>>26)==0x3c)&&((s->Esr&0xffff)==0xb713);
  BOOLEAN selftest=((s->Esr>>26)==0x3c)&&((s->Esr&0xffff)==0xb711);
  append(previous_phase?"REPORTER SECONDARY EXCEPTION\n":
         selftest?"REPORTER SELFTEST (intentional stop)\n":
         (checkpoint||before_vm)?"XNU CHECKPOINT (intentional stop)\n":"XNU EARLY EXCEPTION\n");
  append_build_label();
  if(checkpoint)append("rebase and C prologue completed; before PE_init_platform\n");
  if(before_vm)append("platform and early C path completed; before arm_vm_init\n");
  if(selftest)append("display/exception/QR selftest only; not kernel boot success\n");
  if(last_message[0]){append(last_message);append("\n");}
  if(diag_live_phase)field("LIVE_PHASE",diag_live_phase);
  if(diag_last_stage&&!last_message[0]){
    static CONST CHAR8 *stages[]={"PE_init_platform","configure_timer_apple_regs",
      "wfe_timeout_configure","wfe_timeout_init","configure_misc_apple_boot_args",
      "configure_misc_apple_regs","ml_parse_cpu_topology","thread_bootstrap",
      "cpu_bootstrap","rtclock_early_init","kernel_startup_bootstrap","cpu_init","processor_bootstrap",
      "arm_vm_init"};
    UINTN i=(diag_last_stage-1)/2;
    if(i<sizeof(stages)/sizeof(stages[0])){
      append((diag_last_stage&1)?"enter: ":"returned: ");append(stages[i]);append("\n");
    }
  }
  if(previous_phase){field("REPORT_PHASE",previous_phase);field("FIRST_ESR",first_state.Esr);field("FIRST_ELR",first_state.Elr);}
  field("ESR",s->Esr);field("FAR",s->Far);field("ELR",s->Elr);
  if(diag_guard_status)field("SWITCH_GUARD",diag_guard_status);
  field("SP",s->Sp);field("LR",s->Regs[30]);field("SPSR",s->Spsr);
  field("SCTLR",s->Sctlr);field("X0",s->Regs[0]);field("X1",s->Regs[1]);
  field("X8",s->Regs[8]);field("X9",s->Regs[9]);
  field("FP",s->Regs[29]);field("X19",s->Regs[19]);field("X20",s->Regs[20]);
  if(s->Kind==2)append("panic entry intercepted; X0 is format pointer\n");
  append("fatal diagnostic; no reboot; one static OneSend frame\n");
  diag_test_text_length=text_used;
  if(s->Framebuffer==0||s->Width<320||s->Width>8192||s->Height<320||s->Height>8192||s->RowBytes<s->Width*4||s->RowBytes>65536)return;
  for(UINTN y=0;y<s->Height;y++)for(UINTN x=0;x<s->Width;x++)pixel(s,x,y,0xff000000);
  draw_text(s,text,16,16,s->Width>=1600?2:1);
  if(previous_phase){draw_text(s,"SECONDARY REPORT: QR SKIPPED",16,s->Height-48,1);diag_report_phase=99;return;}
  diag_report_phase=2;
  draw_text(s,"QR: PREPARING FRAME",16,s->Height-48,1);
  make_frame(FALSE);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bitmap=NULL;
  diag_report_phase=3;
  draw_text(s,"QR: ENCODING       ",16,s->Height-48,1);
  diag_test_qr_status=QrEncodeData(QrAutoVersion,QrECLevel_L,QrByteMode,0,frame,sizeof(frame),121,&bitmap);
  diag_report_phase=4;
  diag_test_bitmap=bitmap;diag_test_heap_used=arena_used;
  if(diag_test_qr_status||!bitmap){draw_text(s,"QR ENCODE FAILED; COPY REGISTERS ABOVE",16,s->Height-48,1);return;}
  UINTN scale=s->Height/2/121;if(scale>8)scale=8;
  if(scale==0)scale=1;
  UINTN side=121*scale;
  UINTN ox=s->Width>side*2+32?s->Width-side-32:(s->Width>side?(s->Width-side)/2:0);
  UINTN oy=s->Width>side*2+32?64:(s->Height>side+64?s->Height-side-64:0);
  diag_report_phase=5;
  for(UINTN y=0;y<121;y++)for(UINTN x=0;x<121;x++){
    UINT32 c=0xff000000U|((UINT32)bitmap[y*121+x].Red<<16)|((UINT32)bitmap[y*121+x].Green<<8)|bitmap[y*121+x].Blue;
    for(UINTN yy=0;yy<scale;yy++)for(UINTN xx=0;xx<scale;xx++)pixel(s,ox+x*scale+xx,oy+y*scale+yy,c);
  }
  draw_text(s,"QR: READY - SCAN WITH ONESEND",16,s->Height-48,1);
  diag_report_phase=6;
}

#ifndef TB_REPORT_HOST_TEST
typedef struct {UINT64 Offset,Info;INT64 Addend;} TB_RELA;
/* These three must be direct PC-relative symbols, never unrelocated GOT loads. */
extern UINT8 diag_image_base[] __attribute__((visibility("hidden")));
extern UINT8 diag_rela_start[] __attribute__((visibility("hidden")));
extern UINT8 diag_rela_end[] __attribute__((visibility("hidden")));
#ifndef DIAG_LINK_BASE
#define DIAG_LINK_BASE 0ULL
#endif
static VOID diag_apply_relocations(VOID){
  UINT64 slide=(UINT64)(UINTN)diag_image_base-DIAG_LINK_BASE;
  for(TB_RELA *r=(TB_RELA*)diag_rela_start;r<(TB_RELA*)diag_rela_end;r++){
    if((UINT32)r->Info!=1027)for(;;)__asm__ volatile("wfe");
    *(UINT64*)(UINTN)(r->Offset+slide)=(UINT64)r->Addend+slide;
  }
}
__attribute__((noreturn)) VOID diag_fault_main(TB_REPORT *s){
  diag_apply_relocations();
  diag_build_report(s);__asm__ volatile("dsb sy");for(;;)__asm__ volatile("wfe");
}
#endif
