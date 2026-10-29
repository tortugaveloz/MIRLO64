# v0.6 memory and audio test

This branch starts at v0.6. It keeps the v0.6 VI and RDP coverage path in
SDRAM at 0x43700000. The first 30 MiB of
ROM are stored at PSRAM byte addresses 0x00200000–0x01ffffff; the remaining
34 MiB are stored in SDRAM at 0x40100000–0x422fffff. Cartridge saves and
the Controller Pak occupy PSRAM, with space reserved for four 32 KiB paks;
only Controller 1 is implemented. The runtime and boot ROM use layout marker
0x4d4c0007 and must be installed together.

The patch also includes the bridge pending/preemption save fix and the AI
8 KiB buffer-boundary carry. The latter passed its targeted queue and stall
tests but did not remove Pilotwings cracking in the preceding Pocket test.
The first three fits using the exact v0.6 floating-point implementation missed
CPU timing by 0.316–0.391 ns. The branch therefore includes the equivalent
FPU datapath simplifications from commit 9dd3a24 for another three-seed fit.
Three synthetic randomized FPU instruction streams of 30,000 instructions
each passed lockstep against the CPU reference model.

With the FPU simplifications, Quartus 25.1std seed 13 passed all 128 reported
timing analyses with a minimum slack of +0.023 ns and used 18,222 ALMs.
Seeds 7 and 2 missed setup by 0.028 and 0.006 ns respectively. Some external
I/O is unconstrained, so this does not establish physical memory pin timing.

Without running any games, synthetic SoC tests exercised CPU, PI DMA and
bridge accesses across the PSRAM bank and 30 MiB split boundaries, the last
word of a 64 MiB ROM, four reserved pak regions and a 128 KiB FlashRAM fixture
with four records, including readback and reload. A synthetic VI antialias
frame matched the earlier reference byte for byte with coverage in SDRAM.
These checks do not guarantee save integrity for every Pocket workload or
establish whether Pilotwings audio is fixed.
