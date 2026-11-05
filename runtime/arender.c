/* arender.c - planar rendering of the NES picture from the scene log (see arender.h, docs/RENDER.md).  Target independent. */
#pragma GCC optimize ("no-strict-aliasing")
#include "arender.h"
/* no libc calls (freestanding Amiga build): byte loops */
static void ar_set(void*d,int v,unsigned n){ uint8_t*p=d; while(n--) *p++=(uint8_t)v; }
static void ar_cpy(void*d,const void*s,unsigned n){ uint8_t*p=d; const uint8_t*q=s; while(n--) *p++=*q++; }
static int ar_cmp(const void*a,const void*b,unsigned n){ const uint8_t*p=a,*q=b; while(n--){ if(*p!=*q) return *p-*q; p++; q++; } return 0; }

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
ArScene ar_scene; ArFrame ar_out;
static uint8_t ar_planes[5][240*AR_STRIDE] __attribute__((aligned(4)));

/* ---- persistent background bitmaps ------------------------------------------------------------------------------------------------------ */
typedef struct {
    int valid; uint8_t bank, mirror; uint32_t stamp;
    uint32_t bv[4][32]; uint8_t bvok[4][32];          /* write-counter sum of the source rows the bitmap tile row was built from */
} ArCfg;
static ArCfg cfgs[AR_MAX_CFG];
ArHw ar_hw; int ar_hw_enable;
#ifndef AR_NO_STATIC
static uint8_t hw_static[AR_HW_BYTES] __attribute__((aligned(4)));
#endif
static uint8_t*hw_base; static uint8_t*hw_xsets;      /* hw_base: bitmaps; hw_xsets: 2 sets of (spr, xpf1, xpf2) */
static uint8_t spr_used[2][240];
void ar_hw_setmem(void*chip){ hw_base=chip; }
static void hw_mem(void){ if(!hw_base){
#ifndef AR_NO_STATIC
    hw_base=hw_static;
#endif
  } ar_hw.bmp=hw_base; hw_xsets=hw_base+AR_MAX_CFG*4*AR_BROWS*AR_BROW; }
#define BMP(c,k,row) AR_BMP(c,k,row)
static uint32_t stamp_ctr; static uint8_t bitrev(uint8_t b); static void hw_select(void); static uint8_t fliptab[256];

static uint16_t nt_phys(int mirror,int nt){     /* physical offset of logical nametable nt (0..3) */
    uint16_t o=0; uint16_t a=(uint16_t)(nt*0x400);
    switch(mirror){ case 0: o=0; break; case 1: o=0x400; break; case 2: o=(uint16_t)(a&0x400); break; default: o=(uint16_t)((a&0x800)>>1); }
    return o;
}
void ar_init(void){ for(int i=0;i<256;i++) fliptab[i]=bitrev((uint8_t)i); hw_mem(); hw_select(); ar_set(cfgs,0,sizeof cfgs); stamp_ctr=0; }
static int get_cfg(int bank,int mirror){
    int best=0; uint32_t bs=0xFFFFFFFFu;
    for(int i=0;i<AR_MAX_CFG;i++){
        if(cfgs[i].valid && cfgs[i].bank==bank && cfgs[i].mirror==mirror){ cfgs[i].stamp=++stamp_ctr; return i; }
        uint32_t s=cfgs[i].valid? cfgs[i].stamp : 0; if(s<bs){ bs=s; best=i; }
    }
    ArCfg*c=&cfgs[best]; ar_set(c->bvok,0,sizeof c->bvok); c->valid=1; c->bank=(uint8_t)bank; c->mirror=(uint8_t)mirror; c->stamp=++stamp_ctr;
    return best;
}
/* make tile row cy of logical nametable nt current in bitmap c */
static void ensure_trow(Nes*n,int c,int nt,int cy){
    ArCfg*g=&cfgs[c]; Ppu*p=&n->ppu; uint16_t base=nt_phys(g->mirror,nt);
    uint32_t sum=p->ntv[(base+cy*32)>>5]+p->ntv[(base+0x3C0+(cy>>2)*8)>>5];
    if(g->bvok[nt][cy] && g->bv[nt][cy]==sum) return;
    const uint8_t*chr=n->map.chr+(uint32_t)g->bank*4096u; int ny=nt>>1, nx=nt&1;
    for(int x=0;x<32;x++){
        int ti=p->nt[base+cy*32+x]; int at=p->nt[base+0x3C0+(cy>>2)*8+(x>>2)]; int pb=(at>>(((cy&2)<<1)|(x&2)))&3;
        for(int fy=0;fy<8;fy++){
            int row=ny*256+cy*8+fy, col=nx*32+x;
            uint8_t v0=chr[ti*16+fy], v1=chr[ti*16+fy+8], v2=(pb&1)?0xFF:0, v3=(pb&2)?0xFF:0;
            BMP(c,0,row)[col]=v0; BMP(c,1,row)[col]=v1; BMP(c,2,row)[col]=v2; BMP(c,3,row)[col]=v3;
            if(col<40){ BMP(c,0,row)[col+64]=v0; BMP(c,1,row)[col+64]=v1; BMP(c,2,row)[col+64]=v2; BMP(c,3,row)[col+64]=v3; }
        }
    }
    g->bv[nt][cy]=sum; g->bvok[nt][cy]=1;
}

/* ---- scene log --------------------------------------------------------------------------------------------------------------------------- */
static uint16_t incy1(uint16_t a){
    if((a&0x7000)!=0x7000) return (uint16_t)(a+0x1000);
    a&=~0x7000; int y=(a&0x3E0)>>5; if(y==29){ y=0; a^=0x800; } else if(y==31) y=0; else y++; return (uint16_t)((a&~0x3E0)|(y<<5));
}
static uint16_t next_vl(const ArRegion*r){            /* vl of the line after the region */
    uint16_t v=r->vl; for(int y=r->y0;y<=r->y1;y++) v=(uint16_t)((incy1(v)&~0x041F)|r->tx); return v;
}
static int resnap; static ArRegion disp[AR_MAX_REGIONS]; static int ndisp;
static void render_region(Nes*n,const ArRegion*r);
static void hw_region(Nes*n,const ArRegion*r);
static uint8_t xdone[240]; static uint8_t sl_cnt[240], sl_idx[240][8]; static int sl_valid, sl_h;
long ar_st_nt_flush, ar_st_nt_conv;
void ar_begin_frame(Nes*n){
    ar_scene.nreg=0; ar_scene.overflow=0; resnap=0; ndisp=0; ar_set(ar_scene.exact,0,sizeof ar_scene.exact); ar_cpy(ar_scene.oam,n->ppu.oam,256);
    if(ar_hw_enable){
        hw_select(); ar_set(xdone,0,sizeof xdone); sl_valid=0;
        for(int y=0;y<240;y++) if(spr_used[ar_hw.sel][y]){                  /* incremental clearing of the sprite layer rows written two frames ago */
            for(int k=0;k<4;k++){ uint32_t*d=(uint32_t*)AR_SPR(k,y); for(int i=0;i<8;i++) d[i]=0; }
            spr_used[ar_hw.sel][y]=0; }
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
void ar_log_region(Nes*n,const ArRegion*r){
    ArScene*s=&ar_scene;
    if(resnap){ ar_cpy(s->oam,n->ppu.oam,256); resnap=0; }
    if(s->nreg){
        ArRegion*l=&s->reg[s->nreg-1];
        if(l->y1+1==r->y0 && same_params(l,r) && (r->exact || !r->rend || next_vl(l)==r->vl) ){ l->y1=r->y1; return; }
    }
    if(s->nreg>=AR_MAX_REGIONS) ar_flush(n,0);
    s->reg[s->nreg++]=*r;
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
    unsigned set=(unsigned)ar_hw.sel*(3u*4*240*AR_STRIDE);
    ar_hw.spr=hw_xsets+set; ar_hw.xpf1=ar_hw.spr+4*240*AR_STRIDE; ar_hw.xpf2=ar_hw.xpf1+4*240*AR_STRIDE;
}
void ar_hw_shown(void){ ar_hw.sel^=1; hw_select(); }
long ar_st_kind[3];
static void hw_add(const ArRegion*r){ ar_st_kind[r->kind]++; if(ndisp<AR_MAX_REGIONS) disp[ndisp++]=*r; }
/* ar_planes row y (5 planes = palette index) -> the two playfields: background value (index < 16) and sprite value (index - 16) */
static void hw_encode_row(int y){
    const uint32_t*p4=(const uint32_t*)&ar_planes[4][y*AR_STRIDE];
    for(int k=0;k<4;k++){
        const uint32_t*s=(const uint32_t*)&ar_planes[k][y*AR_STRIDE]; uint32_t*a=(uint32_t*)AR_XPF1(k,y),*b=(uint32_t*)AR_XPF2(k,y);
        for(int i=0;i<AR_STRIDE/4;i++){ uint32_t m=p4[i], v=s[i]; a[i]=v&~m; b[i]=v&m; }
    }
    xdone[y]=1;
}
static void hw_build_lists(int h){
    ar_set(sl_cnt,0,sizeof sl_cnt);
    for(int i=0;i<64;i++){ int sy=ar_scene.oam[i*4]; if(sy>=239) continue;
        int e1=sy+h; if(e1>239) e1=239;
        for(int e=sy;e<e1;e++) if(sl_cnt[e]<8) sl_idx[e][sl_cnt[e]++]=(uint8_t)i; }
    sl_valid=1; sl_h=h;
}
/* the sprite layer line y of the background region s (cover of the clipped background, then the sprites) */
static void hw_line(Nes*n,const ArRegion*s,int y){
    int cl=s->bg_en?(s->min_bg>256?256:s->min_bg):256, clb=cl>>3, used=0;
    uint8_t*L0=AR_SPR(0,y),*L1=AR_SPR(1,y),*L2=AR_SPR(2,y),*L3=AR_SPR(3,y);
    if(clb){ ar_set(L2,0xFF,(unsigned)clb); used=1; }                    /* value 4 = backdrop colour, covers the background pixels */
    if(y>=1 && s->sp_en && s->min_sp<256){
        int h=s->large?16:8; if(!sl_valid || sl_h!=h) hw_build_lists(h);
        int e=y-1, cnt=sl_cnt[e];
        if(cnt){
            uint8_t claim[AR_STRIDE]; { uint32_t*q=(uint32_t*)claim; for(int i=0;i<8;i++) q[i]=0; }
            int row=s->row0+(y-s->y0);
            for(int q=0;q<cnt;q++){
                int i=sl_idx[e][q]; const uint8_t*o=&ar_scene.oam[i*4]; int sy=o[0], ti=o[1], at=o[2], sx=o[3];
                int vm=(at&0x80)!=0, hm=(at&0x40)!=0, prio=(at&0x20)!=0, pal=at&3;
                int lo_= vm ? ((s->large?15:7)-(e-sy)) : (e-sy); uint16_t ta;
                if(s->large) ta=(uint16_t)((((ti&1)?0x1000:0)|((ti&~1)<<4))+(lo_>=8?lo_+8:lo_)); else ta=(uint16_t)(((ti<<4)|(s->sppat?0x1000:0))+lo_);
                const uint8_t*cb=n->map.chr+(uint32_t)(ta<0x1000? s->chr_lo : s->chr_hi)*4096u+(ta&0xFFF);
                uint8_t sl=cb[0], sh1=cb[8]; if(hm){ sl=fliptab[sl]; sh1=fliptab[sh1]; }
                uint8_t opq=(uint8_t)(sl|sh1); if(!opq) continue;
                if(sx<s->min_sp){ int cut=s->min_sp-sx; opq&=(uint8_t)(cut>=8?0:(0xFF>>cut)); if(!opq) continue; }
                int bx=sx>>3, ssh=sx&7; uint16_t mw=(uint16_t)((uint16_t)(opq<<8)>>ssh);
                uint16_t s0w=(uint16_t)((uint16_t)(sl<<8)>>ssh), s1w=(uint16_t)((uint16_t)(sh1<<8)>>ssh);
                for(int w=0;w<2;w++){
                    int bb=bx+w; if(bb>=32) break;
                    uint8_t m8=(uint8_t)(w?mw:(mw>>8)); uint8_t avail=(uint8_t)(m8&~claim[bb]); claim[bb]|=m8;
                    uint8_t D=avail;
                    if(D && prio){                                  /* behind the background: only where the background is transparent (patterns 0) */
                        uint8_t bo=0;
                        if(bb>=clb){ int sh=s->fx; const uint8_t*a=BMP(s->cfg,0,row)+s->bytex+bb,*b=BMP(s->cfg,1,row)+s->bytex+bb;
                            bo=(uint8_t)(((a[0]|b[0])<<sh)|((a[1]|b[1])>>(8-sh))); }
                        D&=(uint8_t)~bo;
                    }
                    if(!D) continue;
                    uint8_t sv0=(uint8_t)(w?s0w:(s0w>>8)), sv1=(uint8_t)(w?s1w:(s1w>>8));
                    L0[bb]=(uint8_t)((L0[bb]&~D)|(sv0&D)); L1[bb]=(uint8_t)((L1[bb]&~D)|(sv1&D));
                    L2[bb]=(uint8_t)((L2[bb]&~D)|((pal&1)?D:0)); L3[bb]=(uint8_t)((L3[bb]&~D)|((pal&2)?D:0));
                    used=1;
                }
            }
        }
    }
    spr_used[ar_hw.sel][y]=(uint8_t)used;
}
/* a scene region -> display regions */
static void hw_region(Nes*n,const ArRegion*r){
    if(r->exact){ ArRegion e=*r; e.kind=AR_EXACT; hw_add(&e); return; }
    if(!r->rend){ ArRegion e=*r; e.kind=AR_BLANK; hw_add(&e); return; }
    uint16_t vl=r->vl; int y=r->y0;
    int bank=r->bgpat? r->chr_hi : r->chr_lo; int c=get_cfg(bank,r->mirror);
    while(y<=r->y1){
        int nt=(vl>>10)&3, cy=(vl>>5)&31, fy=(vl>>12)&7, row=(nt>>1)*256+cy*8+fy;
        ArRegion s=*r; s.kind=AR_BG; s.cfg=(uint8_t)c; s.y0=(int16_t)y; s.row0=(uint16_t)row; s.bytex=(uint8_t)(((vl>>10)&1)*32+(vl&31)); s.vl=vl;
        uint16_t xb=vl&0x41F, v2=vl; int ye=y;
        while(ye<r->y1){
            uint16_t nv=(uint16_t)((incy1(v2)&~0x041F)|r->tx);
            int nrow=(((nv>>10)&3)>>1)*256+((nv>>5)&31)*8+((nv>>12)&7);
            if(nrow!=row+(ye+1-y) || (nv&0x41F)!=xb) break;
            v2=nv; ye++;
        }
        s.y1=(int16_t)ye;
        for(int t=row>>3; t<=(row+(ye-y))>>3; t++){ int ny=row>>8; ensure_trow(n,c,ny*2,t&31); ensure_trow(n,c,ny*2+1,t&31); }
        for(int yy=y;yy<=ye;yy++) hw_line(n,&s,yy);
        hw_add(&s);
        vl=(uint16_t)((incy1(v2)&~0x041F)|r->tx); y=ye+1;
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
                ar_cpy(AR_XPF2(k,y),AR_SPR(k,y),AR_STRIDE);
            }
            xdone[y]=1;
        }
        disp[i].kind=AR_EXACT;
    }
}
void ar_hw_palette(const ArRegion*r,uint8_t out[32]){
    if(r->kind==AR_BLANK){ for(int i=0;i<32;i++) out[i]=r->pal[r->blank_idx&31]; return; }
    for(int i=0;i<16;i++) out[i]=(i&3)?r->pal[i]:r->pal[0];
    for(int i=16;i<32;i++) out[i]=(i&3)?r->pal[i]:r->pal[0];
}
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
        }
    }
}
void ar_end_frame(Nes*n,ArFrame*out){
    ArScene*s=&ar_scene; Ppu*p=&n->ppu; unsigned long long t_end=0;
#ifdef AR_TIMING
    t_end=ar_now();
#endif
    for(int k=0;k<5;k++) out->plane[k]=ar_planes[k];
    ar_flush(n,0); out->nreg=ndisp; for(int i=0;i<ndisp;i++) out->reg[i]=disp[i];
#ifdef AR_TIMING
    unsigned long long t_ex=ar_now();
#endif
    /* exact rows: from the chunky frame buffer (palette index per pixel) */
    for(int y=0;y<240;y++) if(s->exact[y] && !(ar_hw_enable && xdone[y])){
        for(int k=0;k<5;k++) ar_set(&ar_planes[k][y*AR_STRIDE],0,AR_STRIDE);
        for(int x=0;x<256;x++){ unsigned v=p->fb[y*256+x]&31; for(int k=0;k<5;k++) if((v>>k)&1) ar_planes[k][y*AR_STRIDE+(x>>3)]|=(uint8_t)(0x80>>(x&7)); }
        if(ar_hw_enable) hw_encode_row(y);
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
