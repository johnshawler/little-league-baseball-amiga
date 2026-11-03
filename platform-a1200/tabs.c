/* tabs.c - the 8 dispatch tables of the translated program (7 switchable-window tables + the fixed one, 16384 entries each = 64 KB each).
 * On the disk they are stored sparsely (tabsp.s, generated from the translator's tab.s by tools/gen_tabsp.py: about 3200 entries of 8 bytes);
 * tabs_expand() fills all entries with rt_unk_ent (the slow-path entry) and patches the translated block entries in. */
#include <stdint.h>
extern void rt_unk_ent(void);
uint32_t tab_win0[16384] __attribute__((aligned(4))), tab_win1[16384] __attribute__((aligned(4))), tab_win2[16384] __attribute__((aligned(4))), tab_win3[16384] __attribute__((aligned(4)));
uint32_t tab_win4[16384] __attribute__((aligned(4))), tab_win5[16384] __attribute__((aligned(4))), tab_win6[16384] __attribute__((aligned(4))), tab_fix[16384] __attribute__((aligned(4)));
extern const uint32_t tabsp_n, tabsp[];
void tabs_expand(void)
{
    uint32_t *t[8] = { tab_win0, tab_win1, tab_win2, tab_win3, tab_win4, tab_win5, tab_win6, tab_fix };
    for (unsigned k = 0; k < 8; k++) { uint32_t *p = t[k]; for (unsigned i = 0; i < 16384; i++) p[i] = (uint32_t)rt_unk_ent; }
    for (uint32_t i = 0; i < tabsp_n; i++) { uint32_t e = tabsp[2 * i]; t[e >> 16][e & 0xFFFF] = tabsp[2 * i + 1]; }
}
