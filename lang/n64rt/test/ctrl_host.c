/* ctrl_test.py's host side: n64rt_ctrl.h's presets, record and selection, printed for it to check.
 *   ctrl_host presets            -> "family set0 set1 m1 map0..map4" per family
 *   ctrl_host family ABCD        -> the family of a cartridge ID, or -1
 *   ctrl_host record s0 s1 m0..m4 [damage] -> store, then load back: "1 set0 set1 map0..map4" or "0"
 *   ctrl_host session saved family  -> commands on stdin: pick n s d k, menu s0 s1, eff, cap, save, forget */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../n64rt_ctrl.h"

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "presets")) {
        for (unsigned f = 0; f < N64RT_FAMILIES; f++) {
            n64rt_ctrl_t c;
            n64rt_ctrl_preset(f, &c);
            printf("%u %x %x %x %x %x %x %x %x\n", f, c.set0, c.set1, c.m1, c.map[0], c.map[1], c.map[2], c.map[3], c.map[4]);
        }
    } else if (argc == 4 && !strcmp(argv[1], "session")) {         /* session <saved 0/1> <family>: commands on stdin, one result line per "eff" / "cap" */
        n64rt_ctrl_state_t st;
        memset(&st, 0, sizeof st);
        st.family = atoi(argv[3]);
        if (atoi(argv[2])) { st.saved_ok = 1; st.saved.set0 = 3u << 10; st.saved.m1 = N64RT_CTRL_MASK1; st.saved.map[0] = 0x1234; }
        n64rt_ctrl_pick_t pk = { 0, 0, 0, 0 };
        uint32_t m0 = 0, m1 = 0;
        char cmd[16];
        while (scanf("%15s", cmd) == 1) {
            if (!strcmp(cmd, "pick") && scanf("%u %u %u %u", &pk.normal, &pk.shift, &pk.shift_dpad, &pk.dpad_keys) == 4) { }
            else if (!strcmp(cmd, "menu") && scanf("%x %x", &m0, &m1) == 2) { }
            else if (!strcmp(cmd, "eff")) {
                uint32_t s0 = m0, s1 = m1, map[5];
                n64rt_ctrl_effective(&st, &pk, &s0, &s1, map);
                printf("eff %x %x %x %x %x %x %x\n", s0, s1, map[0], map[1], map[2], map[3], map[4]);
            } else if (!strcmp(cmd, "cap")) {
                n64rt_ctrl_t r;
                n64rt_ctrl_capture(&st, &pk, m0, m1, &r);
                printf("cap %x %x %x %x %x %x %x\n", r.set0, r.set1, r.map[0], r.map[1], r.map[2], r.map[3], r.map[4]);
            } else if (!strcmp(cmd, "save")) { n64rt_ctrl_capture(&st, &pk, m0, m1, &st.saved); st.saved_ok = 1; }
            else if (!strcmp(cmd, "forget")) st.saved_ok = 0;
        }
    } else if (argc == 3 && !strcmp(argv[1], "family")) {
        printf("%d\n", n64rt_ctrl_family((const unsigned char *)argv[2]));
    } else if (argc >= 9 && !strcmp(argv[1], "record")) {
        n64rt_ctrl_t c = { 0 }, d;
        c.set0 = (uint32_t)strtoul(argv[2], 0, 16); c.set1 = (uint32_t)strtoul(argv[3], 0, 16);
        for (int i = 0; i < 5; i++) c.map[i] = (uint32_t)strtoul(argv[4 + i], 0, 16);
        uint32_t mem[16];
        memset(mem, 0xA5, sizeof mem);
        n64rt_ctrl_store(mem, &c);
        if (argc >= 10) mem[(unsigned)atoi(argv[9]) % 9u] ^= 1u << (unsigned)(argc >= 11 ? atoi(argv[10]) : 0);
        if (n64rt_ctrl_load(mem, argc >= 12 ? (unsigned)atoi(argv[11]) : 64u, &d))
            printf("1 %x %x %x %x %x %x %x\n", d.set0, d.set1, d.map[0], d.map[1], d.map[2], d.map[3], d.map[4]);
        else printf("0\n");
    }
    return 0;
}
