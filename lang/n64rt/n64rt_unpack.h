/* The decompressors of libdragon's compressed ELF (its n64elfcompress, levels 1 to 3): the program headers of the ELF in the ROM
 * name a decompression function of the ELF's own (a PT_N64_DECOMP segment of MIPS code) that its IPL3 runs on every segment with the
 * PF_N64_COMPRESSED flag, in place (the compressed bytes are at the segment's vaddr, the program is wanted at its paddr, a margin
 * further down in the stream than the output ever gets). The runtime does not run that code: it knows what the three are by their
 * constants (n64rt_unpack_kind) and does the same in C. Each takes the raw stream (no header: the segment's file bytes), writes at
 * dst at most max bytes, and returns the size of the program, or -1 for a stream it cannot follow (cut, a match that reaches before
 * the start or past max). The output is never written ahead of the input it has not read yet, so src and dst may be the in-place pair.
 * Pure functions of their bytes (no runtime): a host test (test/unpack_host.c) calls them on anything.
 *   1  LZ4's block format (the match offset up to 64 KiB - 1), the last sequence literals only
 *   2  aPLib (apultra: the 8-bit stream's gamma codes, the 4-bit and 7-bit short matches, the end mark 0 in the 7-bit one)
 *   3  Shrinkler (a binary range coder with adaptive contexts over a literal / match / repeat parse), the 32-bit big-endian words
 *      of its bit stream read where the library's own decoder reads them
 * Copyright 2026 tortuga.  SPDX-License-Identifier: Apache-2.0 */
#ifndef N64RT_UNPACK_H
#define N64RT_UNPACK_H

#include <stdint.h>

#define N64RT_UNPACK_LZ4        1
#define N64RT_UNPACK_APLIB      2
#define N64RT_UNPACK_SHRINKLER  3

int n64rt_unpack_lz4(const uint8_t *src, uint32_t size, uint8_t *dst, uint32_t max);
int n64rt_unpack_aplib(const uint8_t *src, uint32_t size, uint8_t *dst, uint32_t max);
int n64rt_unpack_shrinkler(const uint8_t *src, uint32_t size, uint8_t *dst, uint32_t max);
/* which of the three a decompression function is, from the words of its code (big endian, as the ROM has them: stub[i] is the i-th
 * word): the constants of its instructions that no other of them has -- aPLib's 32000 and 1280 (the offsets from which a match has
 * one more byte), Shrinkler's 1026 and 1538 (two of its contexts' places in its frame), and LZ4's 255 (the length's continuation
 * byte) when neither is there. 0: none of the three. */
int n64rt_unpack_kind(const uint32_t *stub, uint32_t nwords);
int n64rt_unpack(int kind, const uint8_t *src, uint32_t size, uint8_t *dst, uint32_t max);

#endif
