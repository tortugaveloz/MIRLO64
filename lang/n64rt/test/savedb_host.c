/* The save-type database's lookup (../n64rt_savedb.c) on the host: its cases, the shipped file against what is known of
 * the games on disk, and bytes at random (an address-sanitised run must find nothing).
 *   gcc -O1 -g -fsanitize=address,undefined -I.. savedb_host.c ../n64rt_savedb.c -o /tmp/savedb_host
 *   /tmp/savedb_host [<n64savedb.txt>]
 * With the file: a "check" line is run for each ID given as argv ("NBKE=2", the type's number from n64rt_savedb.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n64rt_savedb.h"

static int fails;
static int look(const char *db, const char *id, unsigned rev) { return n64rt_savedb_lookup(db, (unsigned)strlen(db), (const unsigned char *)id, rev); }
#define CHECK(db, id, rev, want) do { int g = look(db, id, rev); if (g != (want)) { printf("FAIL line %d: %s %s rev %u: %d, want %d\n", __LINE__, #db, id, rev, g, want); fails++; } } while (0)

int main(int argc, char **argv)
{
    const int E4 = N64RT_SAVEDB_EEP4K, E16 = N64RT_SAVEDB_EEP16K, NO = N64RT_SAVEDB_NONE;
    CHECK("NBK? eeprom16k\n", "NBKE", 0, E16);
    CHECK("NBK? eeprom16k\n", "NBKJ", 0, E16);
    CHECK("NBK? eeprom16k\n", "NB7E", 0, NO);
    CHECK("NBK? eeprom16k", "NBKE", 0, E16);                                  /* no trailing newline */
    CHECK("NBK? eeprom16k\r\nNSM? eeprom4k\r\n", "NSME", 0, E4);              /* CRLF */
    CHECK("NBK? eeprom16k\rNSM? eeprom4k\r", "NSME", 0, E4);                  /* CR only */
    CHECK("# c\n\n  \t NBK?\teeprom16k  # x\n", "NBKE", 0, E16);             /* comments, blanks, tabs */
    CHECK("#NBK? eeprom16k\n", "NBKE", 0, NO);                                /* commented out */
    CHECK("NBKE eeprom16k\nNBK? eeprom4k\n", "NBKE", 0, E16);                 /* the first match wins */
    CHECK("NBKE eeprom16k\nNBK? eeprom4k\n", "NBKP", 0, E4);
    CHECK("NBKE EEPROM16K\n", "NBKE", 0, E16);                                /* any case */
    CHECK("nbke eeprom16k\n", "NBKE", 0, E16);
    CHECK("????  eeprom16k\n", "XYZW", 0, E16);                               /* all wildcards */
    CHECK("NBKE sram\nNBKE eeprom16k\n", "NBKE", 0, N64RT_SAVEDB_SRAM);
    CHECK("NBKE flash\n", "NBKE", 0, N64RT_SAVEDB_FLASH);
    CHECK("NBKE none\n", "NBKE", 0, N64RT_SAVEDB_NOSAVE);
    CHECK("NBKE sram96k\nNBK? eeprom4k\n", "NBKE", 0, E4);                   /* sram96k: a type the runtime does not act on yet: skipped */
    CHECK("NBKE eeprom32k\nNBK? eeprom4k\n", "NBKE", 0, E4);                  /* an unknown type: the line is skipped */
    CHECK("NBKE\nNBK? eeprom4k\n", "NBKE", 0, E4);                            /* no type */
    CHECK("NBKEE eeprom16k\nNBK eeprom16k\n", "NBKE", 0, NO);                 /* not four characters */
    CHECK("NBKE eeprom16kk\n", "NBKE", 0, NO);
    CHECK("NBKE eeprom1\n", "NBKE", 0, NO);
    CHECK("NBKE eeprom16k extra stuff 123\n", "NBKE", 0, E16);                /* fields after the type */
    CHECK("NK4J sram rev=0,1\nNK4J eeprom16k rev=2,3\n", "NK4J", 1, N64RT_SAVEDB_SRAM);
    CHECK("NK4J sram rev=0,1\nNK4J eeprom16k rev=2,3\n", "NK4J", 3, E16);
    CHECK("NK4J sram rev=0,1\nNK4J eeprom16k rev=2,3\n", "NK4J", 4, NO);
    CHECK("NK4J sram rev=0,1\nNK4? eeprom16k\n", "NK4J", 4, E16);             /* falls through to the wildcard */
    CHECK("NK4J eeprom16k rev=\n", "NK4J", 0, NO);                            /* a malformed rev: the line is skipped */
    CHECK("NK4J eeprom16k rev=x\n", "NK4J", 0, NO);
    CHECK("NK4J eeprom16k rev=1,\n", "NK4J", 1, NO);
    CHECK("NK4J eeprom16k rev=,1\n", "NK4J", 1, NO);
    CHECK("NK4J eeprom16k rev=99999999999999\n", "NK4J", 0, NO);
    CHECK("NK4J eeprom16k rev=1 # c\n", "NK4J", 1, E16);
    CHECK("NBKE eeprom16k#c\n", "NBKE", 0, E16);                              /* the comment right after */
    CHECK("NBKE#c eeprom16k\n", "NBKE", 0, NO);
    CHECK("", "NBKE", 0, NO);                                                 /* empty */
    CHECK("\n\n\n", "NBKE", 0, NO);
    CHECK("#", "NBKE", 0, NO);
    CHECK("NBKE eeprom16k\n", "nbke", 0, E16);
    { static const char nul[] = "NBKE eeprom16k\0NSME eeprom4k\nNBKE eeprom4k\n"; /* a NUL in the middle: a line end */
      int g = n64rt_savedb_lookup(nul, sizeof nul - 1, (const unsigned char *)"NSME", 0); if (g != E4) { printf("FAIL NUL: %d\n", g); fails++; } }
    { char big[70000]; memset(big, 'A', sizeof big); int g = n64rt_savedb_lookup(big, sizeof big, (const unsigned char *)"NBKE", 0); if (g) { printf("FAIL big: %d\n", g); fails++; }
      memset(big, ' ', sizeof big); memcpy(big + sizeof big - 20, "NBKE eeprom16k", 14); g = n64rt_savedb_lookup(big, sizeof big, (const unsigned char *)"NBKE", 0); if (g != E16) { printf("FAIL tail: %d\n", g); fails++; }
      /* a file that stops in the middle of a token, with no NUL after it (the length is all there is) */
      static const char cut[] = "NBKE eeprom16kXXXX"; g = n64rt_savedb_lookup(cut, 14, (const unsigned char *)"NBKE", 0); if (g != E16) { printf("FAIL cut: %d\n", g); fails++; } }
    /* the ROM's own homebrew header: "?ED?", the save type in the version byte's high nibble */
#define HDR(id, rev, want) do { int g = n64rt_savedb_header((const unsigned char *)(id), (rev)); if (g != (want)) { printf("FAIL line %d: header %s 0x%02X: %d, want %d\n", __LINE__, id, (unsigned)(rev), g, want); fails++; } } while (0)
    HDR("NEDE", 0x20, E16);                                                   /* (Mario Kart 64 Amped Up) */
    HDR("NEDE", 0x12, E4);                                                    /* the low nibble is flags: RTC, region free, Expansion Pak */
    HDR("NEDE", 0x2F, E16);
    HDR("NEDJ", 0x10, E4);
    HDR("ZEDX", 0x20, E16);                                                   /* any 1st and 4th character */
    HDR("NEDE", 0x00, N64RT_SAVEDB_NOSAVE);
    HDR("NEDE", 0x30, N64RT_SAVEDB_SRAM);
    HDR("NEDE", 0x40, N64RT_SAVEDB_SRAM);
    HDR("NEDE", 0x50, N64RT_SAVEDB_FLASH);
    HDR("NEDE", 0x60, N64RT_SAVEDB_SRAM);
    HDR("NEDE", 0x70, NO);                                                    /* not a type (yet) */
    HDR("NEDE", 0xF0, NO);
    HDR("NEDE", 0x120, E16);                                                  /* (only the byte counts) */
    HDR("NedE", 0x20, NO);                                                    /* "ED" in capitals only */
    HDR("NSME", 0x20, NO);                                                    /* a retail ID: the file's, not the header's */
    HDR("NKTE", 0x20, NO);
    for (unsigned r = 0; r < 256; r++) { unsigned char id[4] = { (unsigned char)r, 'E', 'D', (unsigned char)(255 - r) };
        int g = n64rt_savedb_header(id, r); if (g < NO || g > N64RT_SAVEDB_NOSAVE) { printf("FAIL header range %u: %d\n", r, g); fails++; } }
    /* random bytes, and random lines of the file's own words */
    srand(1);
    static const char *words[] = { "NBK?", "eeprom16k", "eeprom4k", "sram", "rev=1,2", "rev=", "#", "\n", "\r\n", " ", "\t", "??", "NK4J", "x", "\0" };
    for (int n = 0; n < 20000; n++) {
        char buf[300]; unsigned len = (unsigned)(rand() % 300);
        if (n & 1) for (unsigned i = 0; i < len; i++) buf[i] = (char)rand();
        else { len = 0; for (int w = 0; w < 30; w++) { const char *s = words[rand() % 15]; unsigned l = (rand() % 15 == 0) ? 1u : (unsigned)strlen(s); if (len + l < sizeof buf) { memcpy(buf + len, s, l ? l : 1); len += l ? l : 1; } } }
        unsigned char id[4]; for (int k = 0; k < 4; k++) id[k] = (unsigned char)(n & 2 ? rand() : "NBK?E"[rand() % 5]);
        char *heap = malloc(len ? len : 1); memcpy(heap, buf, len);     /* (a heap copy of exactly len bytes: the sanitiser sees any read past it) */
        (void)n64rt_savedb_lookup(heap, len, id, (unsigned)rand());
        free(heap);
    }
    if (argc > 1) {                                       /* the shipped file */
        FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
        static char file[1 << 20]; unsigned n = (unsigned)fread(file, 1, sizeof file, f); fclose(f);
        printf("%s: %u bytes\n", argv[1], n);
        for (int a = 2; a < argc; a++) {
            char id[5] = { 0 }; unsigned want = 0, rev = 0;
            if (sscanf(argv[a], "%4c=%u@%u", id, &want, &rev) < 2) continue;
            int g = n64rt_savedb_lookup(file, n, (const unsigned char *)id, rev);
            printf("  %s rev %u -> %d%s\n", id, rev, g, g == (int)want ? "" : "   MISMATCH");
            if (g != (int)want) fails++;
        }
    }
    printf(fails ? "savedb_host: %d FAILED\n" : "savedb_host: all passed\n", fails);
    return fails != 0;
}
