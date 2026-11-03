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


/* ======================================================================================================================================
 * Fast path: whole visible lines without CPU activity are rendered by render_line_fast() instead of 341 calls of ppu_step().  The result
 * (framebuffer row, v, flags and the complete pipeline state at the end of the line) is identical to the per-dot model.
 * ====================================================================================================================================== */
static void dec_x(Ppu*p){ uint16_t a=p->v; if((a&0x1F)==0) a=(a|0x1F)^0x400; else a--; p->v=a; }
static inline uint16_t inc_y16(uint16_t a){
    if((a&0x7000)!=0x7000) return (uint16_t)(a+0x1000);
    a&=~0x7000; int y=(a&0x3E0)>>5; if(y==29){ y=0; a^=0x800; } else if(y==31) y=0; else y++; return (uint16_t)((a&~0x3E0)|(y<<5));
}
/* sprites visible on line y (evaluated on line y-1): fills sp[], returns the count; s0 = sprite 0 among them; ovf = more than 8 in range */
typedef struct { uint8_t lo,hi,x,paloff,bgprio,hflip; } FSpr;
static int eval_sprites_fast(Nes*n,int y,FSpr*sp,int*s0,int*ovf){
    Ppu*p=P; int cnt=0; *s0=0; *ovf=0; int h=p->large?16:8; int L=y-1;
    if(L<0) return 0;
    for(int i=0;i<64;i++){
        int sy=p->oam[i*4]; if(!(L>=sy && L<sy+h)) continue;
        if(cnt==8){ *ovf=1; break; }
        uint8_t ti=p->oam[i*4+1], at=p->oam[i*4+2], sx=p->oam[i*4+3];
        if(i==0) *s0=1;
        if(sy<240){
            int vm=(at&0x80)!=0; int lo_= vm ? ((p->large?15:7)-(L-sy)) : (L-sy); uint16_t ta;
            if(p->large) ta=(uint16_t)((((ti&1)?0x1000:0)|((ti&~1)<<4))+(lo_>=8?lo_+8:lo_)); else ta=(uint16_t)(((ti<<4)|p->sppat)+lo_);
            sp[cnt].lo=vram_read(n,ta); sp[cnt].hi=vram_read(n,ta+8); sp[cnt].x=sx; sp[cnt].paloff=(uint8_t)(((at&3)<<2)|0x10); sp[cnt].bgprio=(at&0x20)!=0; sp[cnt].hflip=(at&0x40)!=0;
        } else { sp[cnt].lo=sp[cnt].hi=0; sp[cnt].x=sx; sp[cnt].paloff=0; sp[cnt].bgprio=0; sp[cnt].hflip=0; sp[cnt].x=sx; }
        cnt++;
    }
    return cnt;
}
long ppu_fast_lines;
static void render_line_fast(Nes*n,int y,uint16_t vline){
    Ppu*p=P; ppu_fast_lines++; uint16_t*row=&p->fb[y<<8];
    uint16_t emph=(uint16_t)(p->emph<<6); uint8_t gm=p->gray?0x30:0x3F;
    if(!p->rend){
        int idx= ((p->v&0x3F00)!=0x3F00) ? 0 : (p->v&0x1F);
        uint16_t c=(uint16_t)((p->pal[idx]&gm)|emph); for(int x=0;x<256;x++) row[x]=c; return;
    }
    /* background tiles of this line: tile j (0..32) uses the nametable column (coarse X0 + j) */
    uint8_t tlo[34],thi[34],tpal[34];
    uint16_t va=vline;
    for(int j=0;j<33;j++){
        uint8_t ti=vram_read(n,0x2000|(va&0x0FFF));
        uint8_t sh=((va>>4)&4)|(va&2);
        tpal[j]=(uint8_t)(((vram_read(n,0x23C0|(va&0x0C00)|((va>>4)&0x38)|((va>>2)&7))>>sh)&3)<<2);
        uint16_t ta=(uint16_t)((ti<<4)|(va>>12)|p->bgpat);
        tlo[j]=vram_read(n,ta); thi[j]=vram_read(n,ta+8);
        if((va&0x1F)==31) va=(uint16_t)((va&~0x1F)^0x400); else va++;
    }
    FSpr sp[8]; int s0,ovf; int cnt=eval_sprites_fast(n,y,sp,&s0,&ovf);
    if(ovf){ /* more than 8 sprites in range: the overflow flag follows the hardware's buggy evaluation, which only the per-dot model reproduces */
        int sl=p->line,sc=p->cx; p->line=y-1; p->oamaddr=0; for(int cx=1;cx<=256;cx++){ p->cx=cx; sprite_eval(n); } p->line=sl; p->cx=sc; p->oamaddr=0; }
    int fx=p->fx;
    for(int x=0;x<256;x++){
        int cx=x+1; uint8_t bgc=0, spbg=0; int j=(x+fx)>>3, b=7-((x+fx)&7);
        if(cx>p->min_bg){ spbg=(uint8_t)(((tlo[j]>>b)&1)|(((thi[j]>>b)&1)<<1)); bgc=spbg; }
        uint8_t col=(uint8_t)(tpal[j]+bgc);
        if(cnt && cx>p->min_sp){
            for(int i=0;i<cnt;i++){
                int sh=cx-sp[i].x-1;
                if(sh>=0&&sh<8){
                    uint8_t sc= sp[i].hflip ? (uint8_t)(((sp[i].lo>>sh)&1)|(((sp[i].hi>>sh)&1)<<1)) : (uint8_t)((((sp[i].lo<<sh)&0x80)>>7)|(((sp[i].hi<<sh)&0x80)>>6));
                    if(sc){
                        if(i==0 && spbg && s0 && cx!=256 && p->bg_en && !p->sp0hit && cx>p->min_sp_std) p->sp0hit=1;
                        if(p->sp_en && (bgc==0 || !sp[i].bgprio)) col=(uint8_t)(sp[i].paloff+sc);
                        break;
                    }
                }
            }
        }
        row[x]=(uint16_t)((p->pal[(col&3)?col:0]&gm)|emph);
    }
}
/* make the pipeline state identical to the per-dot model after dot 340 of line ycomplete (rendering on); vline = v at output start of that line */
static void reconstruct(Nes*n,int ycomplete,uint16_t vline){
    Ppu*p=P;
    p->line=ycomplete; p->v=inc_y16(vline); p->oamaddr=0;
    for(int cx=1;cx<=256;cx++){ p->cx=cx; sprite_eval(n); }
    for(int cx=257;cx<=340;cx++){ p->cx=cx; process_scanline(n); }
    p->cx=340;
}
/* run the model until `dots` dots have been executed, using the fast path for whole visible lines. */
void ppu_sync_fast(Nes *n, uint64_t dots){
    Ppu*p=P;
    while(p->dots<dots){
        if(p->cx==340 && p->line>=-1 && p->line<=238 && p->rend==p->rend_prev && !p->need_update && !p->upd_delay){
            int m=(int)((dots-p->dots)/341); if(m>239-p->line) m=239-p->line;
            if(m>=1){
                int y0=p->line+1; uint16_t vl=p->v, vlast;
                if(p->rend){ dec_x(p); dec_x(p); vl=p->v; }          /* v at the output start of line y0 */
                for(int i=0;i<m;i++){
                    vlast=vl; render_line_fast(n,y0+i,vl);
                    if(p->rend) vl=(uint16_t)((inc_y16(vl)&~0x41F)|(p->t&0x41F));
                }
                p->dots+=341ull*m;
                if(p->rend) reconstruct(n,y0+m-1,vlast); else { p->line=y0+m-1; p->cx=340; }
                continue;
            }
        }
        ppu_step(n);
    }
}
/* ---- prediction used to skip polling loops ---------------------------------------------------------------------------------------- */
/* first column (cx 1..256) at which sprite 0 hits the background on line y, or 0; vline = v at the output start of the line */
static int line_hit_cx(Nes*n,int y,uint16_t vline){
    Ppu*p=P; int L=y-1; if(L<0||!p->rend||!p->bg_en) return 0;
    int h=p->large?16:8, sy=p->oam[0]; if(!(L>=sy && L<sy+h) || sy>=240) return 0;
    uint8_t ti=p->oam[1], at=p->oam[2], sx=p->oam[3];
    int vm=(at&0x80)!=0, hm=(at&0x40)!=0; int lo_= vm ? ((p->large?15:7)-(L-sy)) : (L-sy); uint16_t ta;
    if(p->large) ta=(uint16_t)((((ti&1)?0x1000:0)|((ti&~1)<<4))+(lo_>=8?lo_+8:lo_)); else ta=(uint16_t)(((ti<<4)|p->sppat)+lo_);
    uint8_t slo=vram_read(n,ta), shi=vram_read(n,ta+8);
    for(int k=0;k<8;k++){
        int cx=sx+1+k; if(cx>256) break;
        int sh=k; uint8_t sc= hm ? (uint8_t)(((slo>>sh)&1)|(((shi>>sh)&1)<<1)) : (uint8_t)((((slo<<sh)&0x80)>>7)|(((shi<<sh)&0x80)>>6));
        if(!sc || cx==256 || !(cx>p->min_sp) || !(cx>p->min_sp_std) ) continue;
        if(!(cx>p->min_bg)) continue;
        int x=cx-1, j=(x+p->fx)>>3, b=7-((x+p->fx)&7);
        uint16_t va=vline; for(int q=0;q<j;q++){ if((va&0x1F)==31) va=(uint16_t)((va&~0x1F)^0x400); else va++; }
        uint8_t nt=vram_read(n,0x2000|(va&0x0FFF)); uint16_t pa=(uint16_t)((nt<<4)|(va>>12)|p->bgpat);
        uint8_t blo=vram_read(n,pa), bhi=vram_read(n,pa+8);
        if(((blo>>b)&1)|((bhi>>b)&1)) return cx;
    }
    return 0;
}
/* predicts the sprite-0 hit of the current frame, assuming nothing changes: returns the number of dots executed once the hit dot has run, or 0 */
uint64_t ppu_predict_hit(Nes*n){
    Ppu*p=P; if(!p->rend||p->rend!=p->rend_prev||!p->bg_en||!p->sp_en||p->sp0hit) return 0;
    int64_t cur=(int64_t)(p->line+1)*341+p->cx;
    int yn=p->line+1; uint16_t vl;
    if(p->line==-1){ uint16_t vert=(p->cx<=304)?(p->t&0x7BE0):(p->v&0x7BE0); vl=(uint16_t)(vert|(p->t&0x41F)); }
    else { uint16_t v=p->v; if(p->cx<256) v=inc_y16(v); vl=(uint16_t)((v&0x7BE0)|(p->t&0x41F)); }
    for(int y=yn;y<=239;y++){
        int cx=line_hit_cx(n,y,vl);
        if(cx) return p->dots+(uint64_t)(((int64_t)(y+1)*341+cx)-cur);
        vl=(uint16_t)((inc_y16(vl)&~0x41F)|(p->t&0x41F));
    }
    return 0;
}
void ppu_sync(Nes *n, uint64_t dots){ while(n->ppu.dots<dots) ppu_step(n); }
