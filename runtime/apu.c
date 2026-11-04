/* apu.c - 2A03 APU (NTSC).  See apu.h and docs/APU.md.  Behaviour oracle: Mesen (ApuFrameCounter.h, ApuLengthCounter.h, ApuEnvelope.h,
 * SquareChannel.h, TriangleChannel.h, NoiseChannel.h, SoundMixer.cpp); every quirk below is a port of the corresponding Mesen code.
 * Integer only, no libc.
 *
 * Internal clock: a->cyc = bus cycle + 1 (this is Mesen's APU _currentCycle: the APU is clocked in StartCpuCycle, after the cycle counter
 * was incremented, and the reset sequence counts 8 cycles before the first opcode).  Bus-cycle arguments are converted on entry.
 * Events are stored with bus-cycle timestamps. */
#include "apu.h"

#define NEVER 0xFFFFFFFFFFFFFFFFull

#ifdef APU_PROF      /* test builds: E-clock timing of the APU entry points (see docs/APU.md); apu_prof_* are read by the telemetry of the Amiga test build */
extern uint16_t plat_eclock(void);
uint32_t apu_prof_w, apu_prof_r, apu_prof_flush, apu_prof_merge, apu_prof_core, apu_prof_n;
#define PT0 uint16_t pt0_=plat_eclock()
#define PT1(v) do{ uint16_t d_=(uint16_t)(pt0_-plat_eclock()); if(d_<0x8000) (v)+=d_; }while(0)   /* d_ >= 0x8000: the E-clock read twice in a row went backwards by one tick */
#else
#define PT0
#define PT1(v)
#endif

/* ------------------------------------------------------------------ constant tables (mixer tables generated with Mesen's formulas, see tools/apu_gentab.c) */
static const uint8_t len_tab[32] = { 10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const uint16_t noise_per[16] = { 4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068 };
static const uint8_t duty_mask[4] = { 0x80, 0xC0, 0xF0, 0x3F };       /* bit p = output while the sequencer is at position p */
static const int32_t step_cyc[2][6] = { {7457,14913,22371,29828,29829,29830}, {7457,14913,22371,29829,37281,37282} };
static const uint8_t step_type[6] = { 1,2,1,0,2,0 };                  /* 0 none, 1 quarter frame, 2 half frame */
static const uint16_t sq_mix[31] = {0,58,115,170,224,277,329,380,429,477,525,571,616,661,704,746,788,829,869,908,946,984,1021,1057,1092,1127,1161,1195,1228,1260,1292};
static const uint16_t tnd_mix[256] = {0,64,128,191,252,313,373,432,490,547,603,658,713,767,819,872,95,159,221,282,342,402,460,518,574,630,685,739,792,845,897,948,189,251,312,371,430,488,545,602,657,711,765,818,870,922,973,1023,281,341,400,459,516,573,629,684,738,791,844,896,947,997,1047,1096,370,429,487,544,600,656,710,764,817,869,921,971,1021,1071,1119,1167,457,515,571,627,682,737,790,843,894,945,996,1045,1094,1143,1190,1237,543,599,654,709,763,816,868,919,970,1020,1069,1118,1166,1213,1260,1306,626,681,735,789,841,893,944,995,1044,1093,1142,1189,1236,1282,1328,1373,708,761,814,867,918,969,1019,1068,1117,1165,1212,1259,1305,1350,1395,1439,787,840,892,943,993,1043,1092,1140,1188,1235,1281,1327,1372,1417,1460,1504,865,917,968,1018,1067,1116,1164,1211,1258,1304,1349,1394,1438,1482,1525,1567,942,992,1042,1091,1139,1187,1234,1280,1326,1371,1415,1459,1503,1545,1588,1629,1017,1066,1115,1163,1210,1257,1303,1348,1393,1437,1481,1524,1566,1608,1649,1690,1090,1138,1186,1233,1279,1325,1370,1414,1458,1502,1544,1587,1628,1669,1710,1750,1161,1209,1255,1301,1347,1392,1436,1479,1523,1565,1607,1648,1689,1729,1769,1809,1232,1278,1324,1369,1413,1457,1501,1543,1586,1627,1668,1709,1749,1789,1828,1866};

static const uint16_t sq2_mix[256] = {0,58,115,170,224,277,329,380,429,477,525,571,616,661,704,746,58,115,170,224,277,329,380,429,477,525,571,616,661,704,746,788,115,170,224,277,329,380,429,477,525,571,616,661,704,746,788,829,170,224,277,329,380,429,477,525,571,616,661,704,746,788,829,869,224,277,329,380,429,477,525,571,616,661,704,746,788,829,869,908,277,329,380,429,477,525,571,616,661,704,746,788,829,869,908,946,329,380,429,477,525,571,616,661,704,746,788,829,869,908,946,984,380,429,477,525,571,616,661,704,746,788,829,869,908,946,984,1021,429,477,525,571,616,661,704,746,788,829,869,908,946,984,1021,1057,477,525,571,616,661,704,746,788,829,869,908,946,984,1021,1057,1092,525,571,616,661,704,746,788,829,869,908,946,984,1021,1057,1092,1127,571,616,661,704,746,788,829,869,908,946,984,1021,1057,1092,1127,1161,616,661,704,746,788,829,869,908,946,984,1021,1057,1092,1127,1161,1195,661,704,746,788,829,869,908,946,984,1021,1057,1092,1127,1161,1195,1228,704,746,788,829,869,908,946,984,1021,1057,1092,1127,1161,1195,1228,1260,746,788,829,869,908,946,984,1021,1057,1092,1127,1161,1195,1228,1260,1292};     /* sq_mix[l1+l2] indexed by (l1<<4)|l2 */
static const uint8_t tri16[32] = {240,224,208,192,176,160,144,128,112,96,80,64,48,32,16,0,0,16,32,48,64,80,96,112,128,144,160,176,192,208,224,240};   /* triangle level << 4 */
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

/* ------------------------------------------------------------------ events
 * Queued as one 32 bit word per level change: (t << 8) | field, t = bus cycle (low 24 bits), field = level in its mixer-index position
 * (pulse 1 / triangle: level << 4, pulse 2 / noise: level; the mixer tables are indexed (pulse1<<4|pulse2) and (tri<<4|noise)).  One channel's events are
 * in time order; the 'last' member of a channel holds the field of its last event (also when that event was dropped). */
static inline uint32_t ev_key(uint32_t t_bus, unsigned f){ return (t_bus<<8)|f; }
static inline unsigned sq_field(int ch, unsigned lvl){ return ch ? lvl : lvl<<4; }
static uint32_t *qarr(Apu *a, int ch){ return ch==0 ? a->qsq0 : ch==1 ? a->qsq1 : ch==2 ? a->qtri : a->qnoi; }
static uint32_t qcap(int ch){ return ch<2 ? APU_Q_SQ : ch==2 ? APU_Q_TRI : APU_Q_NOI; }
/* make room for `need` events in queue ch (drained queue restarts at 0, a partly consumed one moves down); returns the free slots */
static uint32_t q_room(Apu *a, int ch, uint32_t need){
    uint32_t cap=qcap(ch), h=a->qh[ch], t=a->qt[ch];
    if(h==t){ a->qh[ch]=a->qt[ch]=0; return cap; }
    if(cap-t<need && h){ uint32_t *q=qarr(a,ch), i; for(i=h;i<t;i++) q[i-h]=q[i]; a->qt[ch]=t-h; a->qh[ch]=0; return cap-(t-h); }
    return cap-t;
}
static void ev_push(Apu *a, int ch, uint32_t t_bus, unsigned f){
    if(!q_room(a,ch,1)){ a->dropped++; return; }
    qarr(a,ch)[a->qt[ch]++]=ev_key(t_bus,f);
}
static inline void emit(Apu *a, int ch, uint64_t tint, unsigned f, uint8_t *last){
    if(f==*last) return; *last=(uint8_t)f; ev_push(a,ch,(uint32_t)tint-1,f);
}

/* ------------------------------------------------------------------ shared pieces */
static inline unsigned env_vol(const ApuLen *l, const ApuEnv *e){ return l->len ? (e->constvol ? e->vol : e->counter) : 0; }
static inline int sq_muted(const ApuPulse *s){ return s->real_period<8 || (!s->sw_negate && s->sw_target>0x7FF); }
static inline unsigned sq_level(const ApuPulse *s, unsigned pos){ return (sq_muted(s) || !((duty_mask[s->duty]>>pos)&1)) ? 0 : env_vol(&s->l,&s->e); }

/* catch a channel up to internal cycle `target`: timer skip-ahead in closed form, level changes are queued as events.
 * Mirrors BaseApuChannel::Run: clocks happen at sync+timer+1, then every period+1 cycles.  All time arithmetic is 32 bit (the events keep 24 bits). */
static void sq_sync(Apu *a, ApuPulse *s, int ch, uint64_t target){
    uint32_t n=(uint32_t)(target-s->sync), P, first, k, lastoff, V, j, t, room, *w; unsigned pos, last, f, Vf;
    uint32_t base=(uint32_t)s->sync;
    s->sync=target;
    if(!a->audio || !n) return;
    if(n<=s->timer){ s->timer-=n; return; }
    P=s->period+1; first=s->timer+1; k=1+(n-first)/P; lastoff=first+(k-1)*P; s->timer=s->period-(n-lastoff);
    V = sq_muted(s) ? 0 : env_vol(&s->l,&s->e);
    pos=s->dutypos; last=s->last; Vf=sq_field(ch,V);
    if(!V){ s->dutypos=(uint8_t)((pos-k)&7); emit(a,ch,(uint64_t)base+first,0,&s->last); return; }
    room=q_room(a,ch,k); w=qarr(a,ch)+a->qt[ch]; t=base+first-1;
    { uint32_t dm=duty_mask[s->duty];
      for(j=0;j<k;j++,t+=P){ pos=(pos-1)&7; f=((dm>>pos)&1)?Vf:0; if(f!=last){ last=f; if(room){ room--; *w++=ev_key(t,f); } else a->dropped++; } } }
    s->last=(uint8_t)last; s->dutypos=(uint8_t)pos; a->qt[ch]=(uint32_t)(w-qarr(a,ch));
}
static void tri_sync(Apu *a, ApuTri *s, uint64_t target){
    uint32_t n=(uint32_t)(target-s->sync), P, first, k, lastoff, j, t, room, *w; unsigned seq, last, f;
    uint32_t base=(uint32_t)s->sync;
    s->sync=target;
    if(!a->audio || !n) return;
    if(n<=s->timer){ s->timer-=n; return; }
    P=s->period+1; first=s->timer+1; k=1+(n-first)/P; lastoff=first+(k-1)*P; s->timer=s->period-(n-lastoff);
    if(!(s->l.len>0 && s->lin>0)) return;
    seq=s->seq; last=s->last; room=q_room(a,2,k); w=a->qtri+a->qt[2]; t=base+first-1;
    for(j=0;j<k;j++,t+=P){ seq=(seq+1)&31; f=tri16[seq]; if(f!=last){ last=f; if(room){ room--; *w++=ev_key(t,f); } else a->dropped++; } }
    s->last=(uint8_t)last; s->seq=(uint8_t)seq; a->qt[2]=(uint32_t)(w-a->qtri);
}
static void noi_sync(Apu *a, ApuNoise *s, uint64_t target){
    uint32_t n=(uint32_t)(target-s->sync), P, first, k, lastoff, V, j, t, room, *w; unsigned sr, sh, last, f;
    uint32_t base=(uint32_t)s->sync;
    s->sync=target;
    if(!a->audio || !n) return;
    if(n<=s->timer){ s->timer-=n; return; }
    P=s->period+1; first=s->timer+1; k=1+(n-first)/P; lastoff=first+(k-1)*P; s->timer=s->period-(n-lastoff);
    V=env_vol(&s->l,&s->e);
    if(!V){ lfsr_skip(s,k); emit(a,3,(uint64_t)base+first,0,&s->last); return; }
    sr=s->sr; sh=s->mode?6:1; last=s->last; room=q_room(a,3,k); w=a->qnoi+a->qt[3]; t=base+first-1;
    for(j=0;j<k;j++,t+=P){ sr=(sr>>1)|(((sr^(sr>>sh))&1)<<14); f=(sr&1)?0:V; if(f!=last){ last=f; if(room){ room--; *w++=ev_key(t,f); } else a->dropped++; } }
    s->last=(uint8_t)last; s->sr=(uint16_t)sr; a->qt[3]=(uint32_t)(w-a->qnoi);
}
static void sync_all(Apu *a, uint64_t t){
    if(!a->audio) return;          /* no sequencers simulated (cue mode, or RT_AUDIO=0): apu_set_audio(on) restarts them from the then current cycle */
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
    uint8_t s; PT0;
    apu_run_to(a,cc);
    s=(uint8_t)((a->sq[0].l.len>0) | ((a->sq[1].l.len>0)<<1) | ((a->tri.l.len>0)<<2) | ((a->noi.l.len>0)<<3) | (a->irq<<6));
    a->irq=0; PT1(apu_prof_r);
    return s;
}

void apu_write(Apu *a, uint16_t addr, uint8_t v, uint64_t cc){
    uint64_t T; ApuPulse *s; PT0;
    apu_run_to(a,cc); T=a->cyc;
    if(addr<0x4008){
        int ch=(addr>>2)&1; s=&a->sq[ch];
        if(a->audio) sq_sync(a,s,ch,T);
        switch(addr&3){
        case 0: len_init(a,&s->l,(v>>5)&1); s->e.constvol=(v>>4)&1; s->e.vol=v&15; s->duty=v>>6; break;
        case 1: s->sw_enable=v>>7; s->sw_negate=(v>>3)&1; s->sw_period=((v>>4)&7)+1; s->sw_shift=v&7; sq_target(s); s->sw_reload=1; break;
        case 2: sq_setperiod(s,(s->real_period&0x700)|v); break;
        default: len_load(a,&s->l,v>>3); sq_setperiod(s,(s->real_period&0xFF)|((v&7)<<8)); s->dutypos=0; s->e.start=1; break;
        }
        if(a->audio) emit(a,ch,T,sq_field(ch,sq_level(s,s->dutypos)),&s->last);
    } else if(addr<0x400C){
        if(a->audio) tri_sync(a,&a->tri,T);
        switch(addr&3){
        case 0: a->tri.lin_ctrl=v>>7; a->tri.lin_reload=v&0x7F; len_init(a,&a->tri.l,a->tri.lin_ctrl); break;
        case 2: a->tri.period=(a->tri.period&~0xFFu)|v; break;
        case 3: len_load(a,&a->tri.l,v>>3); a->tri.period=(a->tri.period&~0xFF00u)|((uint32_t)(v&7)<<8); a->tri.lin_flag=1; break;
        }
    } else if(addr<0x4010){
        if(a->audio) noi_sync(a,&a->noi,T);
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
    PT1(apu_prof_w);
#ifdef APU_PROF
    apu_prof_n++;
#endif
}


/* ------------------------------------------------------------------ rendering */
/* full cycle count of a queued event: the queue keeps 24 bits, the events are never older than 2^23 cycles before the APU's current position */
static inline uint32_t ev_time(uint32_t key, uint32_t ref){ uint32_t t=(ref&~0xFFFFFFu)|(key>>8); return t>ref ? t-0x1000000u : t; }

int apu_pop_event(Apu *a, uint64_t until, uint32_t *t, int *ch, int *lvl){
    const uint32_t *qs[4]; int c, best=-1; int32_t bo=0; uint32_t u=(uint32_t)until, ref=(uint32_t)a->cyc, bt=0, bf=0;
    qs[0]=a->qsq0; qs[1]=a->qsq1; qs[2]=a->qtri; qs[3]=a->qnoi;
    for(c=0;c<4;c++) if(a->qh[c]!=a->qt[c]){
        uint32_t key=qs[c][a->qh[c]], et=ev_time(key,ref); int32_t o=(int32_t)(et-u);
        if(best<0 || o<bo){ best=c; bo=o; bt=et; bf=key&255; }
    }
    if(best<0 || bo>=0) return 0;
    *t=bt; *lvl=(best==0||best==2) ? (int)(bf>>4) : (int)(bf&15); *ch=best; a->qh[best]++;
    return 1;
}

#define NOEV 0x40000000
/* Rendering works on the level changes only (a few hundred per frame, thousands in the busiest music):
 *   merge():  the four queues -> one time ordered list of 32 bit entries (t << 16 | mixer value), t relative to the start of the render window (< 65536);
 *   core():   walks the sample boundaries (Bresenham in 32 bit fixed point, exact) and integrates the entries; samples that contain no entry are one table value.
 * render_slow() is the straightforward version for any window (< 2^22 cycles) and any sample count; the fast path must give the identical result. */
#define MRG_CHUNK 256
static unsigned merge(Apu *a, uint32_t *dst, unsigned max, uint32_t base, uint32_t limit){
    uint32_t *q[4], h[4]; int32_t k[4]; unsigned sq=a->rsq, tn=a->rtn, n=0, c;
    q[0]=a->qsq0; q[1]=a->qsq1; q[2]=a->qtri; q[3]=a->qnoi;
    for(c=0;c<4;c++){ h[c]=a->qh[c]; k[c]=(int32_t)(q[c][h[c]]-base); }
    while(n<max){
        int32_t bk=k[0]; unsigned f, mix;
        c=0; if(k[1]<bk){ bk=k[1]; c=1; } if(k[2]<bk){ bk=k[2]; c=2; } if(k[3]<bk){ bk=k[3]; c=3; }
        if((uint32_t)bk>=limit) break;
        f=q[c][h[c]]&255;
        switch(c){ case 0: sq=(sq&0x0F)|f; break; case 1: sq=(sq&0xF0)|f; break; case 2: tn=(tn&0x0F)|f; break; default: tn=(tn&0xF0)|f; break; }
        mix=(unsigned)sq2_mix[sq]+tnd_mix[tn];
        dst[n++]=(((uint32_t)bk<<8)&0xFFFF0000u)|mix;
        h[c]++; k[c]=(int32_t)(q[c][h[c]]-base);
    }
    for(c=0;c<4;c++) a->qh[c]=h[c];
    a->rsq=(uint8_t)sq; a->rtn=(uint8_t)tn;
    return n;
}

typedef struct { uint32_t S, cur, b, acc, mix, i, n, q, f, c, rec[2], pb; uint8_t *o8; int16_t *o16; } Core;
static inline uint32_t smp_conv(uint32_t S, uint32_t rec){ return (uint32_t)(((uint64_t)S*rec)>>32); }
static inline uint32_t pure_byte(uint32_t mix, int s8){ return s8 ? (((mix*83u)>>10)-128u)&255u : mix; }
static inline void core_step(Core *z){ uint32_t o=z->acc; z->acc+=z->f; z->c=z->acc<o; z->b+=z->q+z->c; }
/* feed the entries of ev[0..cnt) (t, new mixer value) in; returns 1 when all n samples are complete */
static int core(Core *z, const uint32_t *ev, unsigned cnt, int s8){
    unsigned e;
    for(e=0;e<cnt;e++){
        uint32_t t=ev[e]>>16, m=ev[e]&0xFFFFu;
        if(t>=z->b){
            uint32_t S=z->S+z->mix*(z->b-z->cur), v=smp_conv(S,z->rec[z->c]);
            if(s8) z->o8[z->i]=(uint8_t)(((v*83u)>>10)-128u); else z->o16[z->i]=(int16_t)v;
            z->cur=z->b; z->S=0; z->i++; if(z->i>=z->n) return 1;
            core_step(z);
            z->pb=pure_byte(z->mix,s8);
            while(t>=z->b){
                if(s8) z->o8[z->i]=(uint8_t)z->pb; else z->o16[z->i]=(int16_t)z->pb;
                z->cur=z->b; z->i++; if(z->i>=z->n) return 1;
                core_step(z);
            }
        }
        z->S+=z->mix*(t-z->cur); z->cur=t; z->mix=m;
    }
    return 0;
}

static uint32_t recip_ceil32(uint32_t r, uint32_t n){        /* ceil(r * 2^32 / n) for r < n < 65536 using 32 bit divisions only */
    uint32_t x=r<<16, d1=x/n, r1=x%n, y=r1<<16, d2=y/n, r2=y%n, f=(d1<<16)|d2;
    return f+(r2!=0);
}

static void render_slow(Apu *a, uint64_t from, uint64_t to, void *out, unsigned n, int s8){
    uint64_t span=to-from; uint32_t base=(uint32_t)from<<8, pos=0, i, c; uint32_t *q[4], h[4]; int32_t k[4]; unsigned sq=a->rsq, tn=a->rtn, mix;
    q[0]=a->qsq0; q[1]=a->qsq1; q[2]=a->qtri; q[3]=a->qnoi;
    for(c=0;c<4;c++){ h[c]=a->qh[c]; q[c][a->qt[c]]=base+NOEV; k[c]=(int32_t)(q[c][h[c]]-base); }
    for(;;){                                  /* events before `from` that were never rendered: apply */
        int32_t bk=k[0]; unsigned f;
        c=0; if(k[1]<bk){ bk=k[1]; c=1; } if(k[2]<bk){ bk=k[2]; c=2; } if(k[3]<bk){ bk=k[3]; c=3; }
        if(bk>=0) break;
        f=q[c][h[c]]&255;
        switch(c){ case 0: sq=(sq&0x0F)|f; break; case 1: sq=(sq&0xF0)|f; break; case 2: tn=(tn&0x0F)|f; break; default: tn=(tn&0xF0)|f; break; }
        h[c]++; k[c]=(int32_t)(q[c][h[c]]-base);
    }
    mix=(unsigned)sq2_mix[sq]+tnd_mix[tn];
    for(i=0;i<n;i++){
        uint32_t end=(uint32_t)((span*(i+1))/n), len=end-pos, cur=pos, sum=0, v; uint64_t rec;
        for(;;){
            int32_t bk=k[0]; unsigned f;
            c=0; if(k[1]<bk){ bk=k[1]; c=1; } if(k[2]<bk){ bk=k[2]; c=2; } if(k[3]<bk){ bk=k[3]; c=3; }
            if((uint32_t)(bk>>8)>=end) break;
            sum+=mix*((uint32_t)(bk>>8)-cur); cur=(uint32_t)(bk>>8);
            f=q[c][h[c]]&255;
            switch(c){ case 0: sq=(sq&0x0F)|f; break; case 1: sq=(sq&0xF0)|f; break; case 2: tn=(tn&0x0F)|f; break; default: tn=(tn&0xF0)|f; break; }
            mix=(unsigned)sq2_mix[sq]+tnd_mix[tn];
            h[c]++; k[c]=(int32_t)(q[c][h[c]]-base);
        }
        sum+=mix*(end-cur); pos=end;
        rec=(0x100000000ull+len-1)/len; v=(uint32_t)(((uint64_t)sum*rec)>>32);
        if(s8) ((int8_t*)out)[i]=(int8_t)(((v*83u)>>10)-128u); else ((int16_t*)out)[i]=(int16_t)v;
    }
    for(c=0;c<4;c++) a->qh[c]=h[c];
    a->rsq=(uint8_t)sq; a->rtn=(uint8_t)tn;
}

static void render(Apu *a, uint64_t from, uint64_t to, void *out, unsigned n, int s8){
    uint64_t span;
    if(!n || to<=from) return;
    { PT0; apu_flush(a,to); PT1(apu_prof_flush); }
    span=to-from;
    if(span>=0x10000u || n>=0x10000u || span/n<2){ render_slow(a,from,to,out,n,s8); }
    else {
        static uint32_t buf[MRG_CHUNK+1];
        uint32_t base=(uint32_t)from<<8, sp=(uint32_t)span, q=sp/n, r=sp%n, *qs[4]; Core z; unsigned c, cnt, done=0;
        qs[0]=a->qsq0; qs[1]=a->qsq1; qs[2]=a->qtri; qs[3]=a->qnoi;
        for(c=0;c<4;c++){ uint32_t *p=qs[c]; p[a->qt[c]]=base+NOEV;
            while(a->qh[c]!=a->qt[c] && (int32_t)(p[a->qh[c]]-base)<0){ unsigned f=p[a->qh[c]]&255; a->qh[c]++;      /* events before `from` that were never rendered: apply */
                if(c==0) a->rsq=(uint8_t)((a->rsq&0x0F)|f); else if(c==1) a->rsq=(uint8_t)((a->rsq&0xF0)|f); else if(c==2) a->rtn=(uint8_t)((a->rtn&0x0F)|f); else a->rtn=(uint8_t)((a->rtn&0xF0)|f); } }
        z.S=0; z.cur=0; z.acc=0; z.i=0; z.n=n; z.q=q; z.f=r?recip_ceil32(r,n):0; z.c=0; z.b=0;
        z.rec[0]=0xFFFFFFFFu/q+1; z.rec[1]=0xFFFFFFFFu/(q+1)+1;
        z.mix=(unsigned)sq2_mix[a->rsq]+tnd_mix[a->rtn]; z.o8=(uint8_t*)out; z.o16=(int16_t*)out;
        core_step(&z);
        for(;;){
            PT0; int fin;
            cnt=merge(a,buf,MRG_CHUNK,base,sp<<8); PT1(apu_prof_merge);
            if(cnt<MRG_CHUNK){ buf[cnt++]=(sp<<16)|(z.mix&0xFFFFu); done=1; }     /* closing entry at the end of the window: no change of the level */
            { PT0; fin=core(&z,buf,cnt,s8); PT1(apu_prof_core); }
            if(fin || done) break;
        }
    }
    {   /* events beyond the window stay: move them to the front */
        unsigned c; for(c=0;c<4;c++){ uint32_t *q=qarr(a,c), h=a->qh[c], t=a->qt[c], i; if(h){ for(i=h;i<t;i++) q[i-h]=q[i]; a->qt[c]=t-h; a->qh[c]=0; } }
    }
    a->rpos=to;
}
void apu_render16(Apu *a, uint64_t from, uint64_t to, int16_t *out, unsigned n){ render(a,from,to,out,n,0); }
void apu_render8(Apu *a, uint64_t from, uint64_t to, int8_t *out, unsigned n){ render(a,from,to,out,n,1); }

/* ------------------------------------------------------------------ cue mode (see apu.h, docs/APU.md "Cue mode")
 * Wave ids: 0..11 pulse (duty*3 + {1,2,4} steps per sample), 12..15 triangle (1,2,4,8 steps per sample), 16..20 noise long mode (every 1,2,4,8,16th LFSR step),
 * 21..25 noise short mode (same strides).  One waveform sample = one or more sequencer steps, played by Paula at the period that keeps the NES pitch. */
#define CUE_NOISE_LEN 4096
static int8_t cue_pulse[12][8] __attribute__((aligned(4)));
static int8_t cue_tri[4][32] __attribute__((aligned(4)));
static int8_t cue_noil[5][CUE_NOISE_LEN] __attribute__((aligned(4)));
static int8_t cue_nois[5][186] __attribute__((aligned(4)));
static ApuCueWave cue_wave[APU_CUE_NWAVE];
static uint8_t cue_vol_sq[16], cue_vol_tri, cue_vol_noi[16];
static uint32_t cue_k[2];            /* Paula colour clocks per CPU cycle in 16.16, PAL / NTSC */
static uint8_t cue_ready;
#define CUE_VOL(mix) ((unsigned)(((uint32_t)(mix)*5312u+130048u)/260096u))    /* mixer units -> Paula volume: peak-to-peak (mix*83/1024 counts) of a +-127 wave at volume 64 is 254 counts */

void apu_cue_init(void){
    static const uint8_t tseq[32]={15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    unsigned i,j,d,k,m;
    if(cue_ready) return;
    lfsr_init();
    for(m=0;m<4;m++) for(d=0;d<3;d++){          /* pulse: step t of the sequence is bit 7-t of the duty mask; samples average 1,2,4 steps */
        unsigned n=8>>d, st=1u<<d; int8_t *w=cue_pulse[m*3+d];
        for(i=0;i<n;i++){ int sum=0; for(j=0;j<st;j++) sum+=((duty_mask[m]>>(7-(i*st+j)))&1)?127:-127; w[i]=(int8_t)(sum/(int)st); }
        cue_wave[m*3+d].data=w; cue_wave[m*3+d].words=(uint16_t)((n+1)/2);
    }
    for(d=0;d<4;d++){                            /* triangle: steps scaled so that the wave spans +-127 through the (non-linear) tnd table of the triangle alone */
        unsigned n=32>>d, st=1u<<d; int8_t *w=cue_tri[d];
        for(i=0;i<n;i++){ int sum=0; for(j=0;j<st;j++){ unsigned l=tseq[i*st+j]; sum+=(int)((254u*tnd_mix[l<<4]+tnd_mix[240]/2)/tnd_mix[240])-127; } w[i]=(int8_t)(sum/(int)st); }
        cue_wave[12+d].data=w; cue_wave[12+d].words=(uint16_t)(n/2);
    }
    for(d=0;d<5;d++){                            /* noise long mode: bit after step j*stride of the 32767 cycle (first 4096 samples of it) */
        unsigned stride=1u<<d; int8_t *w=cue_noil[d];
        for(i=0;i<CUE_NOISE_LEN;i++){ uint32_t p=((uint32_t)i*stride+1)%32767u; w[i]=(lfsr_seq[p]&1)?-127:127; }
        cue_wave[16+d].data=w; cue_wave[16+d].words=CUE_NOISE_LEN/2;
        /* short mode: cycle of 93 states, two periods = 186 samples (even length) */
        { int8_t *s=cue_nois[d]; uint16_t sr=1; uint8_t seq[93]; for(k=0;k<93;k++){ sr=(uint16_t)((sr>>1)|(((sr^(sr>>6))&1)<<14)); seq[k]=(uint8_t)(sr&1); }
          for(i=0;i<186;i++) s[i]=seq[(i*stride)%93]?-127:127;
          cue_wave[21+d].data=s; cue_wave[21+d].words=93; }
    }
    for(i=0;i<16;i++){ cue_vol_sq[i]=(uint8_t)CUE_VOL(sq_mix[i]); cue_vol_noi[i]=(uint8_t)CUE_VOL(tnd_mix[i]); }
    cue_vol_tri=(uint8_t)CUE_VOL(tnd_mix[240]);
    cue_k[0]=(uint32_t)(((uint64_t)3546895u<<16)/1789773u); cue_k[1]=(uint32_t)(((uint64_t)3579545u<<16)/1789773u);
    cue_ready=1;
}
const ApuCueWave *apu_cue_wave(unsigned id){ return &cue_wave[id]; }

/* Paula period for `cyc` CPU cycles per waveform sample; scale = 1 << (number of doublings) steps per sample is chosen by the caller */
/* cyc * K with K = 1 + kf/65536 (1.98): 32 bit arithmetic only, cyc < 65536 here (largest: noise period 4068 * 16) */
static inline unsigned cue_per(uint32_t cyc, uint32_t kf){ return (unsigned)(cyc + ((cyc*kf+32768u)>>16)); }
void apu_cue_frame(Apu *a, uint64_t cyc, unsigned pal, ApuCue *o){
    uint32_t kfp=cue_k[pal?0:1]-65536u; unsigned c, d, per, V;
    { PT0; apu_run_to(a, cyc ? cyc-1 : 0); PT1(apu_prof_flush); }
    o->hi=0;
    for(c=0;c<2;c++){                             /* pulse */
        const ApuPulse *s=&a->sq[c]; uint32_t T=s->period+1;
        V = sq_muted(s) ? 0 : env_vol(&s->l,&s->e);
        for(d=0;d<3;d++){ per=cue_per(T<<d,kfp); if(per>=124) break; }
        if(d==3){ d=2; per=124; if(V) o->hi|=(uint8_t)(1u<<c); V=0; }
        o->ch[c].per=(uint16_t)(per>65535?65535:per); o->ch[c].wave=(uint8_t)(s->duty*3+d); o->ch[c].vol=V?cue_vol_sq[V]:0;
    }
    {   const ApuTri *s=&a->tri; uint32_t T=s->period+1; unsigned on=(s->l.len>0 && s->lin>0);
        for(d=0;d<4;d++){ per=cue_per(T<<d,kfp); if(per>=124) break; }
        if(d==4){ d=3; per=124; on=0; if(s->l.len>0 && s->lin>0) o->hi|=4; }
        o->ch[2].per=(uint16_t)(per>65535?65535:per); o->ch[2].wave=(uint8_t)(12+d); o->ch[2].vol=on?cue_vol_tri:0;
    }
    {   const ApuNoise *s=&a->noi; uint32_t T=s->period+1; V=env_vol(&s->l,&s->e);
        for(d=0;d<5;d++){ per=cue_per(T<<d,kfp); if(per>=124) break; }
        if(d==5){ d=4; per=124; }
        o->ch[3].per=(uint16_t)(per>65535?65535:per); o->ch[3].wave=(uint8_t)((s->mode?21:16)+d); o->ch[3].vol=V?cue_vol_noi[V]:0;
    }
}
