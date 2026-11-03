/* libc.c - the few C library functions the compiler and the runtime need in a freestanding build (no OS). */
#include <stddef.h>
#include <stdint.h>
void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d; uint32_t v = (uint8_t)c; v |= v << 8; v |= v << 16;
    while (n && ((uintptr_t)p & 3)) { *p++ = (uint8_t)c; n--; }
    uint32_t *q = (uint32_t *)p;
    while (n >= 16) { q[0] = v; q[1] = v; q[2] = v; q[3] = v; q += 4; n -= 16; }
    while (n >= 4) { *q++ = v; n -= 4; }
    p = (uint8_t *)q;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *p = d; const uint8_t *q = s;
    if ((((uintptr_t)p | (uintptr_t)q) & 3) == 0) {
        uint32_t *pl = (uint32_t *)p; const uint32_t *ql = (const uint32_t *)q;
        while (n >= 16) { pl[0] = ql[0]; pl[1] = ql[1]; pl[2] = ql[2]; pl[3] = ql[3]; pl += 4; ql += 4; n -= 16; }
        while (n >= 4) { *pl++ = *ql++; n -= 4; }
        p = (uint8_t *)pl; q = (const uint8_t *)ql;
    }
    while (n--) *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *p = d; const uint8_t *q = s;
    if (p <= q || p >= q + n) return memcpy(d, s, n);
    p += n; q += n;
    while (n--) *--p = *--q;
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}
