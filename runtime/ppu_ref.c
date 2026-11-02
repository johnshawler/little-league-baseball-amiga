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
static void vram_write(Nes *n, uint16_t a, uint8_t v){
    a&=0x3FFF;
    if(a<0x2000) return;
    if(a<0x3F00){ P->nt[nt_off(&n->map,a)]=v; return; }
    uint8_t i=a&0x1F; v&=0x3F;
    if((i&3)==0){ P->pal[i&0x0F]=v; P->pal[(i&0x0F)|0x10]=v; } else P->pal[i]=v;
}

void ppu_reset(Ppu *p){ memset(p,0,sizeof *p); p->line=-1; p->cx=340; p->frame=1; p->min_bg=300; p->min_sp=300; p->min_sp_std=300; }

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

static void draw_pixel(Nes *n){
    Ppu*p=P; uint8_t c; int idx;
    if(p->rend || (p->v&0x3F00)!=0x3F00){ c=pixel_color(p); idx=(c&3)?c:0; }
    else { idx=p->v&0x1F; (void)pixel_color(p); }
    uint8_t col=p->pal[idx];
    p->fb[(p->line<<8)+p->cx-1]=(uint16_t)((col&(p->gray?0x30:0x3F))|(p->emph<<6));
}

static void process_scanline(Nes *n){
    Ppu*p=P; int cx=p->cx;
    if(cx<=256){
        load_tile_info(n);
        if(p->rend_prev && (cx&7)==0){ inc_x(p); if(cx==256) inc_y(p); }
        if(p->line>=0){ draw_pixel(n); p->sh_lo<<=1; p->sh_hi<<=1; sprite_eval(n); }
        else if(cx<9){
            if(cx==1){ p->vbl=0; p->nmi_level=0; }
            if(p->oamaddr>=8 && p->rend) p->oam[cx-1]=p->oam[(p->oamaddr&0xF8)+cx-1];
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
static void first_cycle(Nes *n){
    Ppu*p=P; p->cx=0;
    if(++p->line>260){ p->line=-1; p->sp_count=0; }
    if(p->line==241) nes_input_poll();
    if(p->line<240){
        if(p->line==-1){ p->spovf=0; p->sp0hit=0; }
    } else if(p->line==240){ p->frame++; p->frame_done=1; }
}

static void update_state(Nes *n){
    Ppu*p=P; p->need_update=0;
    if(p->rend_prev!=p->rend){ p->rend_prev=p->rend; }
    int nr=(p->bg_en|p->sp_en); if(p->rend!=nr){ p->rend=nr; p->need_update=1; }
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
    Ppu*p=P; p->mask=v; p->gray=v&1; p->bg_en=(v&8)!=0; p->sp_en=(v&0x10)!=0; p->emph=(v>>5)&7;
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
    case 4: r=p->oam[p->oamaddr]; break;
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
        if(p->line>=240 || !p->rend){ uint8_t b=v; if((p->oamaddr&3)==2) b&=0xE3; p->oam[p->oamaddr]=b; p->oamaddr++; }
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

/* lazy synchronisation used by the runtime: run the per-dot model until `dots` dots have been executed.  Returns via nes.ppu.dots. */
void ppu_sync(Nes *n, uint64_t dots){ while(n->ppu.dots<dots) ppu_step(n); }
