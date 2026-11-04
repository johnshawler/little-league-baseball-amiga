/* apu_test.c - self checks of runtime/apu.c that need no ROM and no reference trace.
 *   build + run (host):  gcc -O2 -Wall -Wno-misleading-indentation -o /tmp/apu_test tests/apu_test.c runtime/apu.c && /tmp/apu_test
 *   exit status 0 = all checks passed.
 * What is checked:
 *   1. LFSR skip-ahead tables (long mode is one 32767 cycle, skip == repeated stepping, both modes).
 *   2. LAZY == CYCLE BY CYCLE: random register traffic (all of $4000-$4013, $4015, $4017, status reads) is run once with the APU advanced and the channels
 *      synchronised at every single cycle, and once touching the APU only at the accesses.  Status values and the complete raw event streams must be identical.
 *   3. Frame counter / IRQ flag timing at power-on (numbers verified against the reference emulator trace over 4751 reads, see docs/APU.md) and 5-step mode.
 *   4. Length counter load / halt / $4015 disable.
 *   5. apu_render16 == brute force per-cycle integration of the mixer for random sample counts.
 * The verification against the real reference emulator (R $4015 values, channel events, PCM) is done by tools/apu_verify.c, see docs/APU.md. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../runtime/apu.h"

static int fails, checks;
#define CHECK(c, ...) do{ checks++; if(!(c)){ fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } }while(0)
static uint32_t rs = 12345;
static uint32_t rnd(void){ rs = rs*1664525u + 1013904223u; return rs >> 8; }

static Apu A, B;

/* ---- 1 */
static void test_lfsr(void){
    /* exercised through the public API: noise channel, period index 0, audible constant volume, compare event stream of one big skip vs many small steps happens in test 2;
       here: just check that mode-0 sequence really is one 32767 cycle */
    unsigned s = 1, n = 0; do{ s = (s>>1) | (((s^(s>>1))&1)<<14); n++; }while(s != 1 && n < 40000);
    CHECK(n == 32767, "long mode cycle length %u", n);
    { unsigned L31 = 0, L93 = 0, st; for(st = 1; st < 32768; st++){ unsigned t = st, L = 0; do{ t = (t>>1) | (((t^(t>>6))&1)<<14); L++; }while(t != st); if(L == 31) L31++; else if(L == 93) L93++; else CHECK(0, "short mode cycle %u", L); }
      CHECK(L31 == 31 && L93 == 32736, "short mode cycle census %u %u", L31, L93); }
}

/* ---- 2 */
#define NACC 400
typedef struct { uint64_t c; int kind; uint16_t a; uint8_t v; } Acc;      /* kind 0 write, 1 read $4015 */
static Acc acc[NACC];
static void gen(uint64_t span){
    int i; uint64_t c = 3;
    /* enable channels first so that length counters can load */
    for(i = 0; i < NACC; i++){
        uint32_t r = rnd() % 100; Acc *x = &acc[i];
        c += 1 + (rnd() % (i % 7 == 0 ? 3 : (span / NACC) * 2));
        x->c = c;
        if(r < 14){ x->kind = 1; x->a = 0x4015; x->v = 0; }
        else {
            x->kind = 0;
            if(r < 20){ x->a = 0x4015; x->v = (uint8_t)(rnd() & 0x1F); }
            else if(r < 22){ x->a = 0x4017; x->v = (uint8_t)(rnd() & 0xC0); }
            else if(r < 24){ x->a = 0x4010 + rnd() % 4; x->v = (uint8_t)rnd(); }
            else { x->a = 0x4000 + rnd() % 16; x->v = (uint8_t)rnd(); if((x->a & 3) == 3 && (rnd() & 1)) x->v &= 0xF8 | 7; }
        }
    }
    acc[0].kind = 0; acc[0].a = 0x4015; acc[0].v = 0x0F;
}
typedef struct { uint32_t t; uint8_t ch, l; } Ev;
static Ev evA[1<<20], evB[1<<20]; static unsigned nA, nB;
static void drain(Apu *x, Ev *v, unsigned *n, uint64_t until){ uint32_t t; int ch, l; while(apu_pop_event(x, until, &t, &ch, &l)){ v[*n].t = t; v[*n].ch = (uint8_t)ch; v[*n].l = (uint8_t)l; (*n)++; } }
/* per channel comparison (the pop order between channels at equal times is not part of the contract) */
static int stream_eq(const char *what){
    int ch; for(ch = 0; ch < 4; ch++){
        unsigned i = 0, j = 0, n = 0;
        for(;;){
            while(i < nA && evA[i].ch != ch) i++; while(j < nB && evB[j].ch != ch) j++;
            if(i >= nA && j >= nB) break;
            if(i >= nA || j >= nB || evA[i].t != evB[j].t || evA[i].l != evB[j].l){ printf("  %s: ch %d event %u differs\n", what, ch, n); return 0; }
            i++; j++; n++;
        }
    }
    return 1;
}
static void test_lazy(void){
    int seed;
    for(seed = 0; seed < 150; seed++){
        uint64_t span = (seed % 3 == 0) ? 20000 : (seed % 3 == 1) ? 120000 : 400000, end = acc[0].c, c; int i, ok = 1, ia = 0; uint64_t lastdrain;
        uint8_t sa[NACC], sb[NACC]; int nr = 0;
        rs = 1000 + seed; nA = nB = 0; gen(span); end = acc[NACC-1].c + 50000;
        apu_reset(&A); apu_reset(&B);
        /* A: advance and synchronise the channels every cycle */
        for(c = 0; c < end; c++){
            while(ia < NACC && acc[ia].c == c){
                if(acc[ia].kind) sa[nr++] = apu_read_status(&A, c); else apu_write(&A, acc[ia].a, acc[ia].v, c);
                ia++;
            }
            apu_run_to(&A, c); apu_flush(&A, c + 1);
            if((c & 2047) == 2047) drain(&A, evA, &nA, c + 1);
        }
        drain(&A, evA, &nA, end);
        /* B: touch the APU only at the accesses */
        nr = 0; lastdrain = 0;
        for(i = 0; i < NACC; i++){ if(acc[i].kind) sb[nr++] = apu_read_status(&B, acc[i].c); else apu_write(&B, acc[i].a, acc[i].v, acc[i].c);
            if(acc[i].c - lastdrain > 6000){ apu_flush(&B, acc[i].c); drain(&B, evB, &nB, acc[i].c); lastdrain = acc[i].c; } }   /* every ~6000 cycles: flush so the event queues cannot overflow */
        { uint64_t q; for(q = lastdrain; q < end; q += 6000){ apu_flush(&B, q); drain(&B, evB, &nB, q); } }
        apu_flush(&B, end); drain(&B, evB, &nB, end);
        for(i = 0; i < nr; i++) if(sa[i] != sb[i]){ ok = 0; printf("  seed %d read %d: %02x vs %02x\n", seed, i, sa[i], sb[i]); break; }
        CHECK(ok, "status reads differ lazy vs per-cycle (seed %d)", seed);
        ok = stream_eq("events");
        CHECK(ok, "event streams differ lazy vs per-cycle (seed %d)", seed);
        CHECK(A.dropped == 0 && B.dropped == 0, "queue overflow");
    }
}

/* ---- 3, 4 */
static void test_timing(void){
    apu_reset(&A);
    /* power-on: frame counter reads 5 at bus cycle 7 => step 3 (29828) at bus cycle 29830 */
    apu_run_to(&A, 100);
    CHECK(apu_irq_cycle(&A) == 29830, "irq cycle %llu", (unsigned long long)apu_irq_cycle(&A));
    CHECK((apu_read_status(&A, 29829) & 0x40) == 0, "irq early");
    CHECK((apu_read_status(&A, 29830) & 0x40) != 0, "irq at 29830");
    CHECK((apu_read_status(&A, 29830 + 1) & 0x40) != 0, "irq set again by step 29829 (Mesen/hardware: three consecutive cycles)");
    CHECK((apu_read_status(&A, 29830 + 3) & 0x40) != 0, "set a third time by step 29830");
    CHECK((apu_read_status(&A, 29830 + 4) & 0x40) == 0, "clear afterwards");
    CHECK(apu_irq_cycle(&A) == 29830 + 29830, "next irq %llu", (unsigned long long)apu_irq_cycle(&A));
    /* inhibit */
    apu_reset(&A); apu_write(&A, 0x4017, 0x40, 100);
    CHECK(apu_irq_cycle(&A) == ~0ull, "inhibited"); CHECK((apu_read_status(&A, 40000) & 0x40) == 0, "inhibit flag");
    /* length counter: $4015 enable, $4003 load index 1 (254), visible at the next cycle, 4-step: half-frame clocks decrement */
    apu_reset(&A); apu_write(&A, 0x4015, 0x01, 10); apu_write(&A, 0x4003, 0x08, 20);
    CHECK((apu_read_status(&A, 20) & 1) == 0, "length not loaded in the write cycle");
    CHECK((apu_read_status(&A, 21) & 1) == 1, "length loaded one cycle later");
    apu_write(&A, 0x4015, 0x00, 30); CHECK((apu_read_status(&A, 31) & 1) == 0, "disable clears");
    apu_write(&A, 0x4003, 0x08, 40); CHECK((apu_read_status(&A, 42) & 1) == 0, "load ignored while disabled");
    /* length 10 (index 0), no halt: expires after 10 half-frame clocks = 5 frames (2 per frame) */
    apu_reset(&A); apu_write(&A, 0x4015, 0x01, 10); apu_write(&A, 0x4000, 0x00, 11); apu_write(&A, 0x4003, 0x00, 12);
    CHECK((apu_read_status(&A, 100000) & 1) == 1, "still playing after ~3.3 frames");
    CHECK((apu_read_status(&A, 29830*5 + 1000) & 1) == 0, "expired");
    /* halt flag keeps it */
    apu_reset(&A); apu_write(&A, 0x4015, 0x04, 10); apu_write(&A, 0x4008, 0x80, 11); apu_write(&A, 0x400B, 0x00, 12);
    CHECK((apu_read_status(&A, 29830*9) & 4) == 4, "halted triangle length stays");
    /* 5-step mode: writing $4017 bit 7 clocks immediately (after the 3/4 cycle delay) */
    apu_reset(&A); apu_write(&A, 0x4015, 0x01, 10); apu_write(&A, 0x4000, 0x00, 11); apu_write(&A, 0x4003, 0x00, 12);
    { int i; for(i = 0; i < 10; i++) apu_write(&A, 0x4017, 0x80, 100 + 10*i); }   /* 10 clocks */
    CHECK((apu_read_status(&A, 300) & 1) == 0, "5-step mode half-frame clocks expire the length counter");
}

/* ---- 5 */
static void test_render(void){
    int t;
    for(t = 0; t < 20; t++){
        uint64_t end = 60000 + 30000*t, from = 0; unsigned rate = 8000 + 3000*(t % 9), n, i; static int16_t out[4096]; static uint8_t mix[1200000]; uint64_t c; uint32_t tt; int ch, l, lv[4] = {0,0,0,0};
        rs = 77 + t; gen(end - 1000);
        apu_reset(&A);
        { int k; for(k = 0; k < NACC; k++){ if(acc[k].c >= end - 100) break; if(!acc[k].kind) apu_write(&A, acc[k].a, acc[k].v, acc[k].c); } }
        memcpy(&B, &A, sizeof A);
        /* brute force from a copy: the exact per-cycle mixer level */
        apu_flush(&B, end); c = 0;
        { unsigned lastmix = 0; static unsigned lvl[1200000]; (void)mix;
          while(apu_pop_event(&B, end, &tt, &ch, &l)){ while(c < tt) lvl[c++] = lastmix; lv[ch] = l; lastmix = apu_mix(lv[0], lv[1], lv[2], lv[3]); }
          while(c < end) lvl[c++] = lastmix;
          n = (unsigned)((end * rate) / APU_CPU_HZ); if(n > 4096) n = 4096;
          apu_render16(&A, from, end, out, n);
          { unsigned bad = 0; uint64_t span = end - from;
            for(i = 0; i < n; i++){ uint64_t b0 = from + (span * i) / n, b1 = from + (span * (i+1)) / n, s = 0, k; for(k = b0; k < b1; k++) s += lvl[k]; if((uint64_t)out[i] != s / (b1 - b0)) bad++; }
            CHECK(bad == 0, "render16 differs from brute force in %u of %u samples (t=%d)", bad, n, t); } }
    }
}

/* ---- 6: the fast render path (merge + core, 32 bit) == the straightforward reference path, frame after frame (contiguous windows with register traffic in between),
 * 8 and 16 bit output, odd sample counts, tiny sample lengths (2 and 3 cycles), plus the 8 bit output against the brute force definition */
static void test_render_fast(void){
    int t;
    for(t = 0; t < 80; t++){
        uint64_t c = 0; int k, next = 0, ok = 1, ok8 = 1; static int8_t o8a[4096], o8b[4096]; static int16_t o16a[4096], o16b[4096]; unsigned bad = 0;
        rs = 900 + t; gen(400000); apu_reset(&A);
        for(k = 0; k < 12; k++){
            uint64_t span = 1000 + rnd() % ((t % 4 == 0) ? 3000 : 40000), to = c + span; unsigned n = (t % 5 == 0) ? (unsigned)(span / (2 + rnd() % 2)) : 20 + rnd() % 900, i;
            if(n > 4096) n = 4096;
            while(next < NACC && acc[next].c < to){ if(!acc[next].kind) apu_write(&A, acc[next].a, acc[next].v, acc[next].c); next++; }
            memcpy(&B, &A, sizeof A);
            if(k & 1){ apu_render8(&A, c, to, o8a, n); apu_render8_ref(&B, c, to, o8b, n); bad += memcmp(o8a, o8b, n) != 0; }
            else { apu_render16(&A, c, to, o16a, n); apu_render16_ref(&B, c, to, o16b, n); for(i = 0; i < n; i++) if(o16a[i] != o16b[i]) { bad++; break; } }
            if(memcmp(A.qh, B.qh, sizeof A.qh) || memcmp(A.qt, B.qt, sizeof A.qt) || A.rsq != B.rsq || A.rtn != B.rtn || A.dropped != B.dropped) ok = 0;
            c = to;
        }
        CHECK(bad == 0 && ok, "fast render differs from reference (t=%d, %u windows differ, state %d)", t, bad, ok); (void)ok8;
    }
    /* 8 bit == (v*83>>10)-128 of the 16 bit value, from the same events */
    { int t2; for(t2 = 0; t2 < 10; t2++){ static int8_t o8[2048]; static int16_t o16[2048]; unsigned n = 300 + 50*t2, i, bad = 0; rs = 31 + t2; gen(100000); apu_reset(&A);
        { int kk; for(kk = 0; kk < NACC; kk++){ if(acc[kk].c >= 60000) break; if(!acc[kk].kind) apu_write(&A, acc[kk].a, acc[kk].v, acc[kk].c); } }
        memcpy(&B, &A, sizeof A); apu_render8(&A, 0, 60000, o8, n); apu_render16(&B, 0, 60000, o16, n);
        for(i = 0; i < n; i++) if(o8[i] != (int8_t)(((unsigned)o16[i] * 83u >> 10) - 128u)) bad++;
        CHECK(bad == 0, "render8 != render16 conversion (%u samples)", bad); } }
}
/* ---- 7: cue mode sanity: a pulse note, a triangle note and noise give the expected Paula periods and volumes */
static void test_cue(void){
    ApuCue q; apu_reset(&A); apu_set_audio(&A, 0); apu_cue_init();
    apu_write(&A, 0x4015, 0x0F, 10);
    apu_write(&A, 0x4000, 0xBF, 20);      /* duty 2 (50 %), halt, constant volume 15 */
    apu_write(&A, 0x4002, 0xFD, 30); apu_write(&A, 0x4003, 0x08, 40);       /* period 0xFD: 440 Hz class */
    apu_write(&A, 0x4008, 0xFF, 50); apu_write(&A, 0x400A, 0x7E, 60); apu_write(&A, 0x400B, 0x08, 70);
    apu_write(&A, 0x400C, 0x3F, 80); apu_write(&A, 0x400E, 0x04, 90); apu_write(&A, 0x400F, 0x08, 100);
    apu_cue_frame(&A, 29780, 1, &q);
    CHECK(q.ch[0].vol > 0 && q.ch[0].vol <= 64, "pulse volume %u", q.ch[0].vol);
    { double f = 1789773.0 / (16.0 * (0xFD + 1)), fp = 3546895.0 / (q.ch[0].per * (8.0 / (1u << (q.ch[0].wave % 3)))); CHECK(fp / f > 0.998 && fp / f < 1.002, "pulse pitch %.2f vs %.2f Hz", fp, f); }
    CHECK(q.ch[2].vol > 0, "triangle audible");
    { double f = 1789773.0 / (32.0 * (0x7E + 1)), fp = 3546895.0 / (q.ch[2].per * (32.0 / (1u << (q.ch[2].wave - 12)))); CHECK(fp / f > 0.998 && fp / f < 1.002, "triangle pitch %.2f vs %.2f Hz", fp, f); }
    CHECK(q.ch[3].vol > 0 && q.ch[3].per >= 124, "noise cue");
    { unsigned c; for(c = 0; c < 4; c++) CHECK(q.ch[c].per >= 124, "period %u below Paula's limit", q.ch[c].per); }
}

int main(void){
    test_lfsr(); test_lazy(); test_timing(); test_render(); test_render_fast(); test_cue();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
