/* arender.c - planar rendering of the NES picture from the scene log (see arender.h, docs/RENDER.md).  Target independent. */
#pragma GCC optimize ("no-strict-aliasing")
#include "arender.h"
/* no libc calls (freestanding Amiga build): byte loops */
static void ar_set(void*d,int v,unsigned n){ uint8_t*p=d; if(!((uintptr_t)p&3)){ uint32_t w=(uint8_t)v; w|=w<<8; w|=w<<16; uint32_t*q=(uint32_t*)p; while(n>=4){ *q++=w; n-=4; } p=(uint8_t*)q; } while(n--) *p++=(uint8_t)v; }
static void ar_cpy(void*d,const void*s,unsigned n){ uint8_t*p=d; const uint8_t*q=s; if(!(((uintptr_t)p|(uintptr_t)q)&3)){ uint32_t*pl=(uint32_t*)p; const uint32_t*ql=(const uint32_t*)q; while(n>=4){ *pl++=*ql++; n-=4; } p=(uint8_t*)pl; q=(const uint8_t*)ql; } while(n--) *p++=*q++; }
static int ar_cmp(const void*a,const void*b,unsigned n){ const uint8_t*p=a,*q=b; if(!(((uintptr_t)p|(uintptr_t)q)&3)){ const uint32_t*pl=(const uint32_t*)p,*ql=(const uint32_t*)q; while(n>=4){ if(*pl!=*ql) break; pl++; ql++; n-=4; } p=(const uint8_t*)pl; q=(const uint8_t*)ql; } while(n--){ if(*p!=*q) return *p-*q; p++; q++; } return 0; }

unsigned long long ar_ns_total, ar_ns_exact;
#ifdef AR_TIMING
#include <time.h>
static unsigned long long ar_t0;
static unsigned long long ar_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (unsigned long long)t.tv_sec*1000000000ull+t.tv_nsec; }
#define AR_T0() (ar_t0=ar_now())
#define AR_T1() (ar_ns_total+=ar_now()-ar_t0)
#else
#define AR_T0()
#define AR_T1()
#endif
long ar_st_rb[2], ar_st_cfgmiss, ar_st_calls;
ArScene ar_scene; ArFrame ar_out;
static uint8_t ar_planes[5][240*AR_STRIDE] __attribute__((aligned(4)));

/* ---- persistent background bitmaps ------------------------------------------------------------------------------------------------------ */
typedef struct {
    int valid; uint8_t bank, mirror; uint32_t stamp;
    uint32_t bv[4][32]; uint8_t bvok[4][32]; uint16_t um[4][32]; uint16_t sh[4][32][32]; uint8_t src[4][32][40] __attribute__((aligned(4))); uint8_t pm[256]; uint32_t gen;          /* write-counter sum of the source rows the bitmap tile row was built from */
} ArCfg;
static ArCfg cfgs[AR_MAX_CFG];
ArHw ar_hw; int ar_hw_enable;
#ifndef AR_NO_STATIC
static uint8_t hw_static[AR_HW_BYTES] __attribute__((aligned(4)));
#endif
static uint8_t*hw_base; static uint8_t*hw_xsets;      /* hw_base: bitmaps; hw_xsets: 2 sets of (spr, xpf1, xpf2) */
static uint8_t spr_sh[2][4][240][AR_STRIDE] __attribute__((aligned(4)));     /* fast RAM copies of the two sprite layer sets: only the long words that differ are written to the (slow) chip RAM */
static uint32_t spr_sg[2][240][12], spr_su[2][240]; static uint16_t spr_ext[2][240]; static int hw_zeroed; static uint32_t tmpl[4][9] __attribute__((aligned(4)));   /* spr_ext: byte extent lo | hi<<8 of the non-zero part of a row (0x00FF: empty) */
void ar_hw_setmem(void*chip){ hw_base=chip; }
static void hw_mem(void){ if(!hw_base){
#ifndef AR_NO_STATIC
    hw_base=hw_static;
#endif
  } ar_hw.bmp=hw_base; hw_xsets=hw_base+AR_MAX_CFG*4*AR_BROWS*AR_BROW; }
#define BMP(c,k,row) AR_BMP(c,k,row)
static void c2p_init(void); static uint32_t stamp_ctr; static uint8_t bitrev(uint8_t b); static void hw_select(void); static uint8_t fliptab[256];

static uint16_t nt_phys(int mirror,int nt){     /* physical offset of logical nametable nt (0..3) */
    uint16_t o=0; uint16_t a=(uint16_t)(nt*0x400);
    switch(mirror){ case 0: o=0; break; case 1: o=0x400; break; case 2: o=(uint16_t)(a&0x400); break; default: o=(uint16_t)((a&0x800)>>1); }
    return o;
}
void ar_init(void){ c2p_init(); for(int i=0;i<256;i++) fliptab[i]=bitrev((uint8_t)i); hw_mem(); hw_select(); ar_set(cfgs,0,sizeof cfgs); stamp_ctr=0; }
static int get_cfg(int bank,int mirror){
    int best=0; uint32_t bs=0xFFFFFFFFu;
    for(int i=0;i<AR_MAX_CFG;i++){
        if(cfgs[i].valid && cfgs[i].bank==bank && cfgs[i].mirror==mirror){ cfgs[i].stamp=++stamp_ctr; return i; }
        uint32_t s=cfgs[i].valid? cfgs[i].stamp : 0; if(s<bs){ bs=s; best=i; }
    }
    ar_st_cfgmiss++; ArCfg*c=&cfgs[best]; ar_set(c->bvok,0,sizeof c->bvok); ar_set(c->pm,0xFF,sizeof c->pm); c->valid=1; c->bank=(uint8_t)bank; c->mirror=(uint8_t)mirror; c->stamp=++stamp_ctr;
    return best;
}
/* make tile row cy of logical nametable nt current in bitmap c: only the tiles whose name table byte / palette bits differ from what the bitmap shows are redrawn */
long ar_st_tiles;
static __attribute__((noinline)) void ensure_trow(Nes*n,int c,int nt,int cy){
    ar_st_calls++; ArCfg*g=&cfgs[c]; Ppu*p=&n->ppu; uint16_t base=nt_phys(g->mirror,nt);
    uint32_t sum=p->ntv[(base+cy*32)>>5]+p->ntv[(base+0x3C0+(cy>>2)*8)>>5];
    if(g->bvok[nt][cy] && g->bv[nt][cy]==sum) return;
    ar_st_rb[g->bvok[nt][cy]?1:0]++;
    const uint8_t*chr=n->map.chr+(uint32_t)g->bank*4096u; int ny=nt>>1, nx=nt&1;
    const uint8_t*ntp=p->nt+base+cy*32,*atp=p->nt+base+0x3C0+(cy>>2)*8; int ash=(cy&2)<<1;
    uint16_t*sh=g->sh[nt][cy]; int fresh=!g->bvok[nt][cy]; uint16_t um=0, nch=0;
    uint32_t*sl=(uint32_t*)ntp; uint32_t*dl=(uint32_t*)g->src[nt][cy]; uint8_t*da=g->src[nt][cy]+32;   /* what the bitmap row was built from: 32 tile bytes + 8 attribute bytes */
    uint8_t*row0=BMP(c,0,ny*256+cy*8)+nx*32; const unsigned PS=AR_BROWS*AR_BROW;
    int ngrp=0; if(!fresh) for(int gp=0;gp<8;gp++) ngrp+=(sl[gp]!=dl[gp] || atp[gp]!=da[gp]);
    if(fresh || ngrp>=3){                        /* many tiles: the row is rewritten with long words from fast RAM buffers (chip RAM writes are slow) */
        uint32_t b2l[8], b3l[8], b0l[8], b1l[8]; uint8_t*b2=(uint8_t*)b2l,*b3=(uint8_t*)b3l,*b0=(uint8_t*)b0l,*b1=(uint8_t*)b1l; const uint8_t*tp[32];
        for(unsigned x=0;x<32;x++){
            unsigned ti=ntp[x], pb=(atp[x>>2]>>(ash|(x&2)))&3; sh[x]=(uint16_t)(ti|(pb<<8)); tp[x]=chr+ti*16; b2[x]=(pb&1)?0xFF:0; b3[x]=(pb&2)?0xFF:0;
            unsigned pm=g->pm[ti];
            if(pm==0xFF){ uint8_t p0=0,p1=0,p2=0,p3=0; const uint8_t*t=chr+ti*16; for(int fy=0;fy<8;fy++){ uint8_t a=t[fy], b=t[fy+8]; p0|=(uint8_t)~(a|b); p1|=(uint8_t)(a&~b); p2|=(uint8_t)(~a&b); p3|=(uint8_t)(a&b); }
                g->pm[ti]=(uint8_t)((p0?1:0)|(p1?2:0)|(p2?4:0)|(p3?8:0)); } }
        for(int gp=0;gp<8;gp++){ dl[gp]=sl[gp]; da[gp]=atp[gp]; }
        int two=(nx==0);
        for(int fy=0;fy<8;fy++){
            for(int x=0;x<32;x++){ b0[x]=tp[x][fy]; b1[x]=tp[x][fy+8]; }
            uint32_t*c0=(uint32_t*)(row0+fy*AR_BROW),*c1=(uint32_t*)(row0+PS+fy*AR_BROW),*c2=(uint32_t*)(row0+2*PS+fy*AR_BROW),*c3=(uint32_t*)(row0+3*PS+fy*AR_BROW);
            for(int l=0;l<8;l++){ c0[l]=b0l[l]; c1[l]=b1l[l]; c2[l]=b2l[l]; c3[l]=b3l[l]; }
            if(two) for(int l=0;l<8;l++){ c0[16+l]=b0l[l]; c1[16+l]=b1l[l]; c2[16+l]=b2l[l]; c3[16+l]=b3l[l]; }
        }
        ar_st_tiles+=32; g->gen++;
        um=0; for(int x=0;x<32;x++){ unsigned pb=sh[x]>>8, pm=g->pm[sh[x]&255]; um|=(uint16_t)((pm&1)|((pm>>1)<<(4*pb+1))); }
        g->bv[nt][cy]=sum; g->bvok[nt][cy]=1; g->um[nt][cy]=um; return;
    }
    for(int gp=0;gp<8;gp++){
        if(!fresh && sl[gp]==dl[gp] && atp[gp]==da[gp]) continue;
        dl[gp]=sl[gp]; da[gp]=atp[gp];
        for(int x=gp*4;x<gp*4+4;x++){
            unsigned ti=ntp[x], pb=(atp[x>>2]>>(ash|(x&2)))&3; uint16_t v=(uint16_t)(ti|(pb<<8));
            unsigned pm=g->pm[ti];
            if(pm==0xFF){ uint8_t p0=0,p1=0,p2=0,p3=0; const uint8_t*t=chr+ti*16; for(int fy=0;fy<8;fy++){ uint8_t a=t[fy], b=t[fy+8]; p0|=(uint8_t)~(a|b); p1|=(uint8_t)(a&~b); p2|=(uint8_t)(~a&b); p3|=(uint8_t)(a&b); }
                pm=(p0?1:0)|(p1?2:0)|(p2?4:0)|(p3?8:0); g->pm[ti]=(uint8_t)pm; }
            if(!fresh && sh[x]==v) continue;
            unsigned ov=sh[x];
            sh[x]=v; nch++; um|=(uint16_t)((pm&1)|((pm>>1)<<(4*pb+1)));
            const uint8_t*t=chr+ti*16; uint8_t f2=(pb&1)?0xFF:0, f3=(pb&2)?0xFF:0;
            uint8_t*d0=row0+x, *d1=d0+PS, *d2=d1+PS, *d3=d2+PS;
            const uint8_t*ot=chr+(ov&255)*16; unsigned opb=ov>>8;         /* only the bytes that differ from what the bitmap holds are written (chip RAM writes are slow) */
            int w01=fresh, w2=fresh||((opb^pb)&1), w3=fresh||((opb^pb)&2);
            int two=(nx*32+x<32);
            for(int fy=0;fy<8;fy++){ unsigned o=fy*AR_BROW;
                if(w01 || ot[fy]!=t[fy]){ d0[o]=t[fy]; if(two) d0[o+64]=t[fy]; }
                if(w01 || ot[fy+8]!=t[fy+8]){ d1[o]=t[fy+8]; if(two) d1[o+64]=t[fy+8]; }
                if(w2){ d2[o]=f2; if(two) d2[o+64]=f2; }
                if(w3){ d3[o]=f3; if(two) d3[o+64]=f3; } }
        }
    }
    ar_st_tiles+=nch; if(nch) g->gen++;
    if(fresh || nch>4){                          /* exact mask of the row; a few changed tiles only add their entries (a superset is harmless: the copper writes a colour too many) */
        um=0; for(int x=0;x<32;x++){ unsigned pb=sh[x]>>8, pm=g->pm[sh[x]&255]; um|=(uint16_t)((pm&1)|((pm>>1)<<(4*pb+1))); }
    } else um|=g->um[nt][cy];
    g->bv[nt][cy]=sum; g->bvok[nt][cy]=1; g->um[nt][cy]=um;
}

/* ---- scene log --------------------------------------------------------------------------------------------------------------------------- */
static uint16_t incy1(uint16_t a){
    if((a&0x7000)!=0x7000) return (uint16_t)(a+0x1000);
    a&=~0x7000; int y=(a&0x3E0)>>5; if(y==29){ y=0; a^=0x800; } else if(y==31) y=0; else y++; return (uint16_t)((a&~0x3E0)|(y<<5));
}
static uint16_t next_vl(const ArRegion*r){            /* vl of the line after the region */
    uint16_t v=r->vl; for(int y=r->y0;y<=r->y1;y++) v=(uint16_t)((incy1(v)&~0x041F)|r->tx); return v;
}
#define disp ar_out.reg
static int resnap; static int ndisp;
static void render_region(Nes*n,const ArRegion*r);
static void hw_region(Nes*n,const ArRegion*r);
static uint8_t xdone[240]; static uint8_t sl_cnt[240], sl_idx[240][8]; static int sl_valid, sl_h;
long ar_st_nt_flush, ar_st_nt_conv;
void ar_begin_frame(Nes*n){
    ar_scene.nreg=0; ar_scene.overflow=0; resnap=0; ndisp=0; ar_set(ar_scene.exact,0,sizeof ar_scene.exact); ar_cpy(ar_scene.oam,n->ppu.oam,256);
    if(ar_hw_enable){
        hw_select(); ar_set(xdone,0,sizeof xdone); sl_valid=0;
        if(!hw_zeroed){ hw_zeroed=1; for(int st=0;st<2;st++){ ar_hw.sel=st; hw_select(); for(int k=0;k<4;k++) for(int y=0;y<240;y++){ uint32_t*d=(uint32_t*)AR_SPR(k,y); for(int i=0;i<8;i++) d[i]=0; spr_ext[st][y]=0x00FF; } } ar_set(spr_sh,0,sizeof spr_sh); ar_hw.sel=0; hw_select(); }
    }
}
/* render the regions logged so far with the state as it is now (called before a name table / OAM change in the middle of the frame) */
void ar_flush(Nes*n,int oam_changes){
    AR_T0();
    for(int i=0;i<ar_scene.nreg;i++){
        if(ar_hw_enable) hw_region(n,&ar_scene.reg[i]);
        else { render_region(n,&ar_scene.reg[i]); if(ndisp<AR_MAX_REGIONS) disp[ndisp++]=ar_scene.reg[i]; }
    }
    ar_scene.nreg=0; if(oam_changes){ resnap=1; sl_valid=0; }
    AR_T1();
}
static int same_params(const ArRegion*a,const ArRegion*b){
    return a->tx==b->tx && a->fx==b->fx && a->rend==b->rend && a->exact==b->exact && a->blank_idx==b->blank_idx && a->bg_en==b->bg_en && a->sp_en==b->sp_en && a->min_bg==b->min_bg &&
           a->min_sp==b->min_sp && a->bgpat==b->bgpat && a->sppat==b->sppat && a->large==b->large && a->mirror==b->mirror && a->chr_lo==b->chr_lo &&
           a->chr_hi==b->chr_hi && a->gray==b->gray && a->emph==b->emph && !ar_cmp(a->pal,b->pal,32);
}
static uint16_t last_next;              /* vl of the line after the last logged region (kept incrementally: next_vl() over a growing region would be quadratic) */
void ar_log_region(Nes*n,const ArRegion*r){
    ArScene*s=&ar_scene;
    if(resnap){ ar_cpy(s->oam,n->ppu.oam,256); resnap=0; }
    if(s->nreg){
        ArRegion*l=&s->reg[s->nreg-1];
        if(l->y1+1==r->y0 && same_params(l,r) && (r->exact || !r->rend || last_next==r->vl) ){ l->y1=r->y1; if(!r->exact && r->rend) last_next=next_vl(r); return; }
    }
    if(s->nreg>=AR_MAX_REGIONS) ar_flush(n,0);
    s->reg[s->nreg++]=*r; last_next=(!r->exact && r->rend)? next_vl(r) : 0;
}
void ar_log_exact(Nes*n,int y){ (void)n; if(y>=0&&y<240) ar_scene.exact[y]=1; }

/* ---- output ------------------------------------------------------------------------------------------------------------------------------ */
static const uint8_t rev8[16]={0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
static uint8_t bitrev(uint8_t b){ return (uint8_t)((rev8[b&15]<<4)|rev8[b>>4]); }
/* OR/insert an 8 pixel wide pattern (byte b, mask m) at pixel x of a 256 pixel plane row; returns via window */
static void put_bits(uint8_t*row,int x,uint8_t mask,uint8_t val,int set){
    int bx=x>>3, sh=x&7; uint16_t m=(uint16_t)(mask<<8)>>sh, v=(uint16_t)(val<<8)>>sh;
    if(bx<32){ if(set){ row[bx]=(uint8_t)((row[bx]&~(m>>8))|((v>>8)&(m>>8))); } }
    if(bx+1<32 && sh){ if(set){ row[bx+1]=(uint8_t)((row[bx+1]&~(m&255))|(v&(m&255))); } }
}
static void render_region(Nes*n,const ArRegion*r){
    Ppu*p=&n->ppu; (void)p; if(r->exact) return;
    uint16_t vl=r->vl; int h=r->large?16:8;
    for(int y=r->y0;y<=r->y1;y++){
        if(!r->rend){
            for(int k=0;k<5;k++) ar_set(&ar_planes[k][y*AR_STRIDE],((r->blank_idx>>k)&1)?0xFF:0,AR_STRIDE);
        } else if(!ar_scene.exact[y]){
            int cy=(vl>>5)&31, x0=vl&31, nt=(vl>>10)&3, fy=(vl>>12)&7, ny=nt>>1, nx=nt&1;
            int bank= r->bgpat ? r->chr_hi : r->chr_lo; int c=get_cfg(bank,r->mirror);
            ensure_trow(n,c,nt,cy); ensure_trow(n,c,nt^1,cy);
            int row=ny*256+cy*8+fy; int s=nx*256+x0*8+r->fx; int b0=(s>>3)&63, sh=s&7;
            uint8_t line[5][AR_STRIDE];
            for(int k=0;k<4;k++){
                const uint8_t*src=BMP(c,k,row);
                for(int i=0;i<AR_STRIDE;i++){ int b=(b0+i)&63; line[k][i]= sh? (uint8_t)((src[b]<<sh)|(src[(b+1)&63]>>(8-sh))) : src[b]; }
            }
            ar_set(line[4],0,AR_STRIDE);
            /* left clip / disabled background: pixels x < min_bg are backdrop (index 0) */
            { int cl= r->bg_en ? (r->min_bg>256?256:r->min_bg) : 256;
              for(int i=0;i<(cl>>3);i++) for(int k=0;k<4;k++) line[k][i]=0; }
            uint8_t bo[AR_STRIDE]; for(int i=0;i<AR_STRIDE;i++){ bo[i]=(uint8_t)(line[0][i]|line[1][i]); line[2][i]&=bo[i]; line[3][i]&=bo[i]; }   /* value 0: backdrop, palette bits 0 */
            /* sprites */
            if(y>=1 && r->sp_en && r->min_sp<256){
                int e=y-1, cnt=0, idx[8];
                for(int i=0;i<64 && cnt<8;i++){ int sy=ar_scene.oam[i*4]; if(e>=sy && e<sy+h) idx[cnt++]=i; }
                uint8_t claim[AR_STRIDE]; ar_set(claim,0,sizeof claim);
                for(int q=0;q<cnt;q++){
                    int i=idx[q]; int sy=ar_scene.oam[i*4], ti=ar_scene.oam[i*4+1], at=ar_scene.oam[i*4+2], sx=ar_scene.oam[i*4+3];
                    if(sy>=240) continue;
                    int vm=(at&0x80)!=0, hm=(at&0x40)!=0, prio=(at&0x20)!=0, pal=at&3;
                    int lo_= vm ? ((r->large?15:7)-(e-sy)) : (e-sy); uint16_t ta;
                    if(r->large) ta=(uint16_t)((((ti&1)?0x1000:0)|((ti&~1)<<4))+(lo_>=8?lo_+8:lo_)); else ta=(uint16_t)(((ti<<4)|(r->sppat?0x1000:0))+lo_);
                    const uint8_t*cb=n->map.chr+(uint32_t)(ta<0x1000? r->chr_lo : r->chr_hi)*4096u+(ta&0xFFF);
                    uint8_t sl=cb[0], sh1=cb[8]; if(hm){ sl=bitrev(sl); sh1=bitrev(sh1); }
                    uint8_t opq=(uint8_t)(sl|sh1); if(!opq) continue;
                    /* visible columns: x >= min_sp, x <= 255 */
                    uint8_t colmask=0xFF; if(sx<r->min_sp){ int cut=r->min_sp-sx; colmask=(uint8_t)(cut>=8?0:(0xFF>>cut)); }
                    opq&=colmask; if(!opq) continue;
                    /* window over the line: claim, availability */
                    uint8_t avail_w[2], cl_w[2], bo_w[2];
                    int bx=sx>>3, ssh=sx&7;
                    uint16_t mw=(uint16_t)(opq<<8)>>ssh;
                    for(int w=0;w<2;w++){ int bb=bx+w; avail_w[w]=0; cl_w[w]=(bb<32)?claim[bb]:0xFF; bo_w[w]=(bb<32)?bo[bb]:0; }
                    uint8_t m8[2]={(uint8_t)(mw>>8),(uint8_t)mw};
                    for(int w=0;w<2;w++){ avail_w[w]=(uint8_t)(m8[w]&~cl_w[w]); }
                    /* claim (even where the sprite loses to the background) */
                    for(int w=0;w<2;w++){ int bb=bx+w; if(bb<32) claim[bb]|=m8[w]; }
                    uint8_t draw_w[2]; for(int w=0;w<2;w++) draw_w[w]=(uint8_t)(avail_w[w] & (prio? (uint8_t)~bo_w[w] : 0xFF));
                    uint16_t s0w=(uint16_t)(sl<<8)>>ssh, s1w=(uint16_t)(sh1<<8)>>ssh;
                    uint8_t sv0[2]={(uint8_t)(s0w>>8),(uint8_t)s0w}, sv1[2]={(uint8_t)(s1w>>8),(uint8_t)s1w};
                    for(int w=0;w<2;w++){ int bb=bx+w; if(bb>=32) continue; uint8_t D=draw_w[w]; if(!D) continue;
                        line[0][bb]=(uint8_t)((line[0][bb]&~D)|(sv0[w]&D));
                        line[1][bb]=(uint8_t)((line[1][bb]&~D)|(sv1[w]&D));
                        line[2][bb]=(uint8_t)((line[2][bb]&~D)|((pal&1)?D:0));
                        line[3][bb]=(uint8_t)((line[3][bb]&~D)|((pal&2)?D:0));
                        line[4][bb]|=D; }
                }
            }
            for(int k=0;k<5;k++) ar_cpy(&ar_planes[k][y*AR_STRIDE],line[k],AR_STRIDE);
        }
        vl=(uint16_t)((incy1(vl)&~0x041F)|r->tx);
    }
}

/* ---- stage 2b: display format for the dual playfield (see arender.h) -------------------------------------------------------------------- */
static void hw_select(void){
    unsigned set=(unsigned)ar_hw.sel*(3u*AR_LAYB);
    ar_hw.spr=hw_xsets+set+4; ar_hw.xpf1=hw_xsets+set+AR_LAYB+4; ar_hw.xpf2=hw_xsets+set+2*AR_LAYB+4;
}
void ar_hw_shown(void){ ar_hw.sel^=1; hw_select(); }
long ar_st_kind[3];
static void hw_add(const ArRegion*r){ ar_st_kind[r->kind]++; if(ndisp<AR_MAX_REGIONS) disp[ndisp++]=*r; }
/* chunky exact row -> the two playfields: tables for pixel pairs (slot 2j, 2j+1 of a group of 8 pixels): plane k of the playfield value in byte k of the long word */
static uint32_t c2p1[4][1024], c2p2[4][1024];
static void c2p_init(void){
    for(int j=0;j<4;j++) for(int ia=0;ia<32;ia++) for(int ib=0;ib<32;ib++){
        uint32_t t1=0,t2=0;
        for(int q=0;q<2;q++){ int v=q?ib:ia, sl=2*j+q;
            for(int k=0;k<4;k++){ uint32_t bit=(uint32_t)(0x80>>sl)<<(24-8*k); if(v<16){ if((v>>k)&1) t1|=bit; } else if(((v&15)>>k)&1) t2|=bit; } }
        c2p1[j][(ia<<5)|ib]=t1; c2p2[j][(ia<<5)|ib]=t2;
    }
}
static void hw_exact_row(const uint8_t*fb,int y){
    uint8_t*a0=AR_XPF1(0,y),*a1=AR_XPF1(1,y),*a2=AR_XPF1(2,y),*a3=AR_XPF1(3,y),*b0=AR_XPF2(0,y),*b1=AR_XPF2(1,y),*b2=AR_XPF2(2,y),*b3=AR_XPF2(3,y);
    for(int g=0;g<32;g++,fb+=8){
        uint32_t x1=0,x2=0;
        for(int j=0;j<4;j++){ unsigned idx=((unsigned)(fb[2*j]&31)<<5)|(fb[2*j+1]&31); x1|=c2p1[j][idx]; x2|=c2p2[j][idx]; }
        a0[g]=(uint8_t)(x1>>24); a1[g]=(uint8_t)(x1>>16); a2[g]=(uint8_t)(x1>>8); a3[g]=(uint8_t)x1;
        b0[g]=(uint8_t)(x2>>24); b1[g]=(uint8_t)(x2>>16); b2[g]=(uint8_t)(x2>>8); b3[g]=(uint8_t)x2;
    }
    xdone[y]=1;
}
/* ar_planes row y (5 planes = palette index) -> the two playfields: background value (index < 16) and sprite value (index - 16) */
static void hw_encode_row(int y){
    const uint32_t*p4=(const uint32_t*)&ar_planes[4][y*AR_STRIDE];
    for(int k=0;k<4;k++){
        const uint32_t*s=(const uint32_t*)&ar_planes[k][y*AR_STRIDE]; uint32_t*a=(uint32_t*)AR_XPF1(k,y),*b=(uint32_t*)AR_XPF2(k,y);
        for(int i=0;i<AR_STRIDE/4;i++){ uint32_t m=p4[i], v=s[i]; a[i]=v&~m; b[i]=v&m; }
    }
    xdone[y]=1;
}
static __attribute__((noinline)) void hw_build_lists(int h){
    ar_set(sl_cnt,0,sizeof sl_cnt);
    for(int i=0;i<64;i++){ int sy=ar_scene.oam[i*4]; if(sy>=239) continue;
        int e1=sy+h; if(e1>239) e1=239;
        for(int e=sy;e<e1;e++) if(sl_cnt[e]<8) sl_idx[e][sl_cnt[e]++]=(uint8_t)i; }
    sl_valid=1; sl_h=h;
}
/* the sprite layer line y of the background region s (cover of the clipped background, then the sprites) */
static uint32_t hw_used, tmpsg[12];
#if __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
#define LD16(p) (*(const uint16_t*)(p))
#define ST16(p,v) (*(uint16_t*)(p)=(uint16_t)(v))
#define LD32(p) (*(const uint32_t*)(p))
#else
#define LD16(p) ((uint16_t)(((const uint8_t*)(p))[0]<<8|((const uint8_t*)(p))[1]))
#define ST16(p,v) do{ ((uint8_t*)(p))[0]=(uint8_t)((v)>>8); ((uint8_t*)(p))[1]=(uint8_t)(v); }while(0)
#define LD32(p) ((uint32_t)(((const uint8_t*)(p))[0]<<24|((const uint8_t*)(p))[1]<<16|((const uint8_t*)(p))[2]<<8|((const uint8_t*)(p))[3]))
#endif
static __attribute__((noinline)) void hw_line(Nes*n,const ArRegion*s,int y){
    int cl=s->bg_en?(s->min_bg>256?256:s->min_bg):256, clb=cl>>3, lo=32, hi=-1;
    uint8_t*L0=(uint8_t*)tmpl[0],*L1=(uint8_t*)tmpl[1],*L2=(uint8_t*)tmpl[2],*L3=(uint8_t*)tmpl[3];
    uint32_t used0=hw_used; hw_used=0; int cacheable=1; uint32_t*sg=spr_sg[ar_hw.sel][y];
    {   /* the line is a function of the sprites in range, the pattern banks, the clip: the same inputs as when the line was drawn into this set -> nothing to do (not for sprites behind the background) */
        uint32_t k[12]; int cnt=0, e=y-1, prio=0; for(int i=0;i<12;i++) k[i]=0;
        if(y>=1 && s->sp_en && s->min_sp<256){ int h=s->large?16:8; if(!sl_valid || sl_h!=h) hw_build_lists(h); cnt=sl_cnt[e];
            k[0]=(uint32_t)cnt|((uint32_t)s->large<<4)|((uint32_t)s->sppat<<5)|((uint32_t)s->min_sp<<16); k[1]=(uint32_t)s->chr_lo|((uint32_t)s->chr_hi<<8);
            for(int q=0;q<cnt;q++){ uint32_t w=LD32(&ar_scene.oam[sl_idx[e][q]*4]); k[2+q]=w; if(w&0x2000) prio=1; } }
        if(prio){ k[10]=(uint32_t)(s->row0+(y-s->y0))|((uint32_t)s->bytex<<10)|((uint32_t)s->fx<<17)|((uint32_t)s->cfg<<20); k[11]=cfgs[s->cfg].gen; }   /* behind the background: depends on the background window too */
        k[1]|=(uint32_t)clb<<16;
        if(cacheable && sg[0]==k[0] && sg[1]==k[1] && !((sg[2]^k[2])|(sg[3]^k[3])|(sg[4]^k[4])|(sg[5]^k[5])|(sg[6]^k[6])|(sg[7]^k[7])|(sg[8]^k[8])|(sg[9]^k[9])|(sg[10]^k[10])|(sg[11]^k[11]))){ hw_used=used0|spr_su[ar_hw.sel][y]; return; }
        for(int i=0;i<12;i++) tmpsg[i]=k[i];
    }
    if(clb){ for(int i=0;i<clb;i++) L2[i]=0xFF; lo=0; hi=clb-1; hw_used|=1; }       /* value 4 = backdrop colour, covers the background pixels */
    if(y>=1 && s->sp_en && s->min_sp<256){
        int e=y-1, cnt=sl_cnt[e];
        if(cnt){
            uint32_t claiml[9]; for(int i=0;i<9;i++) claiml[i]=0; uint8_t*claim=(uint8_t*)claiml;
            int row=s->row0+(y-s->y0); const uint8_t*chr_lo=n->map.chr+(uint32_t)s->chr_lo*4096u,*chr_hi=n->map.chr+(uint32_t)s->chr_hi*4096u;
            for(int q=0;q<cnt;q++){
                int i=sl_idx[e][q]; const uint8_t*o=&ar_scene.oam[i*4]; int sy=o[0], ti=o[1], at=o[2], sx=o[3];
                int lo_= (at&0x80) ? ((s->large?15:7)-(e-sy)) : (e-sy); uint16_t ta;
                if(s->large) ta=(uint16_t)((((ti&1)?0x1000:0)|((ti&~1)<<4))+(lo_>=8?lo_+8:lo_)); else ta=(uint16_t)(((ti<<4)|(s->sppat?0x1000:0))+lo_);
                const uint8_t*cb=(ta<0x1000? chr_lo : chr_hi)+(ta&0xFFF);
                uint8_t sl=cb[0], sh1=cb[8]; if(at&0x40){ sl=fliptab[sl]; sh1=fliptab[sh1]; }
                uint8_t opq=(uint8_t)(sl|sh1); if(!opq) continue;
                if(sx<s->min_sp){ int cut=s->min_sp-sx; opq&=(uint8_t)(cut>=8?0:(0xFF>>cut)); if(!opq) continue; }
                int bx=sx>>3, ssh=sx&7, pal=at&3; uint32_t m=(uint32_t)opq<<8>>ssh;
                uint32_t D=m&~(uint32_t)LD16(claim+bx); ST16(claim+bx,LD16(claim+bx)|m);
                if(D && (at&0x20)){                             /* behind the background: only where the background is transparent (patterns 0) */
                    const uint8_t*pa=BMP(s->cfg,0,row)+s->bytex+bx,*pb=BMP(s->cfg,1,row)+s->bytex+bx;
                    uint32_t bo=((LD32(pa)|LD32(pb))<<s->fx)>>16;
                    if(bx<clb) bo&=(bx+1<clb)?0:0x00FF;           /* the clipped part of the background is transparent */
                    D&=~bo;
                }
                if(!D) continue;
                { uint32_t nD=~D, s0=(uint32_t)sl<<8>>ssh, s1=(uint32_t)sh1<<8>>ssh;
                  ST16(L0+bx,(LD16(L0+bx)&nD)|(s0&D)); ST16(L1+bx,(LD16(L1+bx)&nD)|(s1&D));
                  ST16(L2+bx,(LD16(L2+bx)&nD)|((pal&1)?D:0)); ST16(L3+bx,(LD16(L3+bx)&nD)|((pal&2)?D:0)); }
                if(bx<lo) lo=bx; if(bx+1>hi) hi=bx+1;
                hw_used|=0xEu<<(16+4*pal);
            }
            if(hi>31) hi=31;
            for(int k=0;k<4;k++) tmpl[k][8]=0;
        }
    }
    {   /* write the changed long words of the line (union of the old and the new extent) to the layer in chip RAM */
        unsigned old=spr_ext[ar_hw.sel][y]; int olo=old&255, ohi=old>>8;
        for(int i=0;i<12;i++) sg[i]=tmpsg[i];
        spr_su[ar_hw.sel][y]=hw_used; hw_used|=used0;
        if(hi<0 && ohi<olo) return;
        int a=lo<olo?lo:olo, b=hi>ohi?hi:ohi;
        for(int k=0;k<4;k++){
            uint32_t*c=(uint32_t*)AR_SPR(k,y),*sh=(uint32_t*)spr_sh[ar_hw.sel][k][y],*t=tmpl[k];
            for(int l=a>>2;l<=(b>>2);l++){ uint32_t v=t[l]; if(v!=sh[l]){ sh[l]=v;
#ifndef AR_NOCHIP
 c[l]=v;
#endif
 } t[l]=0; }
        }
        spr_ext[ar_hw.sel][y]= hi<0 ? 0x00FF : (uint16_t)(lo|(hi<<8));
    }
}
/* a scene region -> display regions */
static __attribute__((noinline)) void hw_region(Nes*n,const ArRegion*r){
    if(r->exact){ ArRegion e=*r; e.kind=AR_EXACT; e.used=~0u; hw_add(&e); return; }
    if(!r->rend){ ArRegion e=*r; e.kind=AR_BLANK; e.used=~0u; hw_add(&e); return; }
    uint16_t vl=r->vl; int y=r->y0;
    int bank=r->bgpat? r->chr_hi : r->chr_lo; int c=get_cfg(bank,r->mirror);
    while(y<=r->y1){
        int nt=(vl>>10)&3, cy=(vl>>5)&31, fy=(vl>>12)&7, row=(nt>>1)*256+cy*8+fy;
        ArRegion s=*r; s.kind=AR_BG; s.cfg=(uint8_t)c; s.y0=(int16_t)y; s.row0=(uint16_t)row; s.bytex=(uint8_t)(((vl>>10)&1)*32+(vl&31)); s.vl=vl;
        int ye=y, rr=cy*8+fy, last=(cy<30)?239:255, len=last-rr+1, room=r->y1-y+1;      /* lines of this segment: the rows continue up to the last row of the name table */
        if((vl&0x41F)!=(r->tx&0x41F)) len=1;                          /* the first line has other x bits than the following ones */
        if(len>room) len=room;
        ye=y+len-1;
        uint16_t vnext;                                               /* vl of the line after the segment */
        { int tot=rr+len; unsigned nb=nt&2;
          if(tot>last){ tot=0; if(last==239) nb^=2; }
          vnext=(uint16_t)((((unsigned)tot&7)<<12)|(((unsigned)tot>>3)<<5)|(nb<<10)|r->tx); }
#ifdef AR_VERIFY
        { uint16_t v2=vl; for(int i=1;i<len;i++) v2=(uint16_t)((incy1(v2)&~0x041F)|r->tx); uint16_t w=(uint16_t)((incy1(v2)&~0x041F)|r->tx); if(w!=vnext) { extern int printf(const char*,...); printf("SPLIT MISMATCH vl %04x len %d: %04x vs %04x\n",vl,len,w,vnext); } }
#endif
        s.y1=(int16_t)ye; hw_used=0;
        for(int t=row>>3; t<=(row+(ye-y))>>3; t++){ int ny=row>>8; ensure_trow(n,c,ny*2,t&31); ensure_trow(n,c,ny*2+1,t&31); }
        for(int t=row>>3; t<=(row+(ye-y))>>3; t++){ int ny=row>>8; hw_used|=cfgs[c].um[ny*2][t&31]|cfgs[c].um[ny*2+1][t&31]; }
        { int spr_on=s.sp_en && s.min_sp<256, clbs=(s.bg_en?(s.min_bg>256?256:s.min_bg):256)>>3;
          if(spr_on){ int h=s.large?16:8; if(!sl_valid || sl_h!=h) hw_build_lists(h); }
          for(int yy=y;yy<=ye;yy++){
              if(!clbs && spr_ext[ar_hw.sel][yy]==0x00FF && (!spr_on || yy<1 || !sl_cnt[yy-1])) continue;      /* nothing to draw, nothing to clear */
              hw_line(n,&s,yy); } }
        s.used=hw_used;
        hw_add(&s);
        vl=vnext; y=ye+1;
    }
}
/* a name table byte is about to change in the middle of a displayed frame: the background regions shown so far point into bitmaps that are going to change,
 * so they are drawn into the exact rows now (the bitmaps still hold the picture they were logged with) */
void ar_flush_nt(Nes*n){
    ar_flush(n,0); ar_st_nt_flush++;
    if(!ar_hw_enable) return;
    for(int i=0;i<ndisp;i++) if(disp[i].kind==AR_BG){
        const ArRegion*r=&disp[i]; ar_st_nt_conv++;
        for(int y=r->y0;y<=r->y1;y++){
            int row=r->row0+(y-r->y0), sh=r->fx;
            for(int k=0;k<4;k++){
                const uint8_t*src=BMP(r->cfg,k,row)+r->bytex; uint8_t*d=AR_XPF1(k,y);
                for(int j=0;j<AR_STRIDE;j++) d[j]=(uint8_t)((src[j]<<sh)|(src[j+1]>>(8-sh)));
                ar_cpy(AR_XPF2(k,y),spr_sh[ar_hw.sel][k][y],AR_STRIDE);
            }
            xdone[y]=1;
        }
        disp[i].kind=AR_EXACT; disp[i].used=~0u;
    }
}
void ar_hw_palette(const ArRegion*r,uint8_t out[32]){
    if(r->kind==AR_BLANK){ for(int i=0;i<32;i++) out[i]=r->pal[r->blank_idx&31]; return; }
    for(int i=0;i<16;i++) out[i]=(i&3)?r->pal[i]:r->pal[0];
    for(int i=16;i<32;i++) out[i]=(i&3)?r->pal[i]:r->pal[0];
}
int ar_used_viol;
void ar_expand_hw(const ArFrame*f,uint8_t*fb){
    for(int i=0;i<f->nreg;i++){
        const ArRegion*r=&f->reg[i];
        for(int y=r->y0;y<=r->y1;y++) for(int x=0;x<256;x++){
            unsigned v1=0,v2=0,idx;
            if(r->kind==AR_BLANK){ fb[y*256+x]=r->blank_idx; continue; }
            if(r->kind==AR_BG){
                int row=r->row0+(y-r->y0), sp=r->bytex*8+r->fx+x;
                for(int k=0;k<4;k++){ v1|=(unsigned)((BMP(r->cfg,k,row)[sp>>3]>>(7-(sp&7)))&1)<<k; v2|=(unsigned)((AR_SPR(k,y)[x>>3]>>(7-(x&7)))&1)<<k; }
            } else {
                for(int k=0;k<4;k++){ v1|=(unsigned)((AR_XPF1(k,y)[x>>3]>>(7-(x&7)))&1)<<k; v2|=(unsigned)((AR_XPF2(k,y)[x>>3]>>(7-(x&7)))&1)<<k; }
            }
            if(v2) idx=(v2&3)?16+v2:0; else idx=(v1&3)?v1:0;
            fb[y*256+x]=(uint8_t)idx;
            if(r->kind==AR_BG && !((r->used>>idx)&1)) ar_used_viol=(y<<8|x)+1;
        }
    }
}
void ar_end_frame(Nes*n,ArFrame*out){
    ArScene*s=&ar_scene; Ppu*p=&n->ppu; unsigned long long t_end=0;
#ifdef AR_TIMING
    t_end=ar_now();
#endif
    for(int k=0;k<5;k++) out->plane[k]=ar_planes[k];
    ar_flush(n,0); out->nreg=ndisp; if(out!=&ar_out) for(int i=0;i<ndisp;i++) out->reg[i]=disp[i];
#ifdef AR_TIMING
    unsigned long long t_ex=ar_now();
#endif
    /* exact rows: from the chunky frame buffer (palette index per pixel) */
    if(ar_hw_enable){ for(int y=0;y<240;y++) if(s->exact[y] && !xdone[y]) hw_exact_row(p->fb+y*256,y); }
    else for(int y=0;y<240;y++) if(s->exact[y]){
        for(int k=0;k<5;k++) ar_set(&ar_planes[k][y*AR_STRIDE],0,AR_STRIDE);
        for(int x=0;x<256;x++){ unsigned v=p->fb[y*256+x]&31; for(int k=0;k<5;k++) if((v>>k)&1) ar_planes[k][y*AR_STRIDE+(x>>3)]|=(uint8_t)(0x80>>(x&7)); }
    }
#ifdef AR_TIMING
    { unsigned long long t=ar_now(); ar_ns_total+=t-t_end; ar_ns_exact+=t-t_ex; }
#endif
}
void ar_expand(const ArFrame*f,uint8_t*fb){
    for(int y=0;y<240;y++) for(int x=0;x<256;x++){
        unsigned v=0; for(int k=0;k<5;k++) v|=(unsigned)((f->plane[k][y*AR_STRIDE+(x>>3)]>>(7-(x&7)))&1)<<k;
        fb[y*256+x]=(uint8_t)v;
    }
}
