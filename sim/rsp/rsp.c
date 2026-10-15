/* The RSP reference model (rsp.h). Written from the RSP's public
 * documentation; where that leaves a detail open the model is checked against
 * an independent RSP implementation in simulation, on real tasks. */
#include "rsp.h"
#include <string.h>
#include <stdio.h>

/* ---- memory: DMEM/IMEM wrap at 4 KiB, big-endian */
static inline uint32_t dm8(rsp_t *s, uint32_t a) { return s->dmem[a & 0xFFFu]; }
static inline void dmw8(rsp_t *s, uint32_t a, uint32_t v) { s->dmem[a & 0xFFFu] = (uint8_t)v; }
static uint32_t dm16(rsp_t *s, uint32_t a) { return dm8(s, a) << 8 | dm8(s, a + 1); }
static uint32_t dm32(rsp_t *s, uint32_t a) { return dm16(s, a) << 16 | dm16(s, a + 2); }
static void dmw16(rsp_t *s, uint32_t a, uint32_t v) { dmw8(s, a, v >> 8); dmw8(s, a + 1, v); }
static void dmw32(rsp_t *s, uint32_t a, uint32_t v) { dmw16(s, a, v >> 16); dmw16(s, a + 2, v); }

/* a vector register's bytes: byte 0 is lane 0's high byte */
static inline uint8_t vb(const uint16_t *v, unsigned b)
{
    b &= 15u;
    return (b & 1u) ? (uint8_t)v[b >> 1] : (uint8_t)(v[b >> 1] >> 8);
}
static inline void vbw(uint16_t *v, unsigned b, uint32_t x)
{
    b &= 15u; x &= 0xFFu;
    if (b & 1u) v[b >> 1] = (uint16_t)((v[b >> 1] & 0xFF00u) | x);
    else        v[b >> 1] = (uint16_t)((v[b >> 1] & 0x00FFu) | x << 8);
}

/* ---- the reciprocal and reciprocal-square-root ROMs (512 entries each) */
static uint16_t rcp_rom[512], rsq_rom[512];
static int roms_ready;
static void make_roms(void)
{
    rcp_rom[0] = 0xFFFF;
    for (uint32_t i = 1; i < 512; i++)
        rcp_rom[i] = (uint16_t)((((1ull << 34) / (i + 512u)) + 1u) >> 8);
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t a = (i + 512u) >> (i & 1u);
        uint64_t b = 1u << 17;
        while (a * (b + 1u) * (b + 1u) < (1ull << 44)) b++;
        rsq_rom[i] = (uint16_t)(b >> 1);
    }
    roms_ready = 1;
}
static int clz32(uint32_t x) { int n = 0; if (!x) return 32; while (!(x & 0x80000000u)) { x <<= 1; n++; } return n; }
static int32_t rcp(int32_t input, int sqrt_)
{
    if (input == 0) return 0x7FFFFFFF;
    if (input == (int32_t)0xFFFF8000) return (int32_t)0xFFFF0000;
    int32_t mask = input >> 31;
    int32_t data = input ^ mask;
    if (input > (int32_t)0xFFFF8000) data -= mask;
    int shift = clz32((uint32_t)data);
    uint32_t index = (((uint32_t)data << shift) & 0x7FC00000u) >> 22;
    uint32_t result;
    if (sqrt_) {
        index = (index & 0x1FEu) | ((uint32_t)shift & 1u);
        result = (0x10000u | rsq_rom[index]) << 14;
        result >>= (31 - shift) >> 1;
    } else {
        result = (0x10000u | rcp_rom[index]) << 14;
        result >>= 31 - shift;
    }
    return (int32_t)(result ^ (uint32_t)mask);
}

/* ---- DMA: rows of (len + 1) bytes, 8-byte units, `skip` bytes between rows in RDRAM */
static void dma(rsp_t *s, uint32_t reg, int to_sp)
{
    uint32_t len = (reg & 0xFFFu) | 7u, count = (reg >> 12) & 0xFFu, skip = (reg >> 20) & 0xFF8u;
    /* a transfer starts from the addresses last written (not from where the
     * one before ended) */
    s->sp_addr = s->sp_addr_w; s->ram_addr = s->ram_addr_w;
    uint8_t *bank = (s->sp_addr & 0x1000u) ? s->imem : s->dmem;
    uint32_t sp = s->sp_addr & 0xFF8u, ram = s->ram_addr & 0xFFFFF8u;
    for (uint32_t c = 0; c <= count; c++) {
        if (c) ram += skip;                             /* (between rows only) */
        for (uint32_t i = 0; i <= len; i++) {
            uint32_t ra = (ram + i) % s->rdram_size;
            if (to_sp) bank[(sp + i) & 0xFFFu] = s->rdram[ra];
            else       s->rdram[ra] = bank[(sp + i) & 0xFFFu];
        }
        sp = (sp + len + 1u) & 0xFFFu;
        ram += len + 1u;
        if (to_sp) s->dma_rd_bytes += len + 1u; else s->dma_wr_bytes += len + 1u;
    }
    s->sp_addr = (s->sp_addr & 0x1000u) | (sp & 0xFF8u);
    s->ram_addr = ram & 0xFFFFF8u;
    s->rd_len = s->wr_len = 0xFF8u | skip << 20;      /* as left: the length run out, the count 0, the skip */
    s->dmas++;
}

/* ---- the RDP's commands, as DPC_END moves: complete commands to the callback */
static int rdp_cmd_words(uint32_t w0)
{
    uint32_t op = (w0 >> 24) & 0x3Fu;
    if (op >= 0x08 && op <= 0x0F) return 4 + ((op & 4u) ? 8 : 0) + ((op & 2u) ? 8 : 0) + ((op & 1u) ? 2 : 0);
    if (op == 0x24 || op == 0x25) return 2;
    return 1;
}
static void rdp_feed(rsp_t *s)
{
    while (s->dpc_current != s->dpc_end) {
        uint32_t hi, lo, a = s->dpc_current;
        if (s->dpc_status & 1u) { hi = dm32(s, a); lo = dm32(s, a + 4); }   /* XBUS: from DMEM */
        else {
            const uint8_t *p = s->rdram + (a % s->rdram_size);
            hi = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
            lo = (uint32_t)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7];
        }
        s->cmdbuf[2 * s->cmdn] = hi; s->cmdbuf[2 * s->cmdn + 1] = lo;
        s->cmdn++;
        if (s->cmdn == rdp_cmd_words(s->cmdbuf[0])) {
            if (s->rdp_cmd) s->rdp_cmd(s->user, s->cmdbuf, s->cmdn);
            s->rdp_cmds++;
            s->cmdn = 0;
        }
        s->dpc_current = (s->dpc_current + 8u) & 0xFFFFF8u;
    }
}

/* ---- COP0 */
static uint32_t mfc0(rsp_t *s, unsigned rd)
{
    switch (rd & 15u) {
    case 0: return s->sp_addr;
    case 1: return s->ram_addr;
    case 2: return s->rd_len;
    case 3: return s->wr_len;
    case 4: return s->status;
    case 5: return 0;                               /* DMA full */
    case 6: return 0;                               /* DMA busy */
    case 7: { uint32_t v = s->semaphore; s->semaphore = 1; return v; }
    case 8: return s->dpc_start;
    case 9: return s->dpc_end;
    case 10: return s->dpc_current;
    case 11: return (s->dpc_status & 1u) | 0x80u;   /* XBUS; the command buffer is ready */
    default: return 0;                              /* the RDP's counters */
    }
}
static void mtc0(rsp_t *s, unsigned rd, uint32_t v)
{
    switch (rd & 15u) {
    case 0: s->sp_addr = s->sp_addr_w = v & 0x1FF8u; break;
    case 1: s->ram_addr = s->ram_addr_w = v & 0xFFFFF8u; break;
    case 2: s->rd_len = v; dma(s, v, 1); break;
    case 3: s->wr_len = v; dma(s, v, 0); break;
    case 4:
        /* a clear / set pair: one of them alone acts; both together leave the bit */
#define PAIR(clr, set, bit) do { if ((v & (clr)) && !(v & (set))) s->status &= ~(bit); if ((v & (set)) && !(v & (clr))) s->status |= (bit); } while (0)
        PAIR(0x1u, 0x2u, 0x1u);
        if (v & 0x4u) s->status &= ~0x2u;
        PAIR(0x20u, 0x40u, 0x20u);
        PAIR(0x80u, 0x100u, 0x40u);
        for (int i = 0; i < 8; i++) PAIR(1u << (9 + 2 * i), 1u << (10 + 2 * i), 1u << (7 + i));
#undef PAIR
        break;
    case 7: s->semaphore = 0; break;
    case 8: s->dpc_start = s->dpc_current = v & 0xFFFFF8u; break;
    case 9: s->dpc_end = v & 0xFFFFF8u; rdp_feed(s); break;
    case 11:
        if (v & 0x1u) s->dpc_status &= ~1u;
        if (v & 0x2u) s->dpc_status |= 1u;
        break;
    default: break;
    }
}

/* ---- COP2: the vector unit */
static inline int64_t acc_get(const rsp_t *s, int i)
{
    return (int64_t)(((uint64_t)(uint16_t)s->acc_h[i] << 32 | (uint64_t)s->acc_m[i] << 16 | s->acc_l[i]) << 16) >> 16;
}
static inline void acc_set(rsp_t *s, int i, int64_t a)
{
    s->acc_h[i] = (uint16_t)(a >> 32); s->acc_m[i] = (uint16_t)(a >> 16); s->acc_l[i] = (uint16_t)a;
}
static inline uint16_t clamp_s(int64_t a)           /* signed clamp of acc[47:16] */
{
    int32_t v = (int32_t)(a >> 16);
    return (uint16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
static inline uint16_t clamp_u(int64_t a)           /* VMULU/VMACU */
{
    int32_t v = (int32_t)(a >> 16);
    return (uint16_t)(v < 0 ? 0 : v > 32767 ? 0xFFFF : v);
}
static inline uint16_t clamp_l(int64_t a)           /* the low word, if acc[47:16] is a signed 16-bit value */
{
    int32_t v = (int32_t)(a >> 16);
    return (uint16_t)(v > 32767 ? 0xFFFF : v < -32768 ? 0 : (uint16_t)a);
}
static inline int vt_lane(unsigned e, int i)        /* the lane of vt that lane i takes */
{
    if (e < 2) return i;
    if (e < 4) return (i & ~1) | (int)(e & 1u);
    if (e < 8) return (i & ~3) | (int)(e & 3u);
    return (int)(e & 7u);
}

static void vec_op(rsp_t *s, uint32_t in)
{
    unsigned e = (in >> 21) & 15u, vtn = (in >> 16) & 31u, vsn = (in >> 11) & 31u, vdn = (in >> 6) & 31u, fn = in & 63u;
    uint16_t vs[8], vt[8], vd[8];
    for (int i = 0; i < 8; i++) { vs[i] = s->vr[vsn][i]; vt[i] = s->vr[vtn][vt_lane(e, i)]; vd[i] = s->vr[vdn][i]; }
    uint16_t vco = s->vco, vcc = s->vcc; uint8_t vce = s->vce;
    int wr = 1;
    switch (fn) {
    case 0x00: case 0x01:                           /* VMULF, VMULU */
        for (int i = 0; i < 8; i++) {
            int64_t a = ((int64_t)(int16_t)vs[i] * (int16_t)vt[i]) * 2 + 0x8000;
            acc_set(s, i, a); vd[i] = fn ? clamp_u(a) : clamp_s(a);
        }
        break;
    case 0x08: case 0x09:                           /* VMACF, VMACU */
        for (int i = 0; i < 8; i++) {
            int64_t a = acc_get(s, i) + ((int64_t)(int16_t)vs[i] * (int16_t)vt[i]) * 2;
            acc_set(s, i, a); a = acc_get(s, i); vd[i] = (fn & 1u) ? clamp_u(a) : clamp_s(a);
        }
        break;
    case 0x04: case 0x0C:                           /* VMUDL, VMADL */
        for (int i = 0; i < 8; i++) {
            int64_t a = ((uint32_t)vs[i] * (uint32_t)vt[i]) >> 16;
            if (fn & 8u) a += acc_get(s, i);
            acc_set(s, i, a); vd[i] = clamp_l(acc_get(s, i));
        }
        break;
    case 0x05: case 0x0D:                           /* VMUDM, VMADM: signed vs x unsigned vt */
        for (int i = 0; i < 8; i++) {
            int64_t a = (int64_t)(int16_t)vs[i] * (int64_t)(uint16_t)vt[i];
            if (fn & 8u) a += acc_get(s, i);
            acc_set(s, i, a); vd[i] = clamp_s(acc_get(s, i));
        }
        break;
    case 0x06: case 0x0E:                           /* VMUDN, VMADN: unsigned vs x signed vt */
        for (int i = 0; i < 8; i++) {
            int64_t a = (int64_t)(uint16_t)vs[i] * (int64_t)(int16_t)vt[i];
            if (fn & 8u) a += acc_get(s, i);
            acc_set(s, i, a); vd[i] = clamp_l(acc_get(s, i));
        }
        break;
    case 0x07: case 0x0F:                           /* VMUDH, VMADH: into acc[47:16] */
        for (int i = 0; i < 8; i++) {
            int64_t a = ((int64_t)(int16_t)vs[i] * (int16_t)vt[i]) * 65536;
            if (fn & 8u) a += acc_get(s, i);
            acc_set(s, i, a); vd[i] = clamp_s(acc_get(s, i));
        }
        break;
    case 0x10:                                      /* VADD */
        for (int i = 0; i < 8; i++) {
            int32_t r = (int16_t)vs[i] + (int16_t)vt[i] + ((vco >> i) & 1);
            s->acc_l[i] = (uint16_t)r; vd[i] = (uint16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        vco = 0;
        break;
    case 0x11:                                      /* VSUB */
        for (int i = 0; i < 8; i++) {
            int32_t r = (int16_t)vs[i] - (int16_t)vt[i] - ((vco >> i) & 1);
            s->acc_l[i] = (uint16_t)r; vd[i] = (uint16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        vco = 0;
        break;
    case 0x13:                                      /* VABS */
        for (int i = 0; i < 8; i++) {
            int16_t a = (int16_t)vs[i], b = (int16_t)vt[i];
            if (a < 0)       { s->acc_l[i] = (uint16_t)-b; vd[i] = b == -32768 ? 0x7FFF : (uint16_t)-b; }
            else if (a == 0) { s->acc_l[i] = 0; vd[i] = 0; }
            else             { s->acc_l[i] = (uint16_t)b; vd[i] = (uint16_t)b; }
        }
        break;
    case 0x14:                                      /* VADDC */
        vco = 0;
        for (int i = 0; i < 8; i++) {
            uint32_t r = (uint32_t)vs[i] + vt[i];
            s->acc_l[i] = vd[i] = (uint16_t)r;
            if (r >> 16) vco |= 1u << i;
        }
        break;
    case 0x15:                                      /* VSUBC */
        vco = 0;
        for (int i = 0; i < 8; i++) {
            uint32_t r = (uint32_t)vs[i] - vt[i];
            s->acc_l[i] = vd[i] = (uint16_t)r;
            if (r >> 16) vco |= 1u << i;
            if (r & 0x1FFFFu) vco |= 0x100u << i;
        }
        break;
    case 0x1D:                                      /* VSAR: a slice of the accumulator */
        for (int i = 0; i < 8; i++)
            vd[i] = e == 8 ? s->acc_h[i] : e == 9 ? s->acc_m[i] : e == 10 ? s->acc_l[i] : 0;
        break;
    case 0x20: case 0x21: case 0x22: case 0x23: {   /* VLT, VEQ, VNE, VGE */
        uint16_t c = 0;
        for (int i = 0; i < 8; i++) {
            int16_t a = (int16_t)vs[i], b = (int16_t)vt[i];
            int ne = (vco >> (8 + i)) & 1, cy = (vco >> i) & 1, t;
            switch (fn) {
            case 0x20: t = a < b || (a == b && ne && cy); vd[i] = t ? vs[i] : vt[i]; break;
            case 0x21: t = a == b && !ne; vd[i] = vt[i]; break;
            case 0x22: t = a != b || ne; vd[i] = vs[i]; break;
            default:   t = a > b || (a == b && !(ne && cy)); vd[i] = t ? vs[i] : vt[i]; break;
            }
            s->acc_l[i] = vd[i];
            if (t) c |= 1u << i;
        }
        vcc = c; vco = 0;
        break;
    }
    case 0x25: {                                    /* VCH */
        vco = 0; vcc = 0; vce = 0;
        for (int i = 0; i < 8; i++) {
            int32_t a = (int16_t)vs[i], b = (int16_t)vt[i];
            int sign = (a ^ b) < 0, le, ge, ne, ce = 0;
            if (sign) {
                int32_t sum = a + b;
                ge = b < 0; le = sum <= 0; ce = sum == -1; ne = sum != 0 && !ce;
                vd[i] = le ? (uint16_t)-b : vs[i];
            } else {
                int32_t d = a - b;
                le = b < 0; ge = d >= 0; ne = d != 0;
                vd[i] = ge ? vt[i] : vs[i];
            }
            s->acc_l[i] = vd[i];
            if (sign) vco |= 1u << i;
            if (ne) vco |= 0x100u << i;
            if (le) vcc |= 1u << i;
            if (ge) vcc |= 0x100u << i;
            if (ce) vce |= 1u << i;
        }
        break;
    }
    case 0x24: {                                    /* VCL */
        uint16_t c = 0;
        for (int i = 0; i < 8; i++) {
            uint32_t a = vs[i], b = vt[i];
            int sign = (vco >> i) & 1, ne = (vco >> (8 + i)) & 1, le = (vcc >> i) & 1, ge = (vcc >> (8 + i)) & 1;
            if (sign) {
                if (!ne) {
                    uint32_t sum = a + b;
                    int zero = (sum & 0xFFFFu) == 0, carry = (sum >> 16) & 1u;
                    le = ((vce >> i) & 1) ? (zero || !carry) : (zero && !carry);
                }
                vd[i] = le ? (uint16_t)-b : vs[i];
            } else {
                if (!ne) ge = a >= b;
                vd[i] = ge ? vt[i] : vs[i];
            }
            s->acc_l[i] = vd[i];
            if (le) c |= 1u << i;
            if (ge) c |= 0x100u << i;
        }
        vcc = c; vco = 0; vce = 0;
        break;
    }
    case 0x26: {                                    /* VCR */
        uint16_t c = 0;
        for (int i = 0; i < 8; i++) {
            int32_t a = (int16_t)vs[i], b = (int16_t)vt[i];
            int le, ge;
            if ((a ^ b) < 0) { ge = b < 0; le = a + b + 1 <= 0; vd[i] = le ? (uint16_t)~b : vs[i]; }
            else             { le = b < 0; ge = a - b >= 0;     vd[i] = ge ? vt[i] : vs[i]; }
            s->acc_l[i] = vd[i];
            if (le) c |= 1u << i;
            if (ge) c |= 0x100u << i;
        }
        vcc = c; vco = 0; vce = 0;
        break;
    }
    case 0x27:                                      /* VMRG */
        for (int i = 0; i < 8; i++) s->acc_l[i] = vd[i] = ((vcc >> i) & 1) ? vs[i] : vt[i];
        vco = 0;
        break;
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:   /* VAND VNAND VOR VNOR VXOR VNXOR */
        for (int i = 0; i < 8; i++) {
            uint16_t r = (fn & 6u) == 0 ? (vs[i] & vt[i]) : (fn & 6u) == 2 ? (vs[i] | vt[i]) : (vs[i] ^ vt[i]);
            if (fn & 1u) r = (uint16_t)~r;
            s->acc_l[i] = vd[i] = r;
        }
        break;
    case 0x30: case 0x31: case 0x32: case 0x34: case 0x35: case 0x36: {   /* VRCP VRCPL VRCPH VRSQ VRSQL VRSQH */
        unsigned de = vsn & 7u;
        uint16_t x = s->vr[vtn][e & 7u];
        int sq = (fn & 4u) != 0;
        if ((fn & 3u) == 2u) {                      /* H: the input's high half in, the result's high half out */
            for (int i = 0; i < 8; i++) s->acc_l[i] = vt[i];
            s->div_in = (int32_t)((uint32_t)x << 16);
            s->dph = 1;
            vd[de] = (uint16_t)((uint32_t)s->div_out >> 16);
        } else {
            int32_t input = ((fn & 3u) == 1u && s->dph) ? (int32_t)((uint32_t)s->div_in | x) : (int16_t)x;
            int32_t r = rcp(input, sq);
            for (int i = 0; i < 8; i++) s->acc_l[i] = vt[i];
            s->div_out = r; s->dph = 0;
            vd[de] = (uint16_t)r;
        }
        break;
    }
    case 0x33: {                                    /* VMOV */
        unsigned de = vsn & 7u;
        for (int i = 0; i < 8; i++) s->acc_l[i] = vt[i];
        vd[de] = vt[de];
        break;
    }
    case 0x37: case 0x3F: wr = 0; break;            /* VNOP, VNULL */
    case 0x02: case 0x03: case 0x0A: case 0x0B:     /* VRNDP VMULQ VRNDN VMACQ (the MPEG microcode's): not modelled */
        s->unimpl++;
        wr = 0;
        break;
    default:                                        /* the reserved functions: vd = 0, the accumulator's low word vs + vt */
        for (int i = 0; i < 8; i++) { s->acc_l[i] = (uint16_t)(vs[i] + vt[i]); vd[i] = 0; }
        break;
    }
    if (wr) memcpy(s->vr[vdn], vd, sizeof vd);
    s->vco = vco; s->vcc = vcc; s->vce = vce;
}

/* LWC2 / SWC2 */
static void vec_load(rsp_t *s, uint32_t in)
{
    uint32_t base = s->r[(in >> 21) & 31u];
    unsigned vtn = (in >> 16) & 31u, op = (in >> 11) & 31u, e = (in >> 7) & 15u;
    int32_t off = (int32_t)(in << 25) >> 25;
    uint16_t *v = s->vr[vtn];
    static const uint8_t shift[12] = { 0, 1, 2, 3, 4, 4, 3, 3, 4, 4, 4, 4 };
    if (op > 11) { s->unimpl++; return; }
    uint32_t addr = base + (uint32_t)(off * (1 << shift[op]));
    if (e == 0 && (addr & ((1u << shift[op]) - 1u)) == 0) s->ls_plain[0][op]++;
    switch (op) {
    case 0: case 1: case 2: case 3:                 /* LBV LSV LLV LDV */
        for (unsigned k = 0; k < (1u << op) && e + k < 16; k++) vbw(v, e + k, dm8(s, addr + k));
        break;
    case 4:                                         /* LQV: to the end of the 16-byte block */
        for (unsigned k = 0; k < 16u - (addr & 15u) && e + k < 16; k++) vbw(v, e + k, dm8(s, addr + k));
        break;
    case 5: {                                       /* LRV: the block's bytes before addr, at the register's end */
        unsigned n = addr & 15u;
        for (unsigned k = 0; k < n && e + 16u - n + k < 16; k++) vbw(v, e + 16u - n + k, dm8(s, (addr & ~15u) + k));
        break;
    }
    case 6: case 7: {                               /* LPV, LUV: 8 bytes, one a lane */
        uint32_t index = (addr & 7u) - e; addr &= ~7u;
        for (unsigned i = 0; i < 8; i++) v[i] = (uint16_t)(dm8(s, addr + ((index + i) & 15u)) << (op == 6 ? 8 : 7));
        break;
    }
    case 8: {                                       /* LHV: every other byte */
        uint32_t index = (addr & 7u) - e; addr &= ~7u;
        for (unsigned i = 0; i < 8; i++) v[i] = (uint16_t)(dm8(s, addr + ((index + 2u * i) & 15u)) << 7);
        break;
    }
    case 11: {                                      /* LTV: a transposed load across 8 registers */
        uint32_t begin = addr & ~7u;
        uint32_t a = begin + ((e + (addr & 8u)) & 15u);
        unsigned vbase = vtn & ~7u, voff = e >> 1;
        for (unsigned i = 0; i < 8; i++) {
            uint32_t hi = dm8(s, a++); if (a == begin + 16u) a = begin;
            uint32_t lo = dm8(s, a++); if (a == begin + 16u) a = begin;
            s->vr[vbase + voff][i] = (uint16_t)(hi << 8 | lo);
            voff = (voff + 1u) & 7u;
        }
        break;
    }
    default: s->unimpl++; break;                    /* LFV, LWV */
    }
}
static void vec_store(rsp_t *s, uint32_t in)
{
    uint32_t base = s->r[(in >> 21) & 31u];
    unsigned vtn = (in >> 16) & 31u, op = (in >> 11) & 31u, e = (in >> 7) & 15u;
    int32_t off = (int32_t)(in << 25) >> 25;
    const uint16_t *v = s->vr[vtn];
    static const uint8_t shift[12] = { 0, 1, 2, 3, 4, 4, 3, 3, 4, 4, 4, 4 };
    if (op > 11) { s->unimpl++; return; }
    uint32_t addr = base + (uint32_t)(off * (1 << shift[op]));
    if (e == 0 && (addr & ((1u << shift[op]) - 1u)) == 0) s->ls_plain[1][op]++;
    switch (op) {
    case 0: case 1: case 2: case 3:                 /* SBV SSV SLV SDV */
        for (unsigned k = 0; k < (1u << op); k++) dmw8(s, addr + k, vb(v, e + k));
        break;
    case 4:                                         /* SQV */
        for (unsigned k = 0; k < 16u - (addr & 15u); k++) dmw8(s, addr + k, vb(v, e + k));
        break;
    case 5: {                                       /* SRV */
        unsigned n = addr & 15u;
        for (unsigned k = 0; k < n; k++) dmw8(s, (addr & ~15u) + k, vb(v, e + 16u - n + k));
        break;
    }
    case 6: case 7:                                 /* SPV, SUV */
        for (unsigned i = 0; i < 8; i++) {
            unsigned idx = e + i;
            int first = (idx & 15u) < 8;
            dmw8(s, addr + i, v[idx & 7u] >> ((op == 6) == first ? 8 : 7));
        }
        break;
    case 8: {                                       /* SHV */
        uint32_t index = addr & 7u; addr &= ~7u;
        for (unsigned i = 0; i < 8; i++) {
            unsigned b = e + 2u * i;
            dmw8(s, addr + ((index + 2u * i) & 15u), (uint32_t)vb(v, b) << 1 | vb(v, b + 1) >> 7);
        }
        break;
    }
    case 9: {                                       /* SFV */
        /* bits 14..7 of four lanes, a byte every fourth in the 16 from the
         * 8-byte block: lanes 0-3 (e 0), 1,2,3,0 (e 4), 4-7 (e 8), 5,6,7,4
         * (e 12) -- a JPEG microcode's U and V. Other
         * elements: not done. */
        if (e & 3u) { s->unimpl++; break; }
        uint32_t index = addr & 7u; addr &= ~7u;
        for (unsigned i = 0; i < 4; i++) {
            unsigned lane = (e & 8u) >> 1 | ((i + (e >> 2 & 1u)) & 3u);
            dmw8(s, addr + ((index + 4u * i) & 15u), v[lane] >> 7);
        }
        break;
    }
    case 10: {                                      /* SWV */
        uint32_t index = addr & 7u; addr &= ~7u;
        for (unsigned i = e; i < e + 16u; i++) dmw8(s, addr + (index++ & 15u), vb(v, i));
        break;
    }
    case 11: {                                      /* STV */
        unsigned start = vtn & ~7u, elem = 16u - (e & ~1u);
        uint32_t index = (addr & 7u) - (e & ~1u); addr &= ~7u;
        for (unsigned i = start; i < start + 8u; i++) {
            dmw8(s, addr + (index++ & 15u), vb(s->vr[i], elem++));
            dmw8(s, addr + (index++ & 15u), vb(s->vr[i], elem++));
        }
        break;
    }
    default: s->unimpl++; break;
    }
}

/* ---- the scalar unit */
void rsp_reset(rsp_t *s, uint8_t *rdram, uint32_t rdram_size)
{
    memset(s, 0, sizeof *s);
    s->rdram = rdram; s->rdram_size = rdram_size;
    s->status = 1;                                  /* halted */
    if (!roms_ready) make_roms();
}

int rsp_run(rsp_t *s, uint64_t max_insts)
{
    s->status &= ~1u;
    for (uint64_t n = 0; n < max_insts; n++)
        if (rsp_step(s)) return 0;
    return -1;
}

int rsp_step(rsp_t *s)
{
    {
        uint32_t pc = s->pc & 0xFFCu;
        uint32_t in = (uint32_t)s->imem[pc] << 24 | s->imem[pc + 1] << 16 | s->imem[pc + 2] << 8 | s->imem[pc + 3];
        uint32_t next = s->delay ? s->delay_target : (pc + 4u) & 0xFFCu;
        s->delay = 0;
        unsigned op = in >> 26, rs = (in >> 21) & 31u, rt = (in >> 16) & 31u, rd = (in >> 11) & 31u;
        uint32_t imm = in & 0xFFFFu, simm = (uint32_t)(int32_t)(int16_t)imm;
        uint32_t btarget = (pc + 4u + (simm << 2)) & 0xFFCu;
        uint32_t *r = s->r;
        int br = 0, link = 0;                       /* branch taken; link register */
        static int prev_vec;
        s->last_pc = pc; s->last_ins = in;
        int is_vec = (op == 0x12 && (in & (1u << 25))) || op == 0x32 || op == 0x3A;
        if (is_vec && prev_vec) s->vec_after_vec++;
        prev_vec = is_vec;
        s->insts++; s->cycles++;
        switch (op) {
        case 0x00:
            switch (in & 63u) {
            case 0x00: r[rd] = r[rt] << ((in >> 6) & 31u); s->prof[RSP_P_SHIFT]++; break;
            case 0x02: r[rd] = r[rt] >> ((in >> 6) & 31u); s->prof[RSP_P_SHIFT]++; break;
            case 0x03: r[rd] = (uint32_t)((int32_t)r[rt] >> ((in >> 6) & 31u)); s->prof[RSP_P_SHIFT]++; break;
            case 0x04: r[rd] = r[rt] << (r[rs] & 31u); s->prof[RSP_P_SHIFT]++; break;
            case 0x06: r[rd] = r[rt] >> (r[rs] & 31u); s->prof[RSP_P_SHIFT]++; break;
            case 0x07: r[rd] = (uint32_t)((int32_t)r[rt] >> (r[rs] & 31u)); s->prof[RSP_P_SHIFT]++; break;
            case 0x08: s->delay = 1; s->delay_target = r[rs] & 0xFFCu; s->prof[RSP_P_JUMP]++; break;
            case 0x09: { uint32_t t = r[rs] & 0xFFCu; r[rd] = (pc + 8u) & 0xFFCu; s->delay = 1; s->delay_target = t; s->prof[RSP_P_JUMP]++; break; }
            case 0x0D: s->status |= 3u; s->pc = next; r[0] = 0; s->prof[RSP_P_OTHER]++; return 1;   /* BREAK */
            case 0x20: case 0x21: r[rd] = r[rs] + r[rt]; s->prof[RSP_P_ALU]++; break;
            case 0x22: case 0x23: r[rd] = r[rs] - r[rt]; s->prof[RSP_P_ALU]++; break;
            case 0x24: r[rd] = r[rs] & r[rt]; s->prof[RSP_P_ALU]++; break;
            case 0x25: r[rd] = r[rs] | r[rt]; s->prof[RSP_P_ALU]++; break;
            case 0x26: r[rd] = r[rs] ^ r[rt]; s->prof[RSP_P_ALU]++; break;
            case 0x27: r[rd] = ~(r[rs] | r[rt]); s->prof[RSP_P_ALU]++; break;
            case 0x2A: r[rd] = (int32_t)r[rs] < (int32_t)r[rt]; s->prof[RSP_P_ALU]++; break;
            case 0x2B: r[rd] = r[rs] < r[rt]; s->prof[RSP_P_ALU]++; break;
            default: s->prof[RSP_P_OTHER]++; break; /* (the multiplies, divides, SYNC...: nothing) */
            }
            break;
        case 0x01:
            s->prof[RSP_P_BRANCH]++;
            switch (rt) {
            case 0x00: br = (int32_t)r[rs] < 0; break;
            case 0x01: br = (int32_t)r[rs] >= 0; break;
            case 0x10: br = (int32_t)r[rs] < 0; link = 1; break;
            case 0x11: br = (int32_t)r[rs] >= 0; link = 1; break;
            default: break;
            }
            if (link) r[31] = (pc + 8u) & 0xFFCu;
            if (br) { s->delay = 1; s->delay_target = btarget; }
            break;
        case 0x02: s->delay = 1; s->delay_target = (in << 2) & 0xFFCu; s->prof[RSP_P_JUMP]++; break;
        case 0x03: r[31] = (pc + 8u) & 0xFFCu; s->delay = 1; s->delay_target = (in << 2) & 0xFFCu; s->prof[RSP_P_JUMP]++; break;
        case 0x04: if (r[rs] == r[rt]) { s->delay = 1; s->delay_target = btarget; } s->prof[RSP_P_BRANCH]++; break;
        case 0x05: if (r[rs] != r[rt]) { s->delay = 1; s->delay_target = btarget; } s->prof[RSP_P_BRANCH]++; break;
        case 0x06: if ((int32_t)r[rs] <= 0) { s->delay = 1; s->delay_target = btarget; } s->prof[RSP_P_BRANCH]++; break;
        case 0x07: if ((int32_t)r[rs] > 0) { s->delay = 1; s->delay_target = btarget; } s->prof[RSP_P_BRANCH]++; break;
        case 0x08: case 0x09: r[rt] = r[rs] + simm; s->prof[RSP_P_ALU]++; break;
        case 0x0A: r[rt] = (int32_t)r[rs] < (int32_t)simm; s->prof[RSP_P_ALU]++; break;
        case 0x0B: r[rt] = r[rs] < simm; s->prof[RSP_P_ALU]++; break;
        case 0x0C: r[rt] = r[rs] & imm; s->prof[RSP_P_ALU]++; break;
        case 0x0D: r[rt] = r[rs] | imm; s->prof[RSP_P_ALU]++; break;
        case 0x0E: r[rt] = r[rs] ^ imm; s->prof[RSP_P_ALU]++; break;
        case 0x0F: r[rt] = imm << 16; s->prof[RSP_P_ALU]++; break;
        case 0x10:
            if (rs == 0x00) { r[rt] = mfc0(s, rd); s->prof[RSP_P_MFC0]++; }
            else if (rs == 0x04) { mtc0(s, rd, r[rt]); s->prof[RSP_P_MTC0]++; }
            else s->prof[RSP_P_OTHER]++;
            break;
        case 0x12:
            if (in & (1u << 25)) { vec_op(s, in); s->prof[RSP_P_VEC + (in & 63u)]++; s->vec_e[(in >> 21) & 15u]++; }
            else {
                unsigned e = (in >> 7) & 15u;
                switch (rs) {
                case 0x00: r[rt] = (uint32_t)(int32_t)(int16_t)(vb(s->vr[rd], e) << 8 | vb(s->vr[rd], e + 1)); s->prof[RSP_P_MFC2]++; break;
                case 0x04: vbw(s->vr[rd], e, r[rt] >> 8); if (e != 15) vbw(s->vr[rd], e + 1, r[rt]); s->prof[RSP_P_MTC2]++; break;
                case 0x02:
                    r[rt] = (rd & 3u) == 0 ? (uint32_t)(int32_t)(int16_t)s->vco : (rd & 3u) == 1 ? (uint32_t)(int32_t)(int16_t)s->vcc : s->vce;
                    s->prof[RSP_P_CFC2]++; break;
                case 0x06:
                    if ((rd & 3u) == 0) s->vco = (uint16_t)r[rt]; else if ((rd & 3u) == 1) s->vcc = (uint16_t)r[rt]; else s->vce = (uint8_t)r[rt];
                    s->prof[RSP_P_CTC2]++; break;
                default: s->prof[RSP_P_OTHER]++; break;
                }
            }
            break;
        case 0x20: r[rt] = (uint32_t)(int32_t)(int8_t)dm8(s, r[rs] + simm); s->prof[RSP_P_LOAD]++; break;
        case 0x21: r[rt] = (uint32_t)(int32_t)(int16_t)dm16(s, r[rs] + simm); s->prof[RSP_P_LOAD]++; break;
        case 0x23: case 0x27: r[rt] = dm32(s, r[rs] + simm); s->prof[RSP_P_LOAD]++; break;
        case 0x24: r[rt] = dm8(s, r[rs] + simm); s->prof[RSP_P_LOAD]++; break;
        case 0x25: r[rt] = dm16(s, r[rs] + simm); s->prof[RSP_P_LOAD]++; break;
        case 0x28: dmw8(s, r[rs] + simm, r[rt]); s->prof[RSP_P_STORE]++; break;
        case 0x29: dmw16(s, r[rs] + simm, r[rt]); s->prof[RSP_P_STORE]++; break;
        case 0x2B: dmw32(s, r[rs] + simm, r[rt]); s->prof[RSP_P_STORE]++; break;
        case 0x32: vec_load(s, in); s->prof[RSP_P_LWC2 + (rd & 15u)]++; break;
        case 0x3A: vec_store(s, in); s->prof[RSP_P_SWC2 + (rd & 15u)]++; break;
        default: s->prof[RSP_P_OTHER]++; break;
        }
        r[0] = 0;
        s->pc = next;
    }
    return s->status & 1u;                          /* (the microcode set its own halt bit) */
}

const char *rsp_prof_name(int i)
{
    static const char *vec[64] = {
        "VMULF", "VMULU", "VRNDP", "VMULQ", "VMUDL", "VMUDM", "VMUDN", "VMUDH",
        "VMACF", "VMACU", "VRNDN", "VMACQ", "VMADL", "VMADM", "VMADN", "VMADH",
        "VADD", "VSUB", "v12", "VABS", "VADDC", "VSUBC", "v16", "v17",
        "v18", "v19", "v1a", "v1b", "v1c", "VSAR", "v1e", "v1f",
        "VLT", "VEQ", "VNE", "VGE", "VCL", "VCH", "VCR", "VMRG",
        "VAND", "VNAND", "VOR", "VNOR", "VXOR", "VNXOR", "v2e", "v2f",
        "VRCP", "VRCPL", "VRCPH", "VMOV", "VRSQ", "VRSQL", "VRSQH", "VNOP",
        "v38", "v39", "v3a", "v3b", "v3c", "v3d", "v3e", "VNULL" };
    static const char *ld[16] = { "LBV", "LSV", "LLV", "LDV", "LQV", "LRV", "LPV", "LUV", "LHV", "LFV", "LWV", "LTV", "l12", "l13", "l14", "l15" };
    static const char *st[16] = { "SBV", "SSV", "SLV", "SDV", "SQV", "SRV", "SPV", "SUV", "SHV", "SFV", "SWV", "STV", "s12", "s13", "s14", "s15" };
    static const char *rest[] = { "MFC2", "MTC2", "CFC2", "CTC2", "MFC0", "MTC0", "load", "store", "branch", "jump", "alu", "shift", "other" };
    if (i < 64) return vec[i];
    if (i < 80) return ld[i - 64];
    if (i < 96) return st[i - 80];
    return rest[i - 96];
}
