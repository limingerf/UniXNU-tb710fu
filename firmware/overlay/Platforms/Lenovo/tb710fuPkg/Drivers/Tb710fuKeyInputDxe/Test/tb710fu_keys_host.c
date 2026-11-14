// SPDX-License-Identifier: BSD-2-Clause-Patent
// Synchronous observer model: every command starts a fresh transfer, no threads.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t rd (volatile uint32_t *);
static void wr (volatile uint32_t *, uint32_t);
static unsigned barriers;
#define TB710FU_SPMI_READ32(address) rd(address)
#define TB710FU_SPMI_WRITE32(address, value) wr((address), (value))
#define TB710FU_SPMI_BARRIER() (++barriers)
#include "../Tb710fuSpmiKeys.h"

static volatile uint32_t core[0x3000/4], cnfg[0x4000/4], obs[8*SK_OBS_EE_STRIDE/4];
static const int pon = 9, gpio = 21;
static uint8_t pon_sts, gpio_sts;
static int bad_apid = -1, silent_apid = -1;
static uint32_t errors, final_status;
static unsigned commands, status_reads, delay_reads, remaining;
static volatile uint32_t *channel;

static uint32_t rd (volatile uint32_t *p) {
  if (channel && p == channel + SK_STATUS/4) {
    ++status_reads;
    if (remaining) { --remaining; return 0; }
    *p = final_status;
  }
  return *p;
}
static void wr (volatile uint32_t *p, uint32_t cmd) {
  uintptr_t n = (uintptr_t)p - (uintptr_t)obs;
  assert ((uintptr_t)p >= (uintptr_t)obs && n < sizeof obs);
  unsigned apid = (unsigned)(n % SK_OBS_EE_STRIDE) / SK_OBS_APID_STRIDE;
  assert (n % SK_OBS_APID_STRIDE == SK_CMD && apid < SK_MAX_APIDS);
  assert (cmd == ((uint32_t)SK_OP_EXT_READL << 27 | SK_REG_RT_STS << 4));
  ++commands; *p = cmd; channel = p;
  channel[SK_STATUS/4] = 0;
  channel[SK_RDATA0/4] = apid == (unsigned)pon ? pon_sts : apid == (unsigned)gpio ? gpio_sts : 0xee;
  final_status = (int)apid == silent_apid ? 0 : SK_STATUS_DONE | ((int)apid == bad_apid ? errors : 0);
  remaining = delay_reads;
}
static void setup (void) {
  memset((void*)core,0,sizeof core); memset((void*)cnfg,0,sizeof cnfg); memset((void*)obs,0,sizeof obs);
  core[0] = SK_VERSION_V7_MIN; core[SK_FEATURES/4] = 40;
  core[(SK_APID_MAP+4*3)/4] = SK_PPID_PON << 8; cnfg[3] = 2;
  core[(SK_APID_MAP+4*pon)/4] = SK_PPID_PON << 8;
  core[(SK_APID_MAP+4*gpio)/4] = SK_PPID_GPIO6 << 8;
  core[(SK_APID_MAP+4*30)/4] = SK_PPID_PON << 8; cnfg[30] = 3;
  channel=NULL; commands=barriers=status_reads=delay_reads=0; bad_apid=silent_apid=-1;
}
static void arbiter_tests (void) {
  struct sk_arbiter a={core,obs,cnfg,0}; struct sk_keys k; int apid; uint8_t mask, value=0xa5;
  setup();
  assert(!sk_find_apid(&a,SK_PPID_PON,&apid) && apid==pon);
  assert(sk_find_apid(&a,0x555,&apid)==3 && apid==-1);
  cnfg[pon]=4; assert(!sk_find_apid(&a,SK_PPID_PON,&apid) && apid==3); cnfg[pon]=0;
  core[0]=0x60000000; assert(sk_find_apid(&a,SK_PPID_PON,&apid)==1);
  core[0]=SK_VERSION_V8_MIN; assert(sk_find_apid(&a,SK_PPID_PON,&apid)==1); core[0]=SK_VERSION_V7_MIN;
  core[SK_FEATURES/4]=0; assert(sk_find_apid(&a,SK_PPID_PON,&apid)==2);
  core[SK_FEATURES/4]=1025; assert(sk_find_apid(&a,SK_PPID_PON,&apid)==2);
  core[SK_FEATURES/4]=SK_MAX_APIDS; core[(SK_APID_MAP+4*(SK_MAX_APIDS-1))/4]=0x555u<<8;
  assert(!sk_find_apid(&a,0x555,&apid) && apid==SK_MAX_APIDS-1); core[SK_FEATURES/4]=40;
  assert(!sk_open(&a,&k) && k.pon==pon && k.gpio==gpio);
  struct {uint8_t p,g,want;} cases[]={
    {0,1,0},{0x80,1,SK_KEY_POWER},{0x40,1,SK_KEY_VOLDOWN},{0xc0,1,SK_KEY_POWER|SK_KEY_VOLDOWN},
    {0,0,SK_KEY_VOLUP},{0x3f,1,0},{0x80,0,SK_KEY_POWER|SK_KEY_VOLUP}
  };
  delay_reads=2;
  for(unsigned i=0;i<sizeof cases/sizeof *cases;++i) {
    pon_sts=cases[i].p; gpio_sts=cases[i].g; unsigned before=commands;
    assert(!sk_sample(&a,&k,&mask) && mask==cases[i].want);
    assert(commands==before+2); // identical command still gets fresh data on the next poll
  }
  assert(barriers==commands*2);
  for(unsigned i=1;i<=3;++i) {
    bad_apid=pon; errors=1u<<i;
    assert(sk_read8(&a,pon,SK_REG_RT_STS,&value)==2 && value==0xa5);
    assert(sk_sample(&a,&k,&mask)==2 && mask==0);
  }
  bad_apid=gpio; pon_sts=0x80; assert(sk_sample(&a,&k,&mask)==2 && mask==0); // no partial mask
  bad_apid=-1; silent_apid=pon; unsigned before=status_reads;
  assert(sk_read8(&a,pon,SK_REG_RT_STS,&value)==1 && value==0xa5 && status_reads-before==SK_POLLS);
  silent_apid=-1; delay_reads=0; before=commands;
  assert(sk_read8(&a,-1,SK_REG_RT_STS,&value)==4 && sk_read8(&a,40,SK_REG_RT_STS,&value)==4);
  assert(sk_read8(&a,pon,SK_REG_RT_STS,NULL)==4);
  a.ee=8; assert(sk_read8(&a,pon,SK_REG_RT_STS,&value)==4); a.ee=0; assert(commands==before);
  struct sk_arbiter invalid=a; invalid.cnfg=NULL; assert(sk_find_apid(&invalid,SK_PPID_PON,&apid)==4);
  assert(sk_find_apid(&a,0x1000,&apid)==4 && sk_find_apid(&a,1,NULL)==4);
  assert(sk_sample(&a,NULL,&mask)==4 && sk_sample(&a,&k,NULL)==4 && sk_open(&a,NULL)==4);
  a.ee=7; pon_sts=0x40; assert(!sk_read8(&a,pon,SK_REG_RT_STS,&value) && value==0x40);
  assert(channel==obs+(7*SK_OBS_EE_STRIDE+pon*SK_OBS_APID_STRIDE)/4); a.ee=0;
  core[(SK_APID_MAP+4*gpio)/4]=0; assert(!sk_open(&a,&k) && k.gpio==-1);
  pon_sts=0x80; assert(!sk_sample(&a,&k,&mask) && mask==SK_KEY_POWER);
}
static void filter_tests (void) {
  struct sk_filter f={0};
  assert(!sk_step(&f,SK_KEY_VOLUP,4,2) && sk_step(&f,SK_KEY_VOLUP,4,2)==SK_KEY_VOLUP);
  unsigned repeats=0;
  for(unsigned i=0;i<12;++i) repeats+=sk_step(&f,SK_KEY_VOLUP,4,2)==SK_KEY_VOLUP;
  assert(repeats==5 && f.held[2]==4);
  assert(!sk_step(&f,0,4,2) && !sk_step(&f,0,4,2) && !f.held[2] && !f.repeat[2]);
  assert(!sk_step(&f,SK_KEY_VOLDOWN,4,2) && sk_step(&f,SK_KEY_VOLDOWN,4,2)==SK_KEY_VOLDOWN);
  f=(struct sk_filter){0};
  assert(!sk_step(&f,SK_KEY_POWER,4,2) && sk_step(&f,SK_KEY_POWER,4,2)==SK_KEY_POWER);
  for(unsigned i=0;i<70000;++i) assert(!sk_step(&f,SK_KEY_POWER,4,2));
  f=(struct sk_filter){0}; assert(!sk_step(&f,SK_KEY_VOLUP,4,2) && !sk_step(&f,0,4,2) && !sk_step(&f,SK_KEY_VOLUP,4,2));
  f=(struct sk_filter){0}; assert(!sk_step(&f,7,4,2) && sk_step(&f,7,4,2)==7);
  for(unsigned i=0;i<30;++i) assert(!(sk_step(&f,7,4,2)&SK_KEY_POWER));
  for(unsigned z=0;z<2;++z) {
    f=(struct sk_filter){0}; uint16_t delay=z?4:0, period=z?0:2;
    assert(!sk_step(&f,SK_KEY_VOLUP,delay,period) && sk_step(&f,SK_KEY_VOLUP,delay,period)==SK_KEY_VOLUP);
    for(unsigned i=0;i<100;++i) assert(!sk_step(&f,SK_KEY_VOLUP,delay,period));
  }
  f=(struct sk_filter){0}; assert(!sk_step(&f,SK_KEY_VOLUP,UINT16_MAX,2) && sk_step(&f,SK_KEY_VOLUP,UINT16_MAX,2)==SK_KEY_VOLUP);
  repeats=0; for(unsigned i=0;i<70000;++i) repeats+=!!sk_step(&f,SK_KEY_VOLUP,UINT16_MAX,2);
  assert(repeats==2233 && f.held[2]==UINT16_MAX);
  f=(struct sk_filter){0}; assert(!sk_step(&f,SK_KEY_VOLDOWN,1,UINT16_MAX) && sk_step(&f,SK_KEY_VOLDOWN,1,UINT16_MAX)==SK_KEY_VOLDOWN);
  assert(sk_step(&f,SK_KEY_VOLDOWN,1,UINT16_MAX)==SK_KEY_VOLDOWN);
  for(unsigned i=0;i<UINT16_MAX-1u;++i) assert(!sk_step(&f,SK_KEY_VOLDOWN,1,UINT16_MAX));
  assert(sk_step(&f,SK_KEY_VOLDOWN,1,UINT16_MAX)==SK_KEY_VOLDOWN);
}
int main(void) {
  arbiter_tests(); filter_tests();
  puts("PASS: v7 APID ownership, bounded observer MMIO, fresh key samples, errors/timeouts, debounce and saturating repeat");
  return 0;
}
