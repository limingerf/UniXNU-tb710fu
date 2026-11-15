/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Terminal early-XNU report. Captures no invented console lines. The immutable
 * report is one OneSend LMS1 envelope / OS frame. QR encoder is upstream Mu.
 */
#include "Report.h"
#include <Library/QrEncoderLib.h>
#include "Font.h"

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
VOID FreePool(VOID *p){(VOID)p;} /* One fatal report, bounded arena, no reuse. */

static VOID append(CONST CHAR8 *p){while(*p&&text_used<sizeof(text)-1)text[text_used++]=*p++;text[text_used]=0;}
static VOID number(UINT64 v){CONST CHAR8 h[]="0123456789abcdef";for(int s=60;s>=0;s-=4){CHAR8 b[2]={h[(v>>s)&15],0};append(b);}}
static VOID field(CONST CHAR8 *name,UINT64 v){append(name);append("=");number(v);append("\n");}
static UINT32 crc(CONST UINT8 *p,UINTN n){UINT32 c=~0U;for(UINTN i=0;i<n;i++){c^=p[i];for(int k=0;k<8;k++)c=(c>>1)^((0U-(c&1U))&0xedb88320U);}return ~c;}
static VOID le16(UINT8 *p,UINT16 v){p[0]=v;p[1]=v>>8;}
static VOID le32(UINT8 *p,UINT32 v){for(int i=0;i<4;i++)p[i]=v>>(i*8);}

static VOID make_frame(VOID) {
  CONST CHAR8 name[]="TB710FU-early-fault.log",mime[]="text/plain";
  UINT8 envelope[720];UINTN nl=sizeof(name)-1,ml=sizeof(mime)-1;
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

VOID diag_build_report(TB_REPORT *s){
  text_used=0;arena_used=0;
  BOOLEAN checkpoint=((s->Esr>>26)==0x3c)&&((s->Esr&0xffff)==0xb710);
  BOOLEAN selftest=((s->Esr>>26)==0x3c)&&((s->Esr&0xffff)==0xb711);
  append(selftest?"REPORTER SELFTEST (intentional stop)\n":
         checkpoint?"XNU CHECKPOINT (intentional stop)\n":"XNU EARLY EXCEPTION\n");
  append("build cc690c9a / reporter 1\n");
  if(checkpoint)append("rebase and C prologue completed; before PE_init_platform\n");
  if(selftest)append("display/exception/QR selftest only; not kernel boot success\n");
  field("ESR",s->Esr);field("FAR",s->Far);field("ELR",s->Elr);
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
  make_frame();
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bitmap=NULL;
  diag_test_qr_status=QrEncodeData(QrAutoVersion,QrECLevel_L,QrByteMode,0,frame,sizeof(frame),121,&bitmap);
  diag_test_bitmap=bitmap;diag_test_heap_used=arena_used;
  if(diag_test_qr_status||!bitmap){draw_text(s,"QR ENCODE FAILED; COPY REGISTERS ABOVE",16,s->Height-48,1);return;}
  UINTN scale=s->Height/2/121;if(scale>8)scale=8;
  if(scale==0)scale=1;
  UINTN side=121*scale,ox=s->Width>side?(s->Width-side)/2:0,oy=s->Height>side+32?s->Height-side-32:0;
  for(UINTN y=0;y<121;y++)for(UINTN x=0;x<121;x++){
    UINT32 c=0xff000000U|((UINT32)bitmap[y*121+x].Red<<16)|((UINT32)bitmap[y*121+x].Green<<8)|bitmap[y*121+x].Blue;
    for(UINTN yy=0;yy<scale;yy++)for(UINTN xx=0;xx<scale;xx++)pixel(s,ox+x*scale+xx,oy+y*scale+yy,c);
  }
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
__attribute__((noreturn)) VOID diag_fault_main(TB_REPORT *s){
  UINT64 slide=(UINT64)(UINTN)diag_image_base-DIAG_LINK_BASE;
  for(TB_RELA *r=(TB_RELA*)diag_rela_start;r<(TB_RELA*)diag_rela_end;r++){
    if((UINT32)r->Info!=1027)for(;;)__asm__ volatile("wfe");
    *(UINT64*)(UINTN)(r->Offset+slide)=(UINT64)r->Addend+slide;
  }
  diag_build_report(s);__asm__ volatile("dsb sy");for(;;)__asm__ volatile("wfe");
}
#endif
