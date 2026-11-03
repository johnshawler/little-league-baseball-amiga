// apu_blipcmp: feeds channel level-change events through Mesen's own mixer arithmetic (SoundMixer::EndFrame / GetOutputVolume) and blip_buf and compares the
// resulting 48 kHz PCM against Mesen's recorded PCM sample by sample.
//   build: g++ -O2 -I$MESEN/Core/../Utilities -o apu_blipcmp tools/apu_blipcmp.cpp $MESEN/Utilities/blip_buf.cpp
//   usage: apu_blipcmp events.ev mesen_blocks.ev mesen.pcm
//     events.ev       8-byte records {u32 bus cycle, u8 ch, u8 lvl, u16}: either Mesen's own log (tools/discovery/mesen-apu.patch) or ours (apu_verify)
//     mesen_blocks.ev Mesen's log; only its block-end markers (ch 255) are used.  Mesen calls EndFrame() every 9999 APU cycles AND at the end of each video frame,
//                     and every EndFrame ends one blip frame; the markers tell us where.
//     mesen.pcm       int16 stereo 48 kHz as recorded from the libretro core (MasterVolume is 10.0 there).
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <algorithm>
#include "blip_buf.h"
struct R { uint32_t cc; uint8_t ch,lvl; uint16_t stamp; };
static uint16_t sqv(int s){ return (uint16_t)((95.88*5000.0)/(8128.0/s+100.0)); }
static uint16_t tndv(int t,int n){ double o=1.0966713261*0+2.7516713261*t+1.8493587125*n; return (uint16_t)((159.79*5000.0)/(22638.0/o+100.0)); }
static std::vector<R> load(const char*fn){ std::vector<R> v; FILE*f=fopen(fn,"rb"); R r; while(fread(&r,8,1,f)==1) v.push_back(r); fclose(f); return v; }
int main(int c,char**v){
    std::vector<R> ev0=load(v[1]), ev, blk=load(v[2]); for(auto&r:ev0) if(r.ch<4) ev.push_back(r);   // channel events only
    std::vector<int16_t> ref; { FILE*p=fopen(v[3],"rb"); int16_t b[2]; long lr=0; while(fread(b,2,2,p)==2){ ref.push_back(b[0]); lr+=b[0]!=b[1]; } printf("mesen pcm: %zu stereo frames, L!=R in %ld\n",ref.size(),lr); }
    std::vector<R> ends; for(auto&r:blk) if(r.ch==255) ends.push_back(r);
    std::stable_sort(ev.begin(),ev.end(),[](const R&a,const R&b){return a.cc<b.cc;});
    blip_t* bl=blip_new(96000); blip_set_rates(bl,1789773,48000); blip_clear(bl);   // Mesen: SoundMixer::Reset() clears after UpdateRates() has set the rates
    int lv[4]={0,0,0,0}; int prev=0; std::vector<int16_t> out; static int16_t tmp[100000]; size_t i=0; int64_t base=-1;   // bus cycle of the cycle before the block's first one
    for(auto&e:ends){
        while(i<ev.size() && ev[i].cc<=e.cc){
            uint32_t cc=ev[i].cc;
            while(i<ev.size() && ev[i].cc==cc){ lv[ev[i].ch]=ev[i].lvl; i++; }
            int cur=sqv(lv[0]+lv[1])+tndv(lv[2],lv[3]);
            blip_add_delta(bl,(uint32_t)(cc-base),(int)((cur-prev)*10.0)); prev=cur;
        }
        blip_end_frame(bl,(uint32_t)(e.cc-base)); base=e.cc;
        int n=blip_read_samples(bl,tmp,100000,0); out.insert(out.end(),tmp,tmp+n);
    }
    size_t n=std::min(out.size(),ref.size()), bad=0, first=(size_t)-1; int maxd=0;
    for(size_t k=0;k<n;k++){ int d=abs(out[k]-ref[k]); if(d){ bad++; if(first==(size_t)-1) first=k; if(d>maxd) maxd=d; } }
    printf("%zu blocks; ours %zu samples, mesen %zu samples, compared %zu: %zu differ (first at %ld), max abs diff %d\n",ends.size(),out.size(),ref.size(),n,bad,(long)first,maxd);
    return bad!=0;
}
