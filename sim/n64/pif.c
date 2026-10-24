/* The PIF's joybus: the command block the game writes into PIF RAM (SI DMA),
 * run against controller 1 and the cartridge's EEPROM. In the gateware this
 * is a small firmware routine (or state machine) on the same RAM.
 *
 * Block format, per channel: tx length, rx length, tx bytes (the command
 * first), rx bytes (the answer). 0x00 skips a channel, 0xFF is padding,
 * 0xFE ends the block; byte 63 is the PIF's control byte.
 */
#include <stdlib.h>
#include <string.h>

#include "n64.h"

uint64_t n64_pif_reads;
static uint8_t ram(n64_t *n, int i) { return rd8(n->mem.pif_ram, (uint32_t)i); }
static void ram_w(n64_t *n, int i, uint8_t v) { wr8(n->mem.pif_ram, (uint32_t)i, v); }

/* one joybus command on a channel; returns 0, or -1 for "no device" */
static int joybus(n64_t *n, int ch, const uint8_t *tx, int txn, uint8_t *rx, int rxn)
{
    uint8_t cmd = txn ? tx[0] : 0xFF;
    if (ch == 0) {                                   /* controller 1 */
        switch (cmd) {
        case 0x00: case 0xFF:                        /* info: a controller, no pak */
            if (rxn >= 3) { rx[0] = 0x05; rx[1] = 0x00; rx[2] = 0x02; }
            return 0;
        case 0x01:                                   /* buttons, stick x, stick y */
            n64_pif_reads++;
            if (rxn >= 4) {
                rx[0] = (uint8_t)(n->buttons >> 24); rx[1] = (uint8_t)(n->buttons >> 16);
                rx[2] = (uint8_t)(n->buttons >> 8);  rx[3] = (uint8_t)n->buttons;
            }
            return 0;
        default:
            return -1;                               /* paks: none */
        }
    }
    if (ch == 4) {                                   /* the cartridge: EEPROM */
        uint32_t size = n->mem.eeprom_size;
        if (!size) return -1;
        switch (cmd) {
        case 0x00: case 0xFF:
            if (rxn >= 3) { rx[0] = 0x00; rx[1] = size > 512 ? 0xC0 : 0x80; rx[2] = 0x00; }
            return 0;
        case 0x04:                                   /* read 8 bytes */
            if (txn >= 2 && rxn >= 8) memcpy(rx, n->mem.eeprom + (tx[1] * 8u) % size, 8);
            return 0;
        case 0x05:                                   /* write 8 bytes */
            if (txn >= 10) memcpy(n->mem.eeprom + (tx[1] * 8u) % size, tx + 2, 8);
            if (rxn >= 1) rx[0] = 0x00;
            return 0;
        default:
            return -1;
        }
    }
    return -1;
}

void n64_pif_process(n64_t *n)
{
    int i = 0, ch = 0;
    if (getenv("N64_PIF_LOG")) {
        static int k;
        if (k++ < 12 || (k % 200 == 0 && k < 3000)) {
            fprintf(stderr, "PIF:");
            for (int j = 0; j < 64; j++) fprintf(stderr, " %02x", ram(n, j));
            fprintf(stderr, "\n");
        }
    }
    if (ram(n, 63) & 0x02) {                        /* the CIC-6105 challenge: not modelled -- say so */
        static int warned;
        if (!warned++) fprintf(stderr, "PIF: CIC-6105 challenge requested (command byte %02x): not modelled\n", ram(n, 63));
    }
    while (i < 63) {
        uint8_t t = ram(n, i);
        if (t == 0xFE) break;
        if (t == 0xFF || t == 0xFD) { i++; continue; }
        if (t == 0x00) { ch++; i++; continue; }
        if (i + 1 >= 63) break;
        int txn = t & 0x3F, rxn = ram(n, i + 1) & 0x3F;
        if (i + 2 + txn + rxn > 63) break;
        uint8_t tx[64], rx[64];
        for (int k = 0; k < txn; k++) tx[k] = ram(n, i + 2 + k);
        memset(rx, 0, sizeof rx);
        if (joybus(n, ch, tx, txn, rx, rxn) == 0) {
            for (int k = 0; k < rxn; k++) ram_w(n, i + 2 + txn + k, rx[k]);
        } else {
            ram_w(n, i + 1, (uint8_t)(ram(n, i + 1) | 0x80));   /* no response */
        }
        i += 2 + txn + rxn;
        ch++;
    }
    ram_w(n, 63, 0);
}
