/* arender.c - planar rendering of the NES picture from the scene log (see arender.h, docs/RENDER.md).  Target independent. */
#include "arender.h"
#include <string.h>

ArScene ar_scene; ArFrame ar_out;
static uint8_t ar_planes[5][240*AR_STRIDE];

/* ---- persistent background bitmaps ------------------------------------------------------------------------------------------------------ */
typedef struct {
    int valid; uint8_t bank, mirror; uint32_t stamp;
    uint32_t bv[4][32]; uint8_t bvok[4][32];          /* write-counter sum of the source rows the bitmap tile row was built from */
} ArCfg;
static ArCfg cfgs[AR_MAX_CFG];
static uint8_t bmp[AR_MAX_CFG][4][512][64];           /* 4 planes (pattern 0/1, palette 0/1) x 512 rows (4 logical nametables) x 64 bytes (512 px) */
static uint32_t stamp_ctr;

static uint16_t nt_phys(int mirror,int nt){     /* physical offset of logical nametable nt (0..3) */
    uint16_t o=0; uint16_t a=(uint16_t)(nt*0x400);
    switch(mirror){ case 0: o=0; break; case 1: o=0x400; break; case 2: o=(uint16_t)(a&0x400); break; default: o=(uint16_t)((a&0x800)>>1); }
    return o;
}
void ar_init(void){ memset(cfgs,0,sizeof cfgs); stamp_ctr=0; }
static int get_cfg(int bank,int mirror){
    int best=0; uint32_t bs=0xFFFFFFFFu;
    for(int i=0;i<AR_MAX_CFG;i++){
        if(cfgs[i].valid && cfgs[i].bank==bank && cfgs[i].mirror==mirror){ cfgs[i].stamp=++stamp_ctr; return i; }
        uint32_t s=cfgs[i].valid? cfgs[i].stamp : 0; if(s<bs){ bs=s; best=i; }
    }
    ArCfg*c=&cfgs[best]; memset(c->bvok,0,sizeof c->bvok); c->valid=1; c->bank=(uint8_t)bank; c->mirror=(uint8_t)mirror; c->stamp=++stamp_ctr;
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
            bmp[c][0][row][col]=chr[ti*16+fy]; bmp[c][1][row][col]=chr[ti*16+fy+8];
            bmp[c][2][row][col]=(pb&1)?0xFF:0; bmp[c][3][row][col]=(pb&2)?0xFF:0;
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
void ar_begin_frame(Nes*n){
    ar_scene.nreg=0; ar_scene.overflow=0; memset(ar_scene.exact,0,sizeof ar_scene.exact); memcpy(ar_scene.oam,n->ppu.oam,256);
}
static int same_params(const ArRegion*a,const ArRegion*b){
    return a->tx==b->tx && a->fx==b->fx && a->rend==b->rend && a->blank_idx==b->blank_idx && a->bg_en==b->bg_en && a->sp_en==b->sp_en && a->min_bg==b->min_bg &&
           a->min_sp==b->min_sp && a->bgpat==b->bgpat && a->sppat==b->sppat && a->large==b->large && a->mirror==b->mirror && a->chr_lo==b->chr_lo &&
           a->chr_hi==b->chr_hi && a->gray==b->gray && a->emph==b->emph && !memcmp(a->pal,b->pal,32);
}
void ar_log_region(Nes*n,const ArRegion*r){
    (void)n; ArScene*s=&ar_scene;
    if(s->overflow) return;
    if(s->nreg){
        ArRegion*l=&s->reg[s->nreg-1];
        if(l->y1+1==r->y0 && same_params(l,r) && (!r->rend || next_vl(l)==r->vl) ){ l->y1=r->y1; return; }
    }
    if(s->nreg>=AR_MAX_REGIONS){ s->overflow=1; return; }
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
    Ppu*p=&n->ppu; (void)p;
    uint16_t vl=r->vl; int h=r->large?16:8;
    for(int y=r->y0;y<=r->y1;y++){
        if(!r->rend){
            for(int k=0;k<5;k++) memset(&ar_planes[k][y*AR_STRIDE],((r->blank_idx>>k)&1)?0xFF:0,AR_STRIDE);
        } else if(!ar_scene.exact[y]){
            int cy=(vl>>5)&31, x0=vl&31, nt=(vl>>10)&3, fy=(vl>>12)&7, ny=nt>>1, nx=nt&1;
            int bank= r->bgpat ? r->chr_hi : r->chr_lo; int c=get_cfg(bank,r->mirror);
            ensure_trow(n,c,nt,cy); ensure_trow(n,c,nt^1,cy);
            int row=ny*256+cy*8+fy; int s=nx*256+x0*8+r->fx; int b0=(s>>3)&63, sh=s&7;
            uint8_t line[5][AR_STRIDE];
            for(int k=0;k<4;k++){
                const uint8_t*src=bmp[c][k][row];
                for(int i=0;i<AR_STRIDE;i++){ int b=(b0+i)&63; line[k][i]= sh? (uint8_t)((src[b]<<sh)|(src[(b+1)&63]>>(8-sh))) : src[b]; }
            }
            memset(line[4],0,AR_STRIDE);
            /* left clip / disabled background: pixels x < min_bg are backdrop (index 0) */
            { int cl= r->bg_en ? (r->min_bg>256?256:r->min_bg) : 256;
              for(int i=0;i<(cl>>3);i++) for(int k=0;k<4;k++) line[k][i]=0; }
            uint8_t bo[AR_STRIDE]; for(int i=0;i<AR_STRIDE;i++){ bo[i]=(uint8_t)(line[0][i]|line[1][i]); line[2][i]&=bo[i]; line[3][i]&=bo[i]; }   /* value 0: backdrop, palette bits 0 */
            /* sprites */
            if(y>=1 && r->sp_en && r->min_sp<256){
                int e=y-1, cnt=0, idx[8];
                for(int i=0;i<64 && cnt<8;i++){ int sy=ar_scene.oam[i*4]; if(e>=sy && e<sy+h) idx[cnt++]=i; }
                uint8_t claim[AR_STRIDE]; memset(claim,0,sizeof claim);
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
            for(int k=0;k<5;k++) memcpy(&ar_planes[k][y*AR_STRIDE],line[k],AR_STRIDE);
        }
        vl=(uint16_t)((incy1(vl)&~0x041F)|r->tx);
    }
}
void ar_end_frame(Nes*n,ArFrame*out){
    ArScene*s=&ar_scene; Ppu*p=&n->ppu;
    for(int k=0;k<5;k++) out->plane[k]=ar_planes[k];
    if(s->overflow){ for(int y=0;y<240;y++) s->exact[y]=1; }
    else for(int i=0;i<s->nreg;i++) render_region(n,&s->reg[i]);
    /* exact rows: from the chunky frame buffer (palette index per pixel) */
    for(int y=0;y<240;y++) if(s->exact[y]){
        for(int k=0;k<5;k++) memset(&ar_planes[k][y*AR_STRIDE],0,AR_STRIDE);
        for(int x=0;x<256;x++){ unsigned v=p->fb[y*256+x]&31; for(int k=0;k<5;k++) if((v>>k)&1) ar_planes[k][y*AR_STRIDE+(x>>3)]|=(uint8_t)(0x80>>(x&7)); }
    }
    out->nreg=s->nreg; for(int i=0;i<s->nreg;i++) out->reg[i]=s->reg[i];
}
void ar_expand(const ArFrame*f,uint8_t*fb){
    for(int y=0;y<240;y++) for(int x=0;x<256;x++){
        unsigned v=0; for(int k=0;k<5;k++) v|=(unsigned)((f->plane[k][y*AR_STRIDE+(x>>3)]>>(7-(x&7)))&1)<<k;
        fb[y*256+x]=(uint8_t)v;
    }
}
