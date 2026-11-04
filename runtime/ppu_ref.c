/* ppu_ref.c - per-dot reference PPU (2C02 NTSC).  One call per dot: slow, host only.  Structure and all timing rules follow the instrumented
 * reference emulator (Mesen) as read in the discovery phase; deviations are listed in FACTS.md ("unverified"). */
#include "nes.h"
Nes nes;
#define P (&n->ppu)

static inline uint16_t nt_off(const Mapper *m, uint16_t a){
    uint16_t o=a&0x3FF;
    switch(m->mirror){ case 0: return o; case 1: return o|0x400; case 2: return o|(a&0x400); default: return o|((a&0x800)>>1); }
}
static uint8_t vram_read(Nes *n, uint16_t a){
    a&=0x3FFF;
    if(a<0x2000) return n->map.chr[(a<0x1000? n->map.chr_lo : n->map.chr_hi) + (a&0xFFF)];
    if(a<0x3F00) return P->nt[nt_off(&n->map,a)];
    return P->pal[a&0x1F];
}
#define PALVAR(p) do{ if((p)->line>=0 && (p)->line<240) (p)->palvar=1; }while(0)
static void vram_write(Nes *n, uint16_t a, uint8_t v){
    a&=0x3FFF;
    if(a<0x2000) return;
    if(a<0x3F00){ uint16_t o=nt_off(&n->map,a); P->nt[o]=v; P->ntv[o>>5]++; P->ntver_all++; return; }
    uint8_t i=a&0x1F; v&=0x3F;
    if((i&3)==0){ if(P->pal[i&0x0F]!=v||P->pal[(i&0x0F)|0x10]!=v) PALVAR(P); P->pal[i&0x0F]=v; P->pal[(i&0x0F)|0x10]=v; } else { if(P->pal[i]!=v) PALVAR(P); P->pal[i]=v; }
}

void ppu_reset(Ppu *p){ memset(p,0,sizeof *p); p->draw=1; p->oam_dirty=1; p->fc[0].y=p->fc[1].y=-1; p->oa_line=-2; p->line=-1; p->cx=340; p->frame=1; p->min_bg=300; p->min_sp=300; p->min_sp_std=300; }

static void inc_x(Ppu*p){ uint16_t a=p->v; if((a&0x1F)==31) a=(a&~0x1F)^0x400; else a++; p->v=a; }
static void inc_y(Ppu*p){
    uint16_t a=p->v;
    if((a&0x7000)!=0x7000) a+=0x1000;
    else { a&=~0x7000; int y=(a&0x3E0)>>5; if(y==29){ y=0; a^=0x800; } else if(y==31) y=0; else y++; a=(a&~0x3E0)|(y<<5); }
    p->v=a;
}
static void update_min_draw(Ppu*p){
    p->min_bg = p->bg_en ? ((p->mask&2)?0:8) : 300;
    p->min_sp = p->sp_en ? ((p->mask&4)?0:8) : 300;
    p->min_sp_std = p->min_sp;
}

static void load_tile_info(Nes *n){
    Ppu*p=P; if(!p->rend) return;
    switch(p->cx&7){
    case 1: {
        p->tprev=p->tcur; p->tcur=p->tnext;
        p->sh_lo|=p->tnext.lo; p->sh_hi|=p->tnext.hi;
        uint8_t ti=vram_read(n,0x2000|(p->v&0x0FFF));
        p->tnext.addr=(uint16_t)((ti<<4)|(p->v>>12)|p->bgpat);
        p->tnext.offy=p->v>>12;
        break; }
    case 3: {
        uint8_t sh=((p->v>>4)&4)|(p->v&2);
        uint16_t aa=0x23C0|(p->v&0x0C00)|((p->v>>4)&0x38)|((p->v>>2)&7);
        p->tnext.paloff=(uint8_t)(((vram_read(n,aa)>>sh)&3)<<2);
        break; }
    case 5: p->tnext.lo=vram_read(n,p->tnext.addr); break;
    case 7: p->tnext.hi=vram_read(n,p->tnext.addr+8); break;
    }
}

static void load_sprite(Nes *n, uint8_t sy, uint8_t ti, uint8_t at, uint8_t sx){
    Ppu*p=P; int fetch_last=1;
    int bgp=(at&0x20)!=0, hm=(at&0x40)!=0, vm=(at&0x80)!=0; uint16_t ta; uint8_t lo_;
    if(vm) lo_=(uint8_t)((p->large?15:7)-(p->line-sy)); else lo_=(uint8_t)(p->line-sy);
    if(p->large) ta=(uint16_t)((((ti&1)?0x1000:0)|((ti&~1)<<4))+(lo_>=8?lo_+8:lo_));
    else ta=(uint16_t)(((ti<<4)|p->sppat)+lo_);
    if(p->sp_index<p->sp_count && sy<240){
        SpriteTile*s=&p->st[p->sp_index];
        s->bgprio=bgp; s->hflip=hm; s->vflip=vm; s->paloff=(uint8_t)(((at&3)<<2)|0x10);
        fetch_last=0; s->lo=vram_read(n,ta); s->hi=vram_read(n,ta+8); s->x=sx;
        if(p->line>=0) for(int i=0;i<8 && sx+i+1<257;i++) p->has_sprite[sx+i+1]=1;
    }
    p->sp_index++; (void)fetch_last;
}

static void sprite_eval(Nes *n){
    Ppu*p=P;
    if(!p->rend) return;
    if(p->cx<65){ p->oamcopy=0xFF; p->oam2[(p->cx-1)>>1]=0xFF; return; }
    if(p->cx==65){ p->s0_added=0; p->sp_inrange=0; p->sec_addr=0; p->ovf_bug=0; p->copy_done=0; p->addrH=(p->oamaddr>>2)&0x3F; p->addrL=p->oamaddr&3; }
    else if(p->cx==256){ p->s0_visible=p->s0_added; p->sp_count=p->sec_addr>>2; }
    if(p->cx&1){ p->oamcopy=p->oam[p->oamaddr]; return; }
    if(p->copy_done){
        p->addrH=(p->addrH+1)&0x3F;
        if(p->sec_addr>=0x20) p->oamcopy=p->oam2[p->sec_addr&0x1F];
    } else {
        if(!p->sp_inrange && p->line>=p->oamcopy && p->line<p->oamcopy+(p->large?16:8)) p->sp_inrange=1;
        if(p->sec_addr<0x20){
            p->oam2[p->sec_addr]=p->oamcopy;
            if(p->sp_inrange){
                p->addrL++; p->sec_addr++;
                if(p->addrH==0) p->s0_added=1;
                if((p->sec_addr&3)==0){ p->sp_inrange=0; p->addrL=0; p->addrH=(p->addrH+1)&0x3F; if(p->addrH==0) p->copy_done=1; }
            } else { p->addrH=(p->addrH+1)&0x3F; if(p->addrH==0) p->copy_done=1; }
        } else {
            p->oamcopy=p->oam2[p->sec_addr&0x1F];
            if(p->sp_inrange){
                p->spovf=1; p->addrL++; if(p->addrL==4){ p->addrH=(p->addrH+1)&0x3F; p->addrL=0; }
                if(p->ovf_bug==0) p->ovf_bug=3; else if(p->ovf_bug>0){ p->ovf_bug--; if(p->ovf_bug==0){ p->copy_done=1; p->addrL=0; } }
            } else {
                p->addrH=(p->addrH+1)&0x3F; p->addrL=(p->addrL+1)&3; if(p->addrH==0) p->copy_done=1;
            }
        }
    }
    p->oamaddr=(uint8_t)((p->addrL&3)|(p->addrH<<2));
}

static uint8_t pixel_color(Ppu*p){
    uint8_t off=p->fx, bgc=0, spbg=0; int cx=p->cx;
    if(cx>p->min_bg){
        spbg=(uint8_t)((((p->sh_lo<<off)&0x8000)>>15)|(((p->sh_hi<<off)&0x8000)>>14));
        if(1) bgc=spbg;
    }
    if(p->has_sprite[cx] && cx>p->min_sp){
        for(int i=0;i<p->sp_count;i++){
            int sh=cx-p->st[i].x-1;
            if(sh>=0&&sh<8){
                const SpriteTile*s=&p->st[i]; uint8_t sc;
                if(s->hflip) sc=(uint8_t)(((s->lo>>sh)&1)|(((s->hi>>sh)&1)<<1)); else sc=(uint8_t)((((s->lo<<sh)&0x80)>>7)|(((s->hi<<sh)&0x80)>>6));
                if(sc){
                    if(i==0 && spbg && p->s0_visible && cx!=256 && p->bg_en && !p->sp0hit && cx>p->min_sp_std) p->sp0hit=1;
                    if(p->sp_en && (bgc==0 || !s->bgprio)) return (uint8_t)(s->paloff+sc);
                    break;
                }
            }
        }
    }
    return (uint8_t)(((off+((cx-1)&7)<8)? p->tprev : p->tcur).paloff+bgc);
}

#ifdef PPU_FB5
#define PIXV(col,idx) ((uint8_t)(idx))
#else
#define PIXV(col,idx) FBVAL((col)&(p->gray?0x30:0x3F),p->emph)
#endif
static void draw_pixel(Nes *n){
    Ppu*p=P; uint8_t c; int idx;
    if(p->rend || (p->v&0x3F00)!=0x3F00){ c=pixel_color(p); idx=(c&3)?c:0; }
    else { idx=p->v&0x1F; (void)pixel_color(p); }
    uint8_t col=p->pal[idx];
    if(p->draw){ p->fb[(p->line<<8)+p->cx-1]=PIXV(col,idx); p->rowdirty[p->line]=1; p->rowok[p->line]=0; p->rowblank[p->line]=0; }
}

static void process_scanline(Nes *n){
    Ppu*p=P; int cx=p->cx;
    if(cx<=256){
        load_tile_info(n);
        if(p->rend_prev && (cx&7)==0){ inc_x(p); if(cx==256) inc_y(p); }
        if(p->line>=0){ draw_pixel(n); p->sh_lo<<=1; p->sh_hi<<=1; sprite_eval(n); }
        else if(cx<9){
            if(cx==1){ p->vbl=0; p->nmi_level=0; }
            if(p->oamaddr>=8 && p->rend){ p->oam[cx-1]=p->oam[(p->oamaddr&0xF8)+cx-1]; p->oam_dirty=1; }
        }
    } else if(cx<=320){
        if(cx==257){ p->sp_index=0; memset(p->has_sprite,0,sizeof p->has_sprite); if(p->rend_prev) p->v=(p->v&~0x041F)|(p->t&0x041F); }
        if(p->rend){
            p->oamaddr=0;
            if(((cx-261)&7)==0 && cx>=261){
                const uint8_t*s=p->oam2+p->sp_index*4;
                if(p->sp_index<8) load_sprite(n,s[0],s[1],s[2],s[3]); else p->sp_index++;
            }
            if(p->line==-1 && cx>=280 && cx<=304) p->v=(p->v&~0x7BE0)|(p->t&0x7BE0);
        }
    } else if(cx<=336){
        load_tile_info(n);
        if(cx==321){ if(p->rend) p->oamcopy=p->oam2[0]; }
        else if(p->rend_prev && (cx==328||cx==336)){ p->sh_lo<<=8; p->sh_hi<<=8; inc_x(p); }
    } else if(cx==337||cx==339){
        if(p->rend && p->line==-1 && cx==339 && (p->frame&1)) p->cx=340;
    }
}

extern void nes_input_poll(void);   /* board: called at the start of line 241 (the reference emulator polls input there) */
int ppu_noinput;                    /* host self-check: the second (reference) PPU instance must not poll the input */
static void first_cycle(Nes *n){
    Ppu*p=P; p->cx=0;
    if(++p->line>260){ p->line=-1; p->sp_count=0; }
    if(p->line==241 && !ppu_noinput) nes_input_poll();
    if(p->line<240){
        if(p->line==-1){ p->spovf=0; p->sp0hit=0; }
    } else if(p->line==240){ p->frame++; p->frame_done=1; }
}

static void fp_oam_fix(Nes*n);
static void update_state(Nes *n){
    Ppu*p=P; p->need_update=0;
    if(p->rend_prev!=p->rend){ p->rend_prev=p->rend; }
    int nr=(p->bg_en|p->sp_en); if(p->rend!=nr){ if(p->fp && p->rend && !nr) fp_oam_fix(n); p->rend=nr; p->need_update=1; }
    if(p->upd_delay>0){ p->upd_delay--; if(p->upd_delay==0){ p->v=p->upd_addr; p->t=p->v; } else p->need_update=1; }
}

void ppu_step(Nes *n){
    Ppu*p=P; p->dots++;
    if(p->cx<340){
        p->cx++;
        if(p->line<240) process_scanline(n);
        else if(p->cx==1 && p->line==241){ if(!p->prevent_vbl){ p->vbl=1; if(p->ctrl&0x80) p->nmi_level=1; } p->prevent_vbl=0; }
    } else first_cycle(n);
    if(p->need_update) update_state(n);
}

/* ---------------------------------------------------------------- registers */
static void set_ctrl(Nes *n, uint8_t v){
    Ppu*p=P; p->ctrl=v;
    p->t=(uint16_t)((p->t&~0x0C00)|((v&3)<<10));
    p->vinc=(v&4)?32:1; p->sppat=(v&8)?0x1000:0; p->bgpat=(v&0x10)?0x1000:0; p->large=(v&0x20)!=0;
    if(!(v&0x80)) p->nmi_level=0; else if(p->vbl) p->nmi_level=1;
}
static void set_mask(Nes *n, uint8_t v){
    Ppu*p=P; if(((v^p->mask)&0xE1)) PALVAR(p); p->mask=v; p->gray=v&1; p->bg_en=(v&8)!=0; p->sp_en=(v&0x10)!=0; p->emph=(v>>5)&7;
    if(p->rend!=(p->bg_en|p->sp_en)) p->need_update=1;
    update_min_draw(p);
}
uint8_t ppu_reg_read(Nes *n, uint16_t a){
    Ppu*p=P; uint8_t r=p->openbus;
    switch(a&7){
    case 2: {
        p->w=0;
        uint8_t st=(uint8_t)((p->spovf<<5)|(p->sp0hit<<6)|(p->vbl<<7));
        p->vbl=0; p->nmi_level=0;
        if(p->line==241 && p->cx==0) p->prevent_vbl=1;
        r=(uint8_t)(st|(p->openbus&0x1F)); break; }
    case 4: if(p->fp) fp_oam_fix(n); r=p->oam[p->oamaddr]; break;
    case 7: {
        uint16_t va=p->v&0x3FFF; uint8_t old=p->rdbuf;
        p->rdbuf=vram_read(n,va);
        if(va>=0x3F00) r=(uint8_t)((p->pal[va&0x1F]&(p->gray?0x30:0x3F))|(p->openbus&0xC0)); else r=old;
        p->v=(uint16_t)(p->v+p->vinc); break; }
    }
    p->openbus=r; return r;
}
void ppu_reg_write(Nes *n, uint16_t a, uint8_t v){
    Ppu*p=P; p->openbus=v;
    switch(a&7){
    case 0: set_ctrl(n,v); break;
    case 1: set_mask(n,v); break;
    case 3: p->oamaddr=v; break;
    case 4:
        if(p->fp) fp_oam_fix(n);
        if(p->line>=240 || !p->rend){ uint8_t b=v; if((p->oamaddr&3)==2) b&=0xE3; p->oam[p->oamaddr]=b; p->oamaddr++; p->oam_dirty=1; }
        else p->oamaddr+=4;
        break;
    case 5:
        if(p->w) p->t=(uint16_t)((p->t&~0x73E0)|((v&0xF8)<<2)|((v&7)<<12));
        else { p->fx=v&7; p->t=(uint16_t)((p->t&~0x1F)|(v>>3)); }
        p->w^=1; break;
    case 6:
        if(p->w){ p->t=(uint16_t)((p->t&~0xFF)|v); p->need_update=1; p->upd_delay=3; p->upd_addr=p->t; }
        else p->t=(uint16_t)((p->t&~0xFF00)|((v&0x3F)<<8));
        p->w^=1; break;
    case 7:
        if((p->v&0x3FFF)>=0x3F00) vram_write(n,p->v,v);
        else if(p->line>=240 || !p->rend) vram_write(n,p->v,v);
        else vram_write(n,p->v,(uint8_t)p->v);
        p->v=(uint16_t)(p->v+p->vinc); break;
    }
}



/* ======================================================================================================================================
 * Fast model.  The PPU is advanced between two bus accesses in closed form instead of dot by dot.  Nothing here is approximate for the
 * situations the model handles (it is proven equal to ppu_step by the host self-check, PPU_CHECK=1 in tools/rt_host.c); the per-dot model above
 * stays the reference (RT_NOFAST=1 runs it alone).
 *
 * Between two accesses every parameter is constant, so the dots in between are processed as "pieces" (line, first dot, last dot):
 *   - v/t/oamaddr/flag effects of the piece in closed form (v_adv): coarse-X increments, the dot-256 Y increment, the dot-257 horizontal copy, the
 *     pre-render vertical copy, odd-frame dot skip, vblank flag at (241,1), clear at (-1,0)/(-1,1), sprite overflow at its evaluation dot;
 *   - when pixels are drawn (nes.ppu.draw) or sprite 0 can hit: the tile and sprite fetches whose dots lie in the piece are performed with the
 *     parameters of the piece (FpCtx: pattern address at the NT-fetch dot, attribute, CHR bytes at their dots: a CHR bank switch in the middle of a
 *     line affects exactly the fetches after it), the pixels of the piece are produced from them and sprite 0 is tested against the background.
 * Whole lines nothing can happen in are skipped in one step (v after m lines is closed form), so an idle frame costs a few hundred cycles.
 * Hidden pipeline registers of the per-dot model (tprev/tcur/tnext, shifters, oam2, st[]) are not maintained; the contexts above replace them.
 * ====================================================================================================================================== */
long ppu_fast_lines, ppu_exotic, ppu_st[16];
#ifdef PPU_STATS
#define ST(i) (ppu_st[i]++)
#else
#define ST(i) ((void)0)
#endif
#ifdef FP_NOFASTLINE
#define FP_GENERIC 1
#else
#define FP_GENERIC 0
#endif     /* statistics: lines drawn / situations the model only approximates (see ppu_exotic uses) */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
#define EXB(k) ((((k)>>3)&1u)|((((k)>>2)&1u)<<8)|((((k)>>1)&1u)<<16)|(((k)&1u)<<24))     /* host byte order: the first pixel is the lowest byte */
#else
#define EXB(k) (((((k)>>3)&1u)<<24)|((((k)>>2)&1u)<<16)|((((k)>>1)&1u)<<8)|((k)&1u))      /* 68k: the first pixel is the top byte */
#endif
static const uint32_t expn[16]={EXB(0),EXB(1),EXB(2),EXB(3),EXB(4),EXB(5),EXB(6),EXB(7),EXB(8),EXB(9),EXB(10),EXB(11),EXB(12),EXB(13),EXB(14),EXB(15)};   /* 4 pattern bits -> 4 bytes 0/1 */
typedef uint32_t __attribute__((may_alias)) u32a;                     /* word access to byte arrays / structs (may alias anything) */
typedef uint32_t __attribute__((may_alias, aligned(1))) u32u;         /* the same, possibly unaligned */
#if defined(PPU_FB8) || defined(PPU_FB5)
#define FB_BYTES 1
#endif
#ifdef PPU_FB5
#define FP_FASTLINE 1          /* 5-plane build: background row cache and tight whole-line loops (fp_line) */
#endif
#ifdef PPU_FB5
#define CV(i) ((uint8_t)(i))
#else
#define CV(i) ((uint8_t)(p->pal[i]&gm))
#endif

static inline uint16_t inc_y16(uint16_t a){
    if((a&0x7000)!=0x7000) return (uint16_t)(a+0x1000);
    a&=~0x7000; int y=(a&0x3E0)>>5; if(y==29){ y=0; a^=0x800; } else if(y==31) y=0; else y++; return (uint16_t)((a&~0x3E0)|(y<<5));
}
static inline uint16_t incx_n(uint16_t v,int n){ int x=(v&31)+n; return (uint16_t)((v&~0x41F)|(x&31)|((((v>>10)^(x>>5))&1)<<10)); }
static inline uint16_t decx_n(uint16_t v,int n){ int x=(v&31)-n, w=0; if(x<0){ w=(31-x)>>5; x+=32*w; } return (uint16_t)((v&~0x41F)|x|((((v>>10)^w)&1)<<10)); }
static uint16_t incy_n(uint16_t v,int n){      /* n Y increments */
    while(n>0){
        int f=(v>>12)&7;
        if(f+n<8) return (uint16_t)((v&~0x7000)|((f+n)<<12));
        n-=8-f; v&=~0x7000;
        int y=(v&0x3E0)>>5; if(y==29){ y=0; v^=0x800; } else if(y==31) y=0; else y++;
        v=(uint16_t)((v&~0x3E0)|(y<<5));
    }
    return v;
}
/* effect of the dots c0+1..c1 of line L on v (rend / rend_prev constant over the dots) */
static uint16_t v_adv(const Ppu*p,uint16_t v,int L,int c0,int c1){
    if(p->rend_prev){
        int lo=c0<0?0:c0, hi=c1<256?c1:256;
        if(hi>lo){ int k=(hi>>3)-(lo>>3); if(k>0) v=incx_n(v,k); }
        if(c0<256 && c1>=256) v=inc_y16(v);
        if(c0<257 && c1>=257) v=(uint16_t)((v&~0x041F)|(p->t&0x041F));
        if(c0<328 && c1>=328) v=incx_n(v,1);
        if(c0<336 && c1>=336) v=incx_n(v,1);
    }
    if(L<0 && p->rend && c1>=280 && c0<304) v=(uint16_t)((v&~0x7BE0)|(p->t&0x7BE0));
    return v;
}

/* ---- per-line sprite tables (rebuilt after an OAM write / sprite size change) ----------------------------------------------------- */
static void lt_build(Nes*n){
    Ppu*p=P; int h=p->large?16:8; int16_t d[242];
    memset(d,0,sizeof d);
    for(int i=0;i<64;i++){ int sy=p->oam[i<<2]; if(sy>=240) continue; int e=sy+h; if(e>240) e=240; d[sy]++; d[e]--; }
    int c=0; for(int l=0;l<240;l++){ c+=d[l]; p->lt_cnt[l]=(uint8_t)c; }
    memset(p->lt_ocx,0xFF,sizeof p->lt_ocx);
    p->lt_valid=1; p->lt_full=0; p->lt_large=p->large; p->oam_dirty=0;
}
static inline void lt_ensure(Nes*n){ Ppu*p=P; if(p->oam_dirty || !p->lt_valid || p->lt_large!=p->large) lt_build(n); }
static void lt_build_full(Nes*n){
    Ppu*p=P; int h=p->large?16:8; uint8_t c[240]; memset(c,0,sizeof c);
    for(int i=0;i<64;i++){ int sy=p->oam[i<<2]; if(sy>=240) continue; int e=sy+h; if(e>240) e=240; for(int l=sy;l<e;l++){ if(c[l]<8) p->lt_idx[l][c[l]]=(uint8_t)i; c[l]++; } }
    p->lt_full=1;
}
/* dot (cx) at which the sprite evaluation of line L sets the overflow flag (0: never): port of sprite_eval for the even dots 66..256, start OAM address 0 */
static int walk_ovf(Ppu*p,int L){
    int h=p->large?16:8, addrH=0, addrL=0, sec=0, inr=0, done=0, bug=0;
    for(int cx=66;cx<=256;cx+=2){
        int oc=p->oam[(addrL&3)|(addrH<<2)];
        if(done){ addrH=(addrH+1)&0x3F; continue; }
        if(!inr && L>=oc && L<oc+h) inr=1;
        if(sec<0x20){
            if(inr){ addrL++; sec++; if((sec&3)==0){ inr=0; addrL=0; addrH=(addrH+1)&0x3F; if(addrH==0) done=1; } }
            else { addrH=(addrH+1)&0x3F; if(addrH==0) done=1; }
        } else {
            if(inr){
                return cx;                       /* the flag is set; what follows does not matter any more */
            } else { addrH=(addrH+1)&0x3F; addrL=(addrL+1)&3; if(addrH==0) done=1; }
        }
        (void)bug;
    }
    return 0;
}
/* OAM address of the sprite evaluation of line L after its step at dot upto (even dots 66..), start address `start` */
static int walk_addr(Ppu*p,int L,int upto,int start){
    int h=p->large?16:8, addrH=(start>>2)&0x3F, addrL=start&3, sec=0, inr=0, done=0, bug=0;
    for(int cx=66;cx<=upto;cx+=2){
        int oc=p->oam[(addrL&3)|(addrH<<2)];
        if(done){ addrH=(addrH+1)&0x3F; continue; }
        if(!inr && L>=oc && L<oc+h) inr=1;
        if(sec<0x20){
            if(inr){ addrL++; sec++; if((sec&3)==0){ inr=0; addrL=0; addrH=(addrH+1)&0x3F; if(addrH==0) done=1; } }
            else { addrH=(addrH+1)&0x3F; if(addrH==0) done=1; }
        } else if(inr){
            addrL++; if(addrL==4){ addrH=(addrH+1)&0x3F; addrL=0; }
            if(bug==0) bug=3; else if(bug>0){ bug--; if(bug==0){ done=1; addrL=0; } }
        } else { addrH=(addrH+1)&0x3F; addrL=(addrL+1)&3; if(addrH==0) done=1; }
    }
    return (addrL&3)|(addrH<<2);
}
/* the fast model does not track the OAM address while the sprite evaluation runs (dots 65..256 of the lines 0..239); bring it up to date when it is observed */
static void fp_oam_fix(Nes*n){
    Ppu*p=P; if(!p->rend || p->line<0 || p->line>=240 || p->cx<66 || p->cx>256) return;
    if(p->oa_line!=p->line){ p->oa_line=(int16_t)p->line; p->oa_start=p->oamaddr; }
    p->oamaddr=(uint8_t)walk_addr(p,p->line,p->cx&~1,p->oa_start);
}
static inline int lt_ovf_cx(Nes*n,int L){
    Ppu*p=P; if(p->lt_ocx[L]==-1) p->lt_ocx[L]=(int16_t)(p->lt_cnt[L]>=8 ? walk_ovf(p,L) : 0);
    return p->lt_ocx[L];
}

/* ---- fetch contexts ---------------------------------------------------------------------------------------------------------------- */
static inline int s0_in_range(Ppu*p,int e){ int sy=p->oam[0], h=p->large?16:8; return sy<240 && e>=sy && e<sy+h; }
/* what is needed for output line y: 0 nothing, 1 sprite 0 only (hit test), 2 everything (the line is drawn) */
static inline int ctx_mode(Nes*n,int y){
    Ppu*p=P; if(y<0||y>239) return 0;
    if(p->draw) return 2;
    if(!p->sp0hit && s0_in_range(p,y-1)) return 1;
    return 0;
}
static void ctx_init(Nes*n,FpCtx*c,int y,int mode){
    Ppu*p=P; int e=y-1;
    c->y=(int16_t)y; c->mode=(uint8_t)mode; c->nspr=0; c->s0=0; c->jlo=0; c->jhi=32; c->t01ok=0;
    if(mode==1){ int sx=p->oam[3]; c->jlo=(uint8_t)(sx>>3); c->jhi=(uint8_t)(((sx+14)>>3)>32?32:((sx+14)>>3)); if(e>=0 && s0_in_range(p,e)){ c->nspr=1; c->s0=1; } }
    else if(e>=0){
        lt_ensure(n); if(!p->lt_full) lt_build_full(n);
        int cnt=p->lt_cnt[e]; c->nspr=(uint8_t)(cnt>8?8:cnt); c->s0=(uint8_t)(c->nspr && p->lt_idx[e][0]==0);
    }
}
static void fetch_sprite(Nes*n,FpCtx*c,int k){
    Ppu*p=P; int e=c->y-1;
    int oi = c->mode==2 ? p->lt_idx[e][k] : 0;       /* mode 1: slot 0 is sprite 0 */
    int sy=p->oam[oi*4], ti=p->oam[oi*4+1], at=p->oam[oi*4+2], sx=p->oam[oi*4+3];
    int vm=(at&0x80)!=0; int lo_= vm ? ((p->large?15:7)-(e-sy)) : (e-sy); uint16_t ta;
    if(p->large) ta=(uint16_t)((((ti&1)?0x1000:0)|((ti&~1)<<4))+(lo_>=8?lo_+8:lo_)); else ta=(uint16_t)(((ti<<4)|p->sppat)+lo_);
    c->slo[k]=vram_read(n,ta); c->shi[k]=vram_read(n,ta+8); c->spx[k]=(uint8_t)sx; c->spal[k]=(uint8_t)(((at&3)<<2)|0x10); c->sprio[k]=(at&0x20)!=0; c->shf[k]=(at&0x40)!=0;
}
/* the four fetches of tile j (dots b, b+2, b+4, b+6 of the line) that lie in a..c1; v = value of v during them */
static void fetch_tile(Nes*n,FpCtx*c,int j,int b,uint16_t v,int a,int c1){
    Ppu*p=P;
    if(b>=a && b<=c1){ uint8_t ti=p->nt[nt_off(&n->map,0x2000|(v&0x0FFF))]; c->tta[j]=(uint16_t)((ti<<4)|(v>>12)|p->bgpat); }
    if(b+2>=a && b+2<=c1){
        uint8_t sh=(uint8_t)(((v>>4)&4)|(v&2)); uint16_t aa=(uint16_t)(0x23C0|(v&0x0C00)|((v>>4)&0x38)|((v>>2)&7));
        c->tpal[j]=(uint8_t)(((p->nt[nt_off(&n->map,aa)]>>sh)&3)<<2);
    }
    if(b+4>=a && b+4<=c1){ uint16_t ta=c->tta[j]; c->tlo[j]=n->map.chr[(ta<0x1000? n->map.chr_lo : n->map.chr_hi)+(ta&0xFFF)]; }
    if(b+6>=a && b+6<=c1){ uint16_t ta=(uint16_t)(c->tta[j]+8); c->thi[j]=n->map.chr[(ta<0x1000? n->map.chr_lo : n->map.chr_hi)+(ta&0xFFF)]; }
}
/* whole context of line y from the current parameters; vline = v at the output start (tile j is fetched with vline+j); the line before supplies
 * the sprites and tiles 0 and 1 */
static void ctx_build(Nes*n,FpCtx*c,int y,int mode,uint16_t vline){
    ctx_init(n,c,y,mode);
    for(int k=0;k<c->nspr;k++) fetch_sprite(n,c,k);
    uint16_t va=vline;
    for(int j=0;j<=32;j++){
        if(j>=c->jlo && j<=c->jhi) fetch_tile(n,c,j,0,va,0,8);
        va=incx_n(va,1);
    }
    c->t01ok=3;
}
/* v at the output start of the line being processed, from the live v after dot c0 (c0 < 257) */
static uint16_t vline_live(Ppu*p,int c0){ int k= (p->rend_prev && c0>=8) ? (c0>256?32:(c0>>3)) : 0; return decx_n(decx_n(p->v,k),2); }

static void ctx_ensure(Nes*n,int L,int a){      /* exotic fallback: the context of the line being output was not prepared by the previous line */
    Ppu*p=P; FpCtx*cc=&p->fc[p->fcur]; if(cc->y==L) return;
    int mode=ctx_mode(n,L); if(!mode) return;
    ppu_exotic++;
    ctx_build(n,cc,L,mode,vline_live(p,a-1));
}

/* ---- pixels ------------------------------------------------------------------------------------------------------------------------- */
#ifdef FB_BYTES
typedef uint8_t fbpix;
#else
typedef uint16_t fbpix;
#endif
#ifdef FP_FASTLINE
static void bk_inval(int L);
#define BK_INVAL(L) bk_inval(L)
#else
#define BK_INVAL(L) ((void)0)
#endif
static void draw_range(Nes*n,int L,int x0,int x1){
    Ppu*p=P; FpCtx*c=&p->fc[p->fcur];
    uint8_t emph=p->emph, gm=p->gray?0x30:0x3F; (void)gm; (void)emph;
    fbpix*row=&p->fb[L<<8]; const fbpix EM=FBVAL(0,emph);
    p->rowblank[L]=0; BK_INVAL(L);
    if(!p->rend){
        p->rowdirty[L]=1;
        int idx= ((p->v&0x3F00)!=0x3F00) ? 0 : (p->v&0x1F);
        fbpix c8=FBVAL(CV(idx),emph);
        p->rowok[L]=0;
        for(int x=x0;x<x1;x++) row[x]=c8;
        return;
    }
    if(c->y!=L) return;      /* no context (exotic) */
    int fx=p->fx, mb=p->min_bg, clipn= mb>256?256:mb; if(!p->bg_en) clipn=256;
    int j0=(x0+fx)>>3, j1=(x1-1+fx)>>3; if(j0<c->jlo) j0=c->jlo; if(j1>c->jhi) j1=c->jhi;
    uint32_t sig=0;
    if(x0==0&&x1==256){
        sig=0x811C9DC5u;
        for(int j=0;j<=32;j++){ uint32_t w=c->tlo[j]|((uint32_t)c->thi[j]<<8)|((uint32_t)c->tpal[j]<<16); sig=((sig<<5)|(sig>>27))^w; sig*=0x01000193u; }
        for(int i=0;i<c->nspr;i++){ uint32_t w=c->slo[i]|((uint32_t)c->shi[i]<<8)|((uint32_t)c->spx[i]<<16); sig=((sig<<5)|(sig>>27))^w; sig*=0x01000193u;
            w=c->spal[i]|((uint32_t)c->sprio[i]<<8)|((uint32_t)c->shf[i]<<9); sig=((sig<<5)|(sig>>27))^w; sig*=0x01000193u; }
#ifndef PPU_FB5
        { const u32a*pl=(const u32a*)p->pal; for(int i=0;i<8;i++){ sig=((sig<<5)|(sig>>27))^pl[i]; sig*=0x01000193u; } }
#endif
        { uint32_t w=(uint32_t)fx|((uint32_t)p->min_bg<<3)|((uint32_t)(p->min_sp&0x1FF)<<12)|((uint32_t)gm<<21)|((uint32_t)emph<<27)|((uint32_t)p->sp_en<<30)|((uint32_t)p->bg_en<<31); sig=((sig<<5)|(sig>>27))^w; sig*=0x01000193u; sig^=c->nspr; }
#if !defined(NOSIG) && !defined(FP_FASTLINE)
        if(p->rowok[L] && p->rowsig[L]==sig) return;
        p->rowsig[L]=sig; p->rowok[L]=1;
#else
        p->rowok[L]=0;
#endif
    } else p->rowok[L]=0;
    p->rowdirty[L]=1;
    uint8_t bgraw[272] __attribute__((aligned(4))), bgc[272] __attribute__((aligned(4)));
    u32a*raw32=(u32a*)bgraw; u32a*c32=(u32a*)bgc; uint8_t pal0=CV(0);
    for(int j=j0;j<=j1;j++){
        uint8_t lo=c->tlo[j], hi=c->thi[j], pbase=c->tpal[j];
        uint32_t r0=expn[lo>>4]|(expn[hi>>4]<<1), r1=expn[lo&15]|(expn[hi&15]<<1);
        raw32[2*j]=r0; raw32[2*j+1]=r1;
#ifdef PPU_FB5
        { uint32_t pb=pbase*0x01010101u; c32[2*j]=r0+(((r0|(r0>>1))&0x01010101u)*pbase); c32[2*j+1]=r1+(((r1|(r1>>1))&0x01010101u)*pbase); (void)pb; }
#else
        uint8_t m1=CV(pbase+1), m2=CV(pbase+2), m3=CV(pbase+3);
        for(int h=0;h<2;h++){ uint32_t r=h?r1:r0; uint32_t o=0;
            for(int k=0;k<4;k++){ unsigned v=(r>>(24-8*k))&3; o=(o<<8)|(v==0?pal0:v==1?m1:v==2?m2:m3); }
            c32[2*j+h]=o; }
#endif
    }
    { const uint8_t*src=bgc+fx;
      for(int x=x0;x<x1;x++) row[x]=(fbpix)(src[x]|EM);
      for(int x=x0;x<clipn&&x<x1;x++) row[x]=(fbpix)(pal0|EM); }
    if(c->nspr){
        uint8_t claim[256]; for(int x=x0;x<x1;x++) claim[x]=0;
        for(int i=0;i<c->nspr;i++){
            int sx=c->spx[i]; uint8_t lo=c->slo[i],hi=c->shi[i];
            if(!(lo|hi) || sx+7<x0 || sx>=x1) continue;
            for(int k=0;k<8;k++){
                int x=sx+k; if(x>255) break; if(x<x0||x>=x1) continue;
                if(x<p->min_sp) continue;
                uint8_t sc= c->shf[i] ? (uint8_t)(((lo>>k)&1)|(((hi>>k)&1)<<1)) : (uint8_t)(((lo>>(7-k))&1)|(((hi>>(7-k))&1)<<1));
                if(!sc || claim[x]) continue;
                claim[x]=1;
                uint8_t braw = (x>=clipn) ? bgraw[x+fx] : 0;
                if(!p->bg_en) braw=0;
                if(p->sp_en && (braw==0 || !c->sprio[i])) row[x]=FBVAL(CV(c->spal[i]+sc),emph);
            }
        }
    }
}
/* sprite 0 against the background in the dots a..c1 (cx, 1..256) of the line the context belongs to: first hit dot or 0 */
static int hit_find(Ppu*p,const FpCtx*c,int a,int c1){
    if(!c->s0) return 0;
    int sx=c->spx[0]; uint8_t lo=c->slo[0], hi=c->shi[0]; if(!(lo|hi)) return 0;
    int cxa= sx+1>a ? sx+1 : a, cxb= sx+8<c1 ? sx+8 : c1; if(cxb>255) cxb=255;
    for(int cx=cxa;cx<=cxb;cx++){
        if(cx<=p->min_sp||cx<=p->min_bg) continue;
        int k=cx-sx-1; uint8_t sc= c->shf[0] ? (uint8_t)(((lo>>k)&1)|(((hi>>k)&1)<<1)) : (uint8_t)((((lo<<k)&0x80)>>7)|(((hi<<k)&0x80)>>6));
        if(!sc) continue;
        int s=(cx-1)+p->fx, j=s>>3, b=7-(s&7);
        if(j<c->jlo||j>c->jhi) continue;
        if(((c->tlo[j]>>b)|(c->thi[j]>>b))&1) return cx;
    }
    return 0;
}
static void hit_scan(Nes*n,int L,int a,int c1){
    Ppu*p=P; FpCtx*c=&p->fc[p->fcur];
    if(p->sp0hit||!p->bg_en||!p->sp_en||c->y!=L) return;
    if(hit_find(p,c,a,c1)) p->sp0hit=1;
}

/* ---- whole lines, 5-plane build: background cache + tight loops ------------------------------------------------------------------- */
#ifdef PPU_FB5
#endif
#ifdef FP_FASTLINE
static inline void st32(uint8_t*d,uint32_t v){ *(u32u*)d=v; }
static uint32_t bgtab[4][256]; static int bgtab_ready;          /* (hi nibble, lo nibble) of a pattern row -> 4 palette RAM indices (palette number a: 4*a + 2 bit value, 0 for value 0) */
static void bgtab_init(void){
    for(int a=0;a<4;a++) for(int i=0;i<256;i++){
        uint8_t b[4]; int hi=i>>4, lo=i&15;
        for(int k=0;k<4;k++){ int bit=3-k; int raw=((lo>>bit)&1)|(((hi>>bit)&1)<<1); b[k]=(uint8_t)(raw? raw+4*a : 0); }
        st32((uint8_t*)&bgtab[a][i], 0); { uint8_t*d=(uint8_t*)&bgtab[a][i]; d[0]=b[0]; d[1]=b[1]; d[2]=b[2]; d[3]=b[3]; }
    }
    bgtab_ready=1;
}
typedef struct { uint16_t vl, fl; uint8_t c_lo, c_hi; uint8_t t01[6]; uint32_t nv[4]; } BgKey;   /* every input of the background pixels of a row (compared exactly) */
static BgKey bgkey[240]; static uint8_t bgok[240];
static uint8_t bgrow[240][272] __attribute__((aligned(4)));       /* background pixels (palette RAM indices) of each row as last built: 8 bytes margin, 256 pixels, 8 bytes margin */
static uint16_t rsk[240]; static uint32_t rsw[240][8];            /* sprites in the frame buffer row: count | min_sp<<8, packed slots */
static inline int bgkey_eq(const BgKey*a,const BgKey*b){ const u32a*x=(const u32a*)a,*y=(const u32a*)b; return !((x[0]^y[0])|(x[1]^y[1])|(x[2]^y[2])|(x[3]^y[3])|(x[4]^y[4])|(x[5]^y[5])|(x[6]^y[6])); }
static __attribute__((noinline)) void fp_blank_line(Nes*n,int L){
    Ppu*p=P; int idx= ((p->v&0x3F00)!=0x3F00) ? 0 : (p->v&0x1F);
    if(p->rowblank[L]==idx+1) return;
    bk_inval(L);
    uint32_t c=(uint32_t)idx*0x01010101u; u32a*r=(u32a*)&p->fb[L<<8]; for(int x=0;x<64;x++) r[x]=c;
    p->rowblank[L]=(uint8_t)(idx+1); p->rowok[L]=0; p->rowdirty[L]=1;
}
static void fp_line(Nes*n,int L);
/* ---- batches of whole lines: background validity per batch ------------------------------------------------------------------------
 * A run of whole lines (no access in between) is described by BatchKey (scroll at its start, parameters, name table write counter, extent).  When the key is the
 * one stored for the same run last time, all background rows are still in bgrow/the frame buffer: only rows that have sprites (now or before) are touched. */
static uint8_t ntbase_m=255; static uint16_t ntbase[4];          /* physical name table offset of logical name table 0..3 for the current mirroring */
static uint32_t bkey[240][4]; static uint8_t bkvalid[240]; static int16_t bowner[240];
static void bk_inval(int L){ int o=bowner[L]; if(o>=0 && o<240) bkvalid[o]=0; }
static int bk_init_done;
static void bk_init(void){ for(int i=0;i<240;i++) bowner[i]=-1; bk_init_done=1; }
static void step_line(Nes*n,int y){ Ppu*p=P; p->dots+=341; p->line=y; p->cx=340; p->fcur^=1; p->fc[p->fcur^1].y=-1; fp_line(n,y); }
/* packed sprites of output line y from the tables (sprite pattern bytes fetched with the current parameters); returns the number of visible slots */
static int sprite_pw(Nes*n,int y,uint32_t*pw){
    Ppu*p=P; int e=y-1, na=0; if(e<0) return 0;
    int cnt=p->lt_cnt[e]; if(cnt>8) cnt=8;
    for(int k=0;k<cnt;k++){
        int oi=p->lt_idx[e][k]; int sy=p->oam[oi*4], ti=p->oam[oi*4+1], at=p->oam[oi*4+2], sx=p->oam[oi*4+3];
        int vm=(at&0x80)!=0; int lo_= vm ? ((p->large?15:7)-(e-sy)) : (e-sy); uint16_t ta;
        if(p->large) ta=(uint16_t)((((ti&1)?0x1000:0)|((ti&~1)<<4))+(lo_>=8?lo_+8:lo_)); else ta=(uint16_t)(((ti<<4)|p->sppat)+lo_);
        uint8_t lo=vram_read(n,ta), hi=vram_read(n,ta+8); if(!(lo|hi)) continue;
        pw[na++]=(uint32_t)sx|((uint32_t)lo<<8)|((uint32_t)hi<<16)|((uint32_t)(at&3)<<24)|((uint32_t)((at>>5)&1)<<26)|((uint32_t)((at>>6)&1)<<27);
    }
    return na;
}
static void overlay_row(Ppu*p,int L,const uint32_t*pw,int na,uint16_t sk){
    uint8_t*fb=&p->fb[L<<8]; const u32a*s=(const u32a*)&bgrow[L][8]; u32a*d=(u32a*)fb; for(int i=0;i<64;i++) d[i]=s[i];
    if(na){
        uint32_t cl[8]={0,0,0,0,0,0,0,0}; int minsp=p->min_sp;
        for(int i=0;i<na;i++){
            uint32_t w=pw[i]; int sx=w&255; uint8_t lo=(uint8_t)(w>>8), hi=(uint8_t)(w>>16); uint8_t pal=(uint8_t)(((w>>24)&3)*4+0x10); int prio=(w>>26)&1, hf=(w>>27)&1;
            for(int kx=0;kx<8;kx++){
                int x=sx+kx; if(x>255) break; if(x<minsp) continue;
                int b= hf ? kx : 7-kx; int sc=((lo>>b)&1)|(((hi>>b)&1)<<1); if(!sc) continue;
                if(cl[x>>5]&(1u<<(x&31))) continue; cl[x>>5]|=1u<<(x&31);
                if(!(fb[x]&3) || !prio) fb[x]=(uint8_t)(pal+sc);
            }
        }
    }
    rsk[L]=sk; for(int i=0;i<na;i++) rsw[L][i]=pw[i];
    p->rowok[L]=1; p->rowdirty[L]=1; p->rowblank[L]=0;
}
/* lines a..b (whole, in 0..239, rendering on, no sprite-0 window inside): position is (a-1,340) */
static void batch_segment(Nes*n,int a,int b){
    Ppu*p=P; FpCtx*cc=&p->fc[p->fcur^1];     /* the context prepared for line a */
    if(!bk_init_done) bk_init();
    if(cc->y==a && cc->mode==2 && cc->t01ok==3){ step_line(n,a); if(++a>b) return; cc=&p->fc[p->fcur^1]; }   /* tiles 0,1 prefetched by a partial line: that line alone takes the per-line path */
    uint16_t vl=decx_n(p->v,2); int clipn= p->bg_en ? (p->min_bg>256?256:p->min_bg) : 256;
    uint32_t key[4];
    key[0]=(uint32_t)vl|((uint32_t)(p->t&0x041F)<<16);
    key[1]=(uint32_t)(p->bgpat?1:0)|((uint32_t)n->map.mirror<<1)|((uint32_t)p->fx<<3)|((uint32_t)clipn<<6)|((uint32_t)(n->map.chr_lo>>12)<<16)|((uint32_t)(n->map.chr_hi>>12)<<21);
    { /* sum of the write counters of every name table row (and attribute row) the lines read: it grows when any of them is written */
        if(ntbase_m!=n->map.mirror){ ntbase_m=n->map.mirror; for(int i=0;i<4;i++) ntbase[i]=nt_off(&n->map,(uint16_t)(0x2000+i*0x400)); }
        uint32_t sum=0; int m=b-a+1, i=0;
        while(i<m){
            uint16_t vv=incy_n(vl,i); int cy=(vv>>5)&31, nA=(vv>>10)&3;
            uint16_t r0=(uint16_t)(ntbase[nA]+cy*32), r1=(uint16_t)(ntbase[nA^1]+cy*32), a0=(uint16_t)(ntbase[nA]+0x3C0+(cy>>2)*8), a1=(uint16_t)(ntbase[nA^1]+0x3C0+(cy>>2)*8);
            sum+=p->ntv[r0>>5]+p->ntv[r1>>5]+p->ntv[a0>>5]+p->ntv[a1>>5];
            i+=8-((vv>>12)&7);
        }
        key[2]=sum; key[3]=(uint32_t)b|((uint32_t)m<<16);
    }
    int pre=(cc->t01ok==3 || cc->y!=a || cc->mode!=2);
    int valid=!pre && bkvalid[a] && key[0]==bkey[a][0] && key[1]==bkey[a][1] && key[2]==bkey[a][2] && key[3]==bkey[a][3];
    if(!valid){ if(pre){ ST(6); if(cc->y!=a){ ST(10); if(ppu_st[10]<8){ extern int fprintf(void*,const char*,...); extern void*stderr; fprintf(stderr,"pre: a=%d b=%d cc.y=%d other.y=%d line=%d frame=%u\n",a,b,cc->y,p->fc[p->fcur].y,p->line,p->frame);} } else if(cc->mode!=2) ST(11); else ST(12); } else if(!bkvalid[a]) ST(7); else ST(8); ST(9);
        for(int y=a;y<=b;y++) step_line(n,y);
        if(!pre){ for(int i=0;i<4;i++) bkey[a][i]=key[i]; bkvalid[a]=1; for(int y=a;y<=b;y++) bowner[y]=(int16_t)a; }
        return;
    }
    ST(5);
    lt_ensure(n); if(!p->lt_full) lt_build_full(n);
    int sprites_on = p->sp_en && p->min_sp<256;
    if(!p->spovf){ for(int y=a;y<=b;y++) if(p->lt_cnt[y]>=8){ int o=lt_ovf_cx(n,y); if(o){ p->spovf=1; break; } } }
    for(int y=a;y<=b;y++){
        int cnt= sprites_on ? p->lt_cnt[y-1] : 0;       /* output line y is drawn from evaluation line y-1 (y>=1; line 0 has no sprites) */
        if(y==0) cnt=0;
        if(!cnt && !(rsk[y]&0xFF)){ if(rsk[y]!=(uint16_t)((p->min_sp&0xFF)<<8) && !(rsk[y]&0xFF)) rsk[y]=(uint16_t)((p->min_sp&0xFF)<<8); continue; }
        uint32_t pw[8]; int na= cnt ? sprite_pw(n,y,pw) : 0;
        uint16_t sk=(uint16_t)(na|((p->min_sp&0xFF)<<8)); int same= (rsk[y]==sk && p->rowok[y]);
        if(same) for(int i=0;i<na;i++) if(rsw[y][i]!=pw[i]){ same=0; break; }
        if(!same) overlay_row(p,y,pw,na,sk);
    }
    int m=b-a+1;
    p->v=(uint16_t)((incy_n(p->v,m)&~0x041F)|(incx_n((uint16_t)(p->t&0x041F),2)&0x041F)); p->oamaddr=0;
    p->dots+=341ull*m; p->line=b; p->cx=340; p->fc[p->fcur].y=-1;
    if(b<239){ FpCtx*cn=&p->fc[p->fcur^1]; int yy=b+1, mm=ctx_mode(n,yy);
        if(mm){ ctx_init(n,cn,yy,mm); for(int kk=0;kk<cn->nspr;kk++) fetch_sprite(n,cn,kk); } else cn->y=-1; }
}
/* m whole lines after (L,340), all in 0..239, rendering on, pixels drawn */
static void fp_draw_batch(Nes*n,int m){
    Ppu*p=P; int a=p->line+1, end=a+m-1;
    int w0=240,w1=-1;
    if(!p->sp0hit && p->bg_en && p->sp_en){ int sy=p->oam[0], h=p->large?16:8; if(sy<240){ w0=sy; w1=sy+h; if(w1>239) w1=239; } }
    while(a<=end){
        if(a>=w0 && a<=w1){ step_line(n,a); a++; continue; }
        int b=end; if(a<w0 && w0-1<b) b=w0-1;
        batch_segment(n,a,b); a=b+1;
        /* a hit inside the window can end it early */
        if(p->sp0hit){ w0=240; w1=-1; }
    }
}

/* whole line L (0..239), dots 1..340, rendering on, pixels drawn.  The context of the line (sprites; tiles 0 and 1 unless the generic path fetched them) was
 * prepared by the line before.  The background row is rebuilt only when one of its inputs changed (BgKey, compared exactly). */
static void fp_line(Nes*n,int L){
    Ppu*p=P; FpCtx*cc=&p->fc[p->fcur], *cn=&p->fc[p->fcur^1];
    if(!bgtab_ready) bgtab_init();
    uint16_t v0=p->v;                                   /* v after dot 340 of the line before */
    uint16_t vl=decx_n(v0,2);                           /* tile j of this line is fetched with vl+j */
    uint16_t vc=(uint16_t)((inc_y16(v0)&~0x041F)|(p->t&0x041F));   /* v after the horizontal copy at dot 257: tile j of the next line uses vc+j */
    if(cc->y!=L || cc->mode!=2){ ppu_exotic++; ctx_build(n,cc,L,2,vl); }
    ST(0);
    if(!p->spovf){ lt_ensure(n); if(p->lt_cnt[L]>=8){ int o=lt_ovf_cx(n,L); if(o) p->spovf=1; } }
    /* ---- background row key */
    int fx=p->fx, clipn= p->bg_en ? (p->min_bg>256?256:p->min_bg) : 256;
    int cy=(vl>>5)&31, x0=vl&31, fy=(vl>>12)&7;
    if(ntbase_m!=n->map.mirror){ ntbase_m=n->map.mirror; for(int i=0;i<4;i++) ntbase[i]=nt_off(&n->map,(uint16_t)(0x2000+i*0x400)); }
    int ntA=(vl>>10)&3, ntB=ntA^1;
    uint16_t rA=(uint16_t)(ntbase[ntA]+cy*32), rB=(uint16_t)(ntbase[ntB]+cy*32), aA=(uint16_t)(ntbase[ntA]+0x3C0+(cy>>2)*8), aB=(uint16_t)(ntbase[ntB]+0x3C0+(cy>>2)*8);
    BgKey k; k.vl=vl; k.fl=(uint16_t)((p->bgpat?1:0)|(n->map.mirror<<1)|(fx<<3)|(clipn<<6)); k.c_lo=(uint8_t)(n->map.chr_lo>>12); k.c_hi=(uint8_t)(n->map.chr_hi>>12);
    k.t01[0]=k.t01[1]=k.t01[2]=k.t01[3]=k.t01[4]=k.t01[5]=0;
    k.nv[0]=p->ntv[rA>>5]; k.nv[1]=p->ntv[rB>>5]; k.nv[2]=p->ntv[aA>>5]; k.nv[3]=p->ntv[aB>>5];
    int pre = (cc->t01ok==3);                            /* tiles 0,1 were fetched earlier under possibly different parameters: they are part of the key */
    if(pre){ k.t01[0]=cc->tlo[0]; k.t01[1]=cc->thi[0]; k.t01[2]=cc->tpal[0]; k.t01[3]=cc->tlo[1]; k.t01[4]=cc->thi[1]; k.t01[5]=cc->tpal[1]; k.vl|=0; k.fl|=0x8000; }
    int bgsame = bgok[L] && bgkey_eq(&k,&bgkey[L]);
#ifdef FPDBG
    if(L==FPDBG_L && p->frame>=FPDBG_F0 && p->frame<=FPDBG_F1){ extern int fprintf(void*,const char*,...); extern void*stderr; fprintf(stderr,"fp_line L=%d frame %u: vl=%04x fl=%04x pre=%d bgsame=%d (ok %d) fx=%d clipn=%d v0=%04x t=%04x nv=%u,%u,%u,%u t01ok=%d cc.y=%d mode %d\n",L,p->frame,vl,k.fl,pre,bgsame,bgok[L],fx,clipn,v0,p->t,k.nv[0],k.nv[1],k.nv[2],k.nv[3],cc->t01ok,cc->y,cc->mode); }
#endif
    /* sprite 0 hit: the background tiles under sprite 0 */
    if(!p->sp0hit && p->bg_en && p->sp_en && cc->s0){
        int sx=cc->spx[0], j0=sx>>3, j1=(sx+14)>>3; if(j1>32) j1=32;
        for(int j=j0;j<=j1;j++){ if(j<2 && pre) continue; fetch_tile(n,cc,j,0,incx_n(vl,j),0,8); }
        if(hit_find(p,cc,1,256)) p->sp0hit=1;
    }
    uint8_t*row=&bgrow[L][8];
    if(!bgsame){ ST(1);
        const uint8_t*chrb=n->map.chr+(p->bgpat? n->map.chr_hi : n->map.chr_lo);
        const uint8_t*ntpA=p->nt+rA, *ntpB=p->nt+rB, *atA=p->nt+aA, *atB=p->nt+aB; int ysh=(cy&2)<<1;
        uint8_t*dst=row-fx;
        if(!pre){ for(int j=0;j<2;j++) fetch_tile(n,cc,j,0,incx_n(vl,j),0,8); }
        for(int j=0;j<2;j++){ uint8_t lo=cc->tlo[j], hi=cc->thi[j]; const uint32_t*tb=bgtab[cc->tpal[j]>>2]; st32(dst+8*j,tb[(hi&0xF0)|(lo>>4)]); st32(dst+8*j+4,tb[((hi&0x0F)<<4)|(lo&0x0F)]); }
        int j=2, xx=x0+2;
        for(;xx<32 && j<=32;j++,xx++){
            int ti=ntpA[xx]; int a=(atA[xx>>2]>>((xx&2)|ysh))&3;
            const uint8_t*cp=chrb+((ti<<4)|fy); uint8_t lo=cp[0], hi=cp[8]; const uint32_t*tb=bgtab[a];
            st32(dst+8*j,tb[(hi&0xF0)|(lo>>4)]); st32(dst+8*j+4,tb[((hi&0x0F)<<4)|(lo&0x0F)]);
        }
        for(xx-=32;j<=32;j++,xx++){
            int ti=ntpB[xx]; int a=(atB[xx>>2]>>((xx&2)|ysh))&3;
            const uint8_t*cp=chrb+((ti<<4)|fy); uint8_t lo=cp[0], hi=cp[8]; const uint32_t*tb=bgtab[a];
            st32(dst+8*j,tb[(hi&0xF0)|(lo>>4)]); st32(dst+8*j+4,tb[((hi&0x0F)<<4)|(lo&0x0F)]);
        }
        { u32a*r=(u32a*)row; int q=clipn>>2; for(int i=0;i<q;i++) r[i]=0; }     /* clipped pixels (left 8 or all): backdrop */
        bgkey[L]=k; bgok[L]=1; bk_inval(L);
    }
    /* ---- sprites of the row */
    uint32_t pw[8]; int na=0;
    if(p->sp_en && p->min_sp<256){
        for(int i=0;i<cc->nspr;i++){ if(!(cc->slo[i]|cc->shi[i])) continue;
            pw[na++]=(uint32_t)cc->spx[i]|((uint32_t)cc->slo[i]<<8)|((uint32_t)cc->shi[i]<<16)|((uint32_t)((cc->spal[i]>>2)&3)<<24)|((uint32_t)cc->sprio[i]<<26)|((uint32_t)cc->shf[i]<<27); }
    }
    uint16_t sk=(uint16_t)(na|((p->min_sp&0xFF)<<8));
    int same=0;
    if(bgsame && p->rowok[L] && rsk[L]==sk){ same=1; for(int i=0;i<na;i++) if(rsw[L][i]!=pw[i]){ same=0; break; } }
    if(!same){ ST(2); if(na) ST(3);
        uint8_t*fb=&p->fb[L<<8];
        { const u32a*s=(const u32a*)row; u32a*d=(u32a*)fb; for(int i=0;i<64;i++) d[i]=s[i]; }
        if(na){
            uint32_t cl[8]={0,0,0,0,0,0,0,0}; int minsp=p->min_sp;
            for(int i=0;i<na;i++){
                uint32_t w=pw[i]; int sx=w&255; uint8_t lo=(uint8_t)(w>>8), hi=(uint8_t)(w>>16); uint8_t pal=(uint8_t)(((w>>24)&3)*4+0x10); int prio=(w>>26)&1, hf=(w>>27)&1;
                for(int kx=0;kx<8;kx++){
                    int x=sx+kx; if(x>255) break; if(x<minsp) continue;
                    int b= hf ? kx : 7-kx; int sc=((lo>>b)&1)|(((hi>>b)&1)<<1); if(!sc) continue;
                    if(cl[x>>5]&(1u<<(x&31))) continue; cl[x>>5]|=1u<<(x&31);
                    if(!(fb[x]&3) || !prio) fb[x]=(uint8_t)(pal+sc);
                }
            }
        }
        rsk[L]=sk; for(int i=0;i<na;i++) rsw[L][i]=pw[i];
        p->rowok[L]=1; p->rowdirty[L]=1; p->rowblank[L]=0;
    }
    /* ---- the line after: sprites (tiles 0 and 1 are fetched when needed); state */
    if(L<239){
        int y=L+1, m=ctx_mode(n,y);
        if(m){ ST(4); ctx_init(n,cn,y,m); for(int kk=0;kk<cn->nspr;kk++) fetch_sprite(n,cn,kk); if(m==1) for(int j=cn->jlo;j<=1&&j<=cn->jhi;j++) fetch_tile(n,cn,j,0,incx_n(vc,j),0,8); }
        else cn->y=-1;
    }
    p->v=incx_n(vc,2); p->oamaddr=0;
}
#endif

/* ---- one piece: the dots a..c1 (1 <= a <= c1 <= 340) of line L; parameters constant, no pending update ---------------------------------- */
static __attribute__((noinline)) void fp_piece(Nes*n,int L,int a,int c1){
    Ppu*p=P; int rend=p->rend;
    if(L>=240){
        if(L==241 && a<=1 && c1>=1){ if(!p->prevent_vbl){ p->vbl=1; if(p->ctrl&0x80) p->nmi_level=1; } p->prevent_vbl=0; }
        return;
    }
    if(L<0){
        if(a<=1 && c1>=1){ p->vbl=0; p->nmi_level=0; }
        if(rend && p->oamaddr>=8 && a<=8){ for(int cx=a;cx<=c1&&cx<=8;cx++){ p->oam[cx-1]=p->oam[(p->oamaddr&0xF8)+cx-1]; p->oam_dirty=1; } }
    } else if(rend && !p->spovf){
        lt_ensure(n);
        if(p->lt_cnt[L]>=8){ int o=lt_ovf_cx(n,L); if(o && a<=o && o<=c1) p->spovf=1; }
    }
    /* fetch contexts */
    if(rend){
        FpCtx*cc=&p->fc[p->fcur], *cn=&p->fc[p->fcur^1];
        if(L<239 && c1>=261){                       /* sprites and tiles 0,1 of the next line */
            int y=L+1;
            if(cn->y!=y){ int m=ctx_mode(n,y); if(m) ctx_init(n,cn,y,m); else cn->y=-1; }
            if(cn->y==y){
                for(int k=0;k<cn->nspr;k++){ int d=261+8*k; if(d>=a && d<=c1) fetch_sprite(n,cn,k); }
                for(int j=cn->jlo;j<=1 && j<=cn->jhi;j++){ int b=321+8*j; if(b+6>=a && b<=c1){ uint16_t v=v_adv(p,p->v,L,a-1,b>a?b:a); fetch_tile(n,cn,j,b,v,a,c1); if(b+6<=c1) cn->t01ok|=(uint8_t)(1<<j); } }
            }
        }
        if(L>=0 && a<=247 && cc->y==L){            /* tiles 2..32 of this line */
            for(int j=cc->jlo>2?cc->jlo:2;j<=cc->jhi && j<=32;j++){
                int b=8*(j-2)+1; if(b>c1) break; if(b+6<a) continue;
                uint16_t v=v_adv(p,p->v,L,a-1,b>a?b:a); fetch_tile(n,cc,j,b,v,a,c1);
            }
        }
    }
    if(L>=0 && a<=256){
        int e=c1<256?c1:256;
        if(rend){
            if(p->draw || (!p->sp0hit && p->bg_en && p->sp_en && s0_in_range(p,L-1))) ctx_ensure(n,L,a);
            { FpCtx*cc=&p->fc[p->fcur];     /* the whole-line path of the line before leaves tiles 0 and 1 to be fetched now (parameters unchanged since) */
              if(cc->y==L && cc->mode==2 && cc->t01ok!=3){ uint16_t vl=(a==1)?decx_n(p->v,2):vline_live(p,a-1); fetch_tile(n,cc,0,0,vl,0,8); fetch_tile(n,cc,1,0,incx_n(vl,1),0,8); cc->t01ok=3; } }
            hit_scan(n,L,a,e);
        }
        if(p->draw) draw_range(n,L,a-1,e);
    }
    p->v=v_adv(p,p->v,L,a-1,c1);
    if(rend && c1>=257 && a<=320) p->oamaddr=0;
}

static void fp_first_cycle(Nes*n){
    Ppu*p=P; p->dots++; p->cx=0;
    if(++p->line>260){ p->line=-1; p->sp_count=0; }
    if(p->line==241 && !ppu_noinput) nes_input_poll();
    if(p->line<240){ if(p->line==-1){ p->spovf=0; p->sp0hit=0; } }
    else if(p->line==240){ p->frame++; p->frame_done=1; }
    if(p->line>=0 && p->line<240){ p->fcur^=1; p->fc[p->fcur^1].y=-1; }
    else { p->fc[0].y=p->fc[1].y=-1; p->fcur=0; }
    if(p->need_update) update_state(n);
}
/* Advance to `target` dots, or until an event the board has to look at (frame completed, NMI line changed). */
void ppu_fp_run(Nes*n,uint64_t target){
    Ppu*p=P; uint8_t nmi0=p->nmi_level;
    while(p->dots<target){
        int L=p->line, c=p->cx; uint64_t room=target-p->dots;
        int trans = p->need_update || p->upd_delay || p->rend!=p->rend_prev;
        if(c==340){
            if(!trans && room>=341){
                int m=0;
                if(L>=241 && L<=259){ m=260-L; if((uint64_t)m>room/341) m=(int)(room/341); if(m>0){ p->dots+=341ull*m; p->line+=m; return; } }
#ifdef FP_FASTLINE
                else if(L>=-1 && L<=238 && p->draw && !FP_GENERIC){
                    int mm=239-L; if((uint64_t)mm>room/341) mm=(int)(room/341);
                    if(p->rend && mm>0) fp_draw_batch(n,mm);
                    else for(int i=0;i<mm;i++){
                        p->dots+=341; p->line++; p->cx=340;
                        p->fcur^=1; p->fc[p->fcur^1].y=-1;
                        p->fc[0].y=p->fc[1].y=-1; fp_blank_line(n,p->line);
                    }
                    continue;
                }
#endif
                else if(L>=-1 && L<=238 && !p->draw){
                    int mm=239-L; if((uint64_t)mm>room/341) mm=(int)(room/341);
                    /* lines that have work: sprite-0 window, overflow lines */
                    if(p->rend){
                        lt_ensure(n);
                        int lim=L+1+mm;                       /* lines L+1 .. lim-1 may be skipped */
                        if(!p->sp0hit){ int sy=p->oam[0], h=p->large?16:8; if(sy<240){ int w0=sy, w1=sy+h; if(w1>239) w1=239;   /* lines with work: sy .. sy+h */
                            if(L+1<=w1 && w0<lim){ lim = w0>L+1 ? w0 : L+1; } } }
                        if(!p->spovf){ for(int l=L+1;l<lim;l++) if(p->lt_cnt[l]>=8){ lim=l; break; } }
                        m=lim-(L+1); if(m<0) m=0;
                    } else m=mm;
                    if(m>0){
                        if(p->rend){ p->v=(uint16_t)((incy_n(p->v,m)&~0x041F)|(incx_n((uint16_t)(p->t&0x041F),2)&0x041F)); p->oamaddr=0; }
                        p->dots+=341ull*m; p->line+=m; p->fc[0].y=p->fc[1].y=-1;
                        continue;
                    }
                }
            }
            fp_first_cycle(n);
            if(p->frame_done || p->nmi_level!=nmi0) return;
            continue;
        }
        int c1 = room>=(uint64_t)(340-c) ? 340 : c+(int)room;
        if(trans) c1=c+1;
        if(L==-1 && c<1 && c1>1) c1=1;
        if(L==241 && c<1 && c1>1) c1=1;
        if(L==-1 && c<339 && c1>339) c1=339;
        fp_piece(n,L,c+1,c1);
        int skip = (L==-1 && c1==339 && p->rend && (p->frame&1));
        p->dots+=(uint64_t)(c1-c); p->cx=skip?340:c1;
        if(p->need_update) update_state(n);
        if(p->nmi_level!=nmi0) return;
    }
}

/* predicts the sprite-0 hit of the current frame assuming no register changes: returns the number of dots executed once the hit dot has run, or 0 */
uint64_t ppu_predict_hit(Nes*n){
    Ppu*p=P; if(!p->rend||p->rend!=p->rend_prev||!p->bg_en||!p->sp_en||p->sp0hit||p->need_update||p->upd_delay) return 0;
    int L=p->line, cx=p->cx; FpCtx t; uint16_t vl; int y;
    int64_t cur=(int64_t)(L+1)*341+cx; if(L==-1 && cx<339 && (p->frame&1)) cur++;
    if(L>=0 && L<=239 && cx<256 && s0_in_range(p,L-1)){
        FpCtx*cc=&p->fc[p->fcur];
        ctx_build(n,&t,L,1,vline_live(p,cx));
        if(cc->y==L){
            for(int j=t.jlo<2?2:t.jlo;j<=t.jhi;j++) if(8*(j-2)+7<=cx){ t.tlo[j]=cc->tlo[j]; t.thi[j]=cc->thi[j]; }
            if(cc->s0){ t.slo[0]=cc->slo[0]; t.shi[0]=cc->shi[0]; t.spx[0]=cc->spx[0]; t.shf[0]=cc->shf[0]; }
        }
        int h=hit_find(p,&t,cx+1,256);
        if(h) return p->dots+(uint64_t)(((int64_t)(L+1)*341+h)-cur);
    }
    if(L==-1){ uint16_t vert=(cx<=304)?(p->t&0x7BE0):(p->v&0x7BE0); vl=(uint16_t)(vert|(p->t&0x41F)); y=0; }
    else { uint16_t v=p->v; if(cx<256) v=inc_y16(v); vl=(uint16_t)((v&0x7BE0)|(p->t&0x41F)); y=L+1; }
    for(;y<=239;y++){
        if(s0_in_range(p,y-1)){
            ctx_build(n,&t,y,1,vl);
            int h=hit_find(p,&t,1,256);
            if(h) return p->dots+(uint64_t)(((int64_t)(y+1)*341+h)-cur);
        }
        vl=(uint16_t)((inc_y16(vl)&~0x41F)|(p->t&0x41F));
    }
    return 0;
}
void ppu_sync(Nes *n, uint64_t dots){ while(n->ppu.dots<dots) ppu_step(n); }
