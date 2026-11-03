/* apu.c - 2A03 APU (NTSC).  See apu.h and docs/APU.md.  Behaviour oracle: Mesen (ApuFrameCounter.h, ApuLengthCounter.h, ApuEnvelope.h,
 * SquareChannel.h, TriangleChannel.h, NoiseChannel.h, SoundMixer.cpp); every quirk below is a port of the corresponding Mesen code.
 * Integer only, no libc.
 *
 * Internal clock: a->cyc = bus cycle + 1 (this is Mesen's APU _currentCycle: the APU is clocked in StartCpuCycle, after the cycle counter
 * was incremented, and the reset sequence counts 8 cycles before the first opcode).  Bus-cycle arguments are converted on entry.
 * Events are stored with bus-cycle timestamps. */
#include "apu.h"

#define NEVER 0xFFFFFFFFFFFFFFFFull

/* ------------------------------------------------------------------ constant tables (mixer tables generated with Mesen's formulas, see tools/apu_gentab.c) */
static const uint8_t len_tab[32] = { 10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const uint16_t noise_per[16] = { 4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068 };
static const uint8_t duty_mask[4] = { 0x80, 0xC0, 0xF0, 0x3F };       /* bit p = output while the sequencer is at position p */
static const uint8_t tri_seq[32] = { 15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
static const int32_t step_cyc[2][6] = { {7457,14913,22371,29828,29829,29830}, {7457,14913,22371,29829,37281,37282} };
static const uint8_t step_type[6] = { 1,2,1,0,2,0 };                  /* 0 none, 1 quarter frame, 2 half frame */
static const uint16_t sq_mix[31] = {0,58,115,170,224,277,329,380,429,477,525,571,616,661,704,746,788,829,869,908,946,984,1021,1057,1092,1127,1161,1195,1228,1260,1292};
static const uint16_t tnd_mix[256] = {0,64,128,191,252,313,373,432,490,547,603,658,713,767,819,872,95,159,221,282,342,402,460,518,574,630,685,739,792,845,897,948,189,251,312,371,430,488,545,602,657,711,765,818,870,922,973,1023,281,341,400,459,516,573,629,684,738,791,844,896,947,997,1047,1096,370,429,487,544,600,656,710,764,817,869,921,971,1021,1071,1119,1167,457,515,571,627,682,737,790,843,894,945,996,1045,1094,1143,1190,1237,543,599,654,709,763,816,868,919,970,1020,1069,1118,1166,1213,1260,1306,626,681,735,789,841,893,944,995,1044,1093,1142,1189,1236,1282,1328,1373,708,761,814,867,918,969,1019,1068,1117,1165,1212,1259,1305,1350,1395,1439,787,840,892,943,993,1043,1092,1140,1188,1235,1281,1327,1372,1417,1460,1504,865,917,968,1018,1067,1116,1164,1211,1258,1304,1349,1394,1438,1482,1525,1567,942,992,1042,1091,1139,1187,1234,1280,1326,1371,1415,1459,1503,1545,1588,1629,1017,1066,1115,1163,1210,1257,1303,1348,1393,1437,1481,1524,1566,1608,1649,1690,1090,1138,1186,1233,1279,1325,1370,1414,1458,1502,1544,1587,1628,1669,1710,1750,1161,1209,1255,1301,1347,1392,1436,1479,1523,1565,1607,1648,1689,1729,1769,1809,1232,1278,1324,1369,1413,1457,1501,1543,1586,1627,1668,1709,1749,1789,1828,1866};

unsigned apu_mix(int sq1, int sq2, int tri, int noi){ return (unsigned)sq_mix[sq1+sq2] + tnd_mix[(tri<<4)|noi]; }

/* ------------------------------------------------------------------ noise LFSR skip-ahead.
 * Long mode (feedback bit0^bit1) is one cycle of 32767 states: position tables give O(1) jumps.
 * Short mode (bit0^bit6) has only cycles of 93 (32736 states) and 31 states: jump = (k mod L) single steps, L found by walking the cycle. */
static uint16_t lfsr_pos[32768], lfsr_seq[32768]; static uint8_t lfsr_ready;
static void lfsr_init(void){
    uint16_t s=1; unsigned i;
    if(lfsr_ready) return;
    for(i=0;i<32767;i++){ lfsr_seq[i]=s; lfsr_pos[s]=(uint16_t)i; s=(uint16_t)((s>>1)|(((s^(s>>1))&1)<<14)); }
    lfsr_ready=1;
}
static void lfsr_skip(ApuNoise *s, uint32_t k){
    uint16_t sr=s->sr;
    if(!s->mode){ uint32_t p=lfsr_pos[sr]+k%32767u; if(p>=32767u) p-=32767u; s->sr=lfsr_seq[p]; }
    else {
        uint16_t t=sr; uint32_t L=0, m;
        do{ t=(uint16_t)((t>>1)|(((t^(t>>6))&1)<<14)); L++; }while(t!=sr);
        for(m=k%L;m;m--) sr=(uint16_t)((sr>>1)|(((sr^(sr>>6))&1)<<14));
        s->sr=sr;
    }
}

/* ------------------------------------------------------------------ events */
static void ev_push(Apu *a, int ch, uint64_t tint, unsigned lvl){
    ApuEvent *q; uint32_t cap;
    switch(ch){ case 0: q=a->qsq0; cap=APU_Q_SQ; break; case 1: q=a->qsq1; cap=APU_Q_SQ; break; case 2: q=a->qtri; cap=APU_Q_TRI; break; default: q=a->qnoi; cap=APU_Q_NOI; break; }
    if(a->qt[ch]-a->qh[ch]>=cap){ a->dropped++; return; }
    q[a->qt[ch]&(cap-1)].t=(uint32_t)(tint-1); q[a->qt[ch]&(cap-1)].lvl=(uint8_t)lvl; a->qt[ch]++;
}
static inline void emit(Apu *a, int ch, uint64_t tint, unsigned lvl, uint8_t *last){
    if(lvl==*last) return; *last=(uint8_t)lvl; ev_push(a,ch,tint,lvl);
}

/* ------------------------------------------------------------------ shared pieces */
static inline unsigned env_vol(const ApuLen *l, const ApuEnv *e){ return l->len ? (e->constvol ? e->vol : e->counter) : 0; }
static inline int sq_muted(const ApuPulse *s){ return s->real_period<8 || (!s->sw_negate && s->sw_target>0x7FF); }
static inline unsigned sq_level(const ApuPulse *s, unsigned pos){ return (sq_muted(s) || !((duty_mask[s->duty]>>pos)&1)) ? 0 : env_vol(&s->l,&s->e); }

/* catch a channel up to internal cycle `target`: timer skip-ahead in closed form, level changes are queued as events.
 * Mirrors BaseApuChannel::Run: clocks happen at sync+timer+1, then every period+1 cycles. */
static void sq_sync(Apu *a, ApuPulse *s, int ch, uint64_t target){
    uint32_t n=(uint32_t)(target-s->sync), P, first, k, lastoff, V, j; uint64_t base=s->sync, t; unsigned pos;
    s->sync=target;
    if(!a->audio || !n) return;
    if(n<=s->timer){ s->timer-=n; return; }
    P=s->period+1; first=s->timer+1; k=1+(n-first)/P; lastoff=first+(k-1)*P; s->timer=s->period-(n-lastoff);
    V = sq_muted(s) ? 0 : env_vol(&s->l,&s->e);
    pos=s->dutypos;
    if(!V){ s->dutypos=(uint8_t)((pos-k)&7); emit(a,ch,base+first,0,&s->last); return; }
    t=base+first;
    for(j=0;j<k;j++,t+=P){ pos=(pos-1)&7; emit(a,ch,t,((duty_mask[s->duty]>>pos)&1)?V:0,&s->last); }
    s->dutypos=(uint8_t)pos;
}
static void tri_sync(Apu *a, ApuTri *s, uint64_t target){
    uint32_t n=(uint32_t)(target-s->sync), P, first, k, lastoff, j; uint64_t base=s->sync, t; unsigned seq;
    s->sync=target;
    if(!a->audio || !n) return;
    if(n<=s->timer){ s->timer-=n; return; }
    P=s->period+1; first=s->timer+1; k=1+(n-first)/P; lastoff=first+(k-1)*P; s->timer=s->period-(n-lastoff);
    if(!(s->l.len>0 && s->lin>0)) return;
    seq=s->seq; t=base+first;
    for(j=0;j<k;j++,t+=P){ seq=(seq+1)&31; emit(a,2,t,tri_seq[seq],&s->last); }
    s->seq=(uint8_t)seq;
}
static void noi_sync(Apu *a, ApuNoise *s, uint64_t target){
    uint32_t n=(uint32_t)(target-s->sync), P, first, k, lastoff, V, j; uint64_t base=s->sync, t; unsigned sr, sh;
    s->sync=target;
    if(!a->audio || !n) return;
    if(n<=s->timer){ s->timer-=n; return; }
    P=s->period+1; first=s->timer+1; k=1+(n-first)/P; lastoff=first+(k-1)*P; s->timer=s->period-(n-lastoff);
    V=env_vol(&s->l,&s->e);
    if(!V){ lfsr_skip(s,k); emit(a,3,base+first,0,&s->last); return; }
    sr=s->sr; sh=s->mode?6:1; t=base+first;
    for(j=0;j<k;j++,t+=P){ sr=(sr>>1)|(((sr^(sr>>sh))&1)<<14); emit(a,3,t,(sr&1)?0:V,&s->last); }
    s->sr=(uint16_t)sr;
}
static void sync_all(Apu *a, uint64_t t){
    sq_sync(a,&a->sq[0],0,t); sq_sync(a,&a->sq[1],1,t); tri_sync(a,&a->tri,t); noi_sync(a,&a->noi,t);
}

static void env_tick(ApuEnv *e, const ApuLen *l){
    if(!e->start){
        e->divider--;
        if(e->divider<0){ e->divider=(int8_t)e->vol; if(e->counter>0) e->counter--; else if(l->halt) e->counter=15; }
    } else { e->start=0; e->counter=15; e->divider=(int8_t)e->vol; }
}
static void len_tick(ApuLen *l){ if(l->len>0 && !l->halt) l->len--; }
static void len_reload(ApuLen *l){
    if(l->reload){ if(l->len==l->prev) l->len=l->reload; l->reload=0; }
    l->halt=l->newhalt;
}
static void len_init(Apu *a, ApuLen *l, int halt){ a->pending=1; l->newhalt=(uint8_t)halt; }
static void len_load(Apu *a, ApuLen *l, unsigned idx){ if(l->enabled){ l->reload=len_tab[idx]; l->prev=l->len; a->pending=1; } }

static void sq_target(ApuPulse *s){
    uint32_t sh=(uint32_t)s->real_period>>s->sw_shift;
    if(s->sw_negate){ s->sw_target=s->real_period-sh; if(s->ch1) s->sw_target--; }
    else s->sw_target=s->real_period+sh;
}
static void sq_setperiod(ApuPulse *s, unsigned p){ s->real_period=(uint16_t)p; s->period=(uint32_t)p*2+1; sq_target(s); }
static void sq_sweep_tick(ApuPulse *s){
    s->sw_divider--;
    if(s->sw_divider==0){
        if(s->sw_shift>0 && s->sw_enable && s->real_period>=8 && s->sw_target<=0x7FF) sq_setperiod(s,s->sw_target);
        s->sw_divider=s->sw_period;
    }
    if(s->sw_reload){ s->sw_divider=s->sw_period; s->sw_reload=0; }
}
static void tri_lin_tick(ApuTri *t){
    if(t->lin_flag) t->lin=t->lin_reload; else if(t->lin>0) t->lin--;
    if(!t->lin_ctrl) t->lin_flag=0;
}

/* frame counter tick at internal cycle T: channels are brought to T-1 first (their clock at T sees the new state) */
static void frame_tick(Apu *a, int type, uint64_t T){
    sync_all(a,T-1);
    env_tick(&a->sq[0].e,&a->sq[0].l); env_tick(&a->sq[1].e,&a->sq[1].l); tri_lin_tick(&a->tri); env_tick(&a->noi.e,&a->noi.l);
    if(type==2){
        len_tick(&a->sq[0].l); len_tick(&a->sq[1].l); len_tick(&a->tri.l); len_tick(&a->noi.l);
        sq_sweep_tick(&a->sq[0]); sq_sweep_tick(&a->sq[1]);
    }
}

/* ApuFrameCounter::Run for exactly n cycles (n never crosses a step boundary: the caller clamps it) */
static void fc_step(Apu *a, uint32_t n){
    uint64_t T=a->cyc+n;
    if(a->fc_prev+(int32_t)n >= step_cyc[a->fc_mode][a->fc_step]){
        int ty;
        if(!a->fc_inhibit && a->fc_mode==0 && a->fc_step>=3) a->irq=1;
        ty=step_type[a->fc_step];
        if(ty && !a->fc_block){ frame_tick(a,ty,T); a->fc_block=2; }
        a->fc_step++;
        if(a->fc_step==6){ a->fc_step=0; a->fc_prev=0; } else a->fc_prev+=(int32_t)n;
    } else a->fc_prev+=(int32_t)n;
    if(a->fc_newval>=0){
        a->fc_wdelay--;
        if(a->fc_wdelay==0){
            a->fc_mode=(a->fc_newval&0x80)?1:0; a->fc_wdelay=-1; a->fc_step=0; a->fc_prev=0; a->fc_newval=-1;
            if(a->fc_mode && !a->fc_block){ frame_tick(a,2,T); a->fc_block=2; }
        }
    }
    if(a->fc_block>0) a->fc_block--;
}

void apu_run_to(Apu *a, uint64_t cc){
    uint64_t target=cc+1;
    while(a->cyc<target){
        uint64_t rem=target-a->cyc; uint32_t n=rem>0x40000000u?0x40000000u:(uint32_t)rem;
        if(a->fc_newval>=0 || a->fc_block>0 || a->pending) n=1;      /* cycle-by-cycle ordering effects */
        else { uint32_t d=(uint32_t)(step_cyc[a->fc_mode][a->fc_step]-a->fc_prev); if(n>d) n=d; }
        fc_step(a,n);
        a->cyc+=n;
        if(a->pending){                                              /* ReloadCounter: after the frame counter, before the channel clocks of this cycle */
            sync_all(a,a->cyc-1);
            len_reload(&a->sq[0].l); len_reload(&a->sq[1].l); len_reload(&a->tri.l); len_reload(&a->noi.l);
            a->pending=0;
        }
    }
}

/* ------------------------------------------------------------------ public */
void apu_set_audio(Apu *a, int on){
    if(on && !a->audio){ a->sq[0].sync=a->sq[1].sync=a->tri.sync=a->noi.sync=a->cyc; }
    a->audio=(uint8_t)(on!=0);
}
void apu_reset(Apu *a){
    uint32_t *w=(uint32_t*)a; unsigned i;
    lfsr_init();
    for(i=0;i<sizeof(Apu)/4;i++) w[i]=0;
    a->sq[0].ch1=1;
    a->noi.period=noise_per[0]-1; a->noi.sr=1;
    a->fc_newval=0; a->fc_wdelay=3;          /* "reset acts as if $00 was written to $4017" */
    a->audio=1;
}
void apu_flush(Apu *a, uint64_t cc){ if(cc){ apu_run_to(a,cc-1); sync_all(a,a->cyc); } }
int apu_irq(const Apu *a){ return a->irq; }
uint64_t apu_irq_cycle(const Apu *a){
    int32_t d;
    if(a->irq || a->fc_inhibit || a->fc_mode) return NEVER;
    if(a->fc_newval>=0 || a->fc_block) return a->cyc;                /* conservative */
    d=step_cyc[0][a->fc_step<3?3:a->fc_step]-a->fc_prev;
    return a->cyc+(uint32_t)d-1;
}

uint8_t apu_read_status(Apu *a, uint64_t cc){
    uint8_t s;
    apu_run_to(a,cc);
    s=(uint8_t)((a->sq[0].l.len>0) | ((a->sq[1].l.len>0)<<1) | ((a->tri.l.len>0)<<2) | ((a->noi.l.len>0)<<3) | (a->irq<<6));
    a->irq=0;
    return s;
}

void apu_write(Apu *a, uint16_t addr, uint8_t v, uint64_t cc){
    uint64_t T; ApuPulse *s;
    apu_run_to(a,cc); T=a->cyc;
    if(addr<0x4008){
        int ch=(addr>>2)&1; s=&a->sq[ch];
        sq_sync(a,s,ch,T);
        switch(addr&3){
        case 0: len_init(a,&s->l,(v>>5)&1); s->e.constvol=(v>>4)&1; s->e.vol=v&15; s->duty=v>>6; break;
        case 1: s->sw_enable=v>>7; s->sw_negate=(v>>3)&1; s->sw_period=((v>>4)&7)+1; s->sw_shift=v&7; sq_target(s); s->sw_reload=1; break;
        case 2: sq_setperiod(s,(s->real_period&0x700)|v); break;
        default: len_load(a,&s->l,v>>3); sq_setperiod(s,(s->real_period&0xFF)|((v&7)<<8)); s->dutypos=0; s->e.start=1; break;
        }
        emit(a,ch,T,sq_level(s,s->dutypos),&s->last);
    } else if(addr<0x400C){
        tri_sync(a,&a->tri,T);
        switch(addr&3){
        case 0: a->tri.lin_ctrl=v>>7; a->tri.lin_reload=v&0x7F; len_init(a,&a->tri.l,a->tri.lin_ctrl); break;
        case 2: a->tri.period=(a->tri.period&~0xFFu)|v; break;
        case 3: len_load(a,&a->tri.l,v>>3); a->tri.period=(a->tri.period&~0xFF00u)|((uint32_t)(v&7)<<8); a->tri.lin_flag=1; break;
        }
    } else if(addr<0x4010){
        noi_sync(a,&a->noi,T);
        switch(addr&3){
        case 0: len_init(a,&a->noi.l,(v>>5)&1); a->noi.e.constvol=(v>>4)&1; a->noi.e.vol=v&15; break;
        case 2: a->noi.period=noise_per[v&15]-1; a->noi.mode=v>>7; break;
        case 3: len_load(a,&a->noi.l,v>>3); a->noi.e.start=1; break;
        }
    } else if(addr<0x4014){
        /* DMC registers: accepted and ignored (DMC never enabled by this game, status bit 4 stays 0) */
    } else if(addr==0x4015){
        sync_all(a,T);
        a->sq[0].l.enabled=v&1; if(!(v&1)) a->sq[0].l.len=0;
        a->sq[1].l.enabled=(v>>1)&1; if(!(v&2)) a->sq[1].l.len=0;
        a->tri.l.enabled=(v>>2)&1; if(!(v&4)) a->tri.l.len=0;
        a->noi.l.enabled=(v>>3)&1; if(!(v&8)) a->noi.l.len=0;
    } else if(addr==0x4017){
        a->fc_newval=v;
        a->fc_wdelay=((cc&1)?4:3);           /* parity of the bus cycle count, as Mesen's CPU cycle count */
        a->fc_inhibit=(v>>6)&1;
        if(a->fc_inhibit) a->irq=0;
    }
}

/* ------------------------------------------------------------------ rendering */
int apu_pop_event(Apu *a, uint64_t until, uint32_t *t, int *ch, int *lvl){
    static const uint32_t caps[4]={APU_Q_SQ,APU_Q_SQ,APU_Q_TRI,APU_Q_NOI};
    const ApuEvent *qs[4]; int c, best=-1; int32_t bo=0; uint32_t u=(uint32_t)until;
    qs[0]=a->qsq0; qs[1]=a->qsq1; qs[2]=a->qtri; qs[3]=a->qnoi;
    for(c=0;c<4;c++) if(a->qh[c]!=a->qt[c]){
        uint32_t et=qs[c][a->qh[c]&(caps[c]-1)].t; int32_t o=(int32_t)(et-u);
        if(best<0 || o<bo){ best=c; bo=o; }
    }
    if(best<0 || bo>=0) return 0;
    { const ApuEvent *e=&qs[best][a->qh[best]&(caps[best]-1)]; *t=e->t; *lvl=e->lvl; *ch=best; a->qh[best]++; }
    return 1;
}

/* Event merge with the next event time of every queue cached in a register-sized array (was: 4 queue head reads and compares per event and per sample).
 * Output is bit-identical to the previous implementation (same integration, same order of equal-time events: lowest channel index first). */
#define NOEV 0x7FFFFFFF
static void render(Apu *a, uint64_t from, uint64_t to, void *out, unsigned n, int s8){
    static const uint32_t caps[4]={APU_Q_SQ,APU_Q_SQ,APU_Q_TRI,APU_Q_NOI};
    const ApuEvent *qs[4]; uint64_t span=to-from, rec[2]; uint32_t q, r, acc=0, base=(uint32_t)from, pos=0, i;
    unsigned lvl[4], mix; int c; int32_t nt[4]; uint32_t hd[4], tl[4];
    if(!n || to<=from) return;
    apu_flush(a,to);
    qs[0]=a->qsq0; qs[1]=a->qsq1; qs[2]=a->qtri; qs[3]=a->qnoi;
    q=(uint32_t)(span/n); r=(uint32_t)(span%n);
    if(!q) return;
    rec[0]=(0x100000000ull+q-1)/q; rec[1]=(0x100000000ull+q)/(q+1);
    for(c=0;c<4;c++){ lvl[c]=a->rlvl[c]; hd[c]=a->qh[c]; tl[c]=a->qt[c]; nt[c]= hd[c]!=tl[c] ? (int32_t)(qs[c][hd[c]&(caps[c]-1)].t-base) : NOEV; }
#define NEXT_BEST(best,bo) do{ best=0; bo=nt[0]; if(nt[1]<bo){best=1;bo=nt[1];} if(nt[2]<bo){best=2;bo=nt[2];} if(nt[3]<bo){best=3;bo=nt[3];} }while(0)
#define CONSUME(best) do{ const ApuEvent *e_=&qs[best][hd[best]&(caps[best]-1)]; lvl[best]=e_->lvl; hd[best]++; \
        nt[best]= hd[best]!=tl[best] ? (int32_t)(qs[best][hd[best]&(caps[best]-1)].t-base) : NOEV; }while(0)
    for(;;){                                      /* events before `from` that were never rendered: apply */
        int best; int32_t bo; NEXT_BEST(best,bo);
        if(bo>=0) break;                          /* also covers "no event" (NOEV) */
        CONSUME(best);
    }
    mix=apu_mix(lvl[0],lvl[1],lvl[2],lvl[3]);
    for(i=0;i<n;i++){
        uint32_t len=q, end, cur=pos, sum=0; int idx=0; uint32_t m;
        acc+=r; if(acc>=n){ acc-=n; len++; idx=1; }
        end=pos+len;
        for(;;){
            int best; int32_t bo; NEXT_BEST(best,bo);
            if((uint32_t)bo>=end) break;
            sum+=mix*((uint32_t)bo-cur); cur=(uint32_t)bo;
            CONSUME(best);
            mix=apu_mix(lvl[0],lvl[1],lvl[2],lvl[3]);
        }
        sum+=mix*(end-cur); pos=end;
        m=(uint32_t)(((uint64_t)sum*rec[idx])>>32);
        if(s8) ((int8_t*)out)[i]=(int8_t)(((m*83u)>>10)-128u); else ((int16_t*)out)[i]=(int16_t)m;
    }
    for(c=0;c<4;c++){ a->rlvl[c]=(uint8_t)lvl[c]; a->qh[c]=hd[c]; }
    a->rpos=to;
}
void apu_render16(Apu *a, uint64_t from, uint64_t to, int16_t *out, unsigned n){ render(a,from,to,out,n,0); }
void apu_render8(Apu *a, uint64_t from, uint64_t to, int8_t *out, unsigned n){ render(a,from,to,out,n,1); }
