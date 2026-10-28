# Removing D-cache snooping: design and validation

The experimental WT-NWA build defaults to `SNOOP=0`. The
16-byte D-cache writes each CPU store through to memory; it has no dirty data
to write back on eviction. The N64 software still uses `CACHE` instructions
to hand buffers between the CPU and the RCP. Those instructions and the
ordering of the write queue must provide the contract currently assisted by
snooping.

## Contract

* CPU stores update a cached hit and enqueue the same byte-enabled write to
  memory; a miss does not allocate. Do not invent a per-byte dirty indication:
  dirty state on the VR4300 is associated with a cache line. A TLB dirty bit
  instead controls whether a mapped page is writable and must stay intact.
* Decode the actual VR4300 D-cache operations separately, including index
  and hit invalidate, writeback and writeback-invalidate, tag operations and
  create-dirty-exclusive. Define their observable behaviour for a clean,
  write-through cache. In particular, an operation that only writes back must
  not inadvertently discard an unrelated line. Check physical translations,
  tag matching and the extra indices in MIRLO's oversized cache. Study actual
  game/libultra usage before deciding whether tag operations need a fuller
  emulation.
* A writeback or hand-off must make earlier stores visible before an external
  master can read them. `cpu_port` orders stores before a CPU read or device
  access, but `CACHE` itself currently does not drain its store FIFO. Specify
  and test a completion barrier, including crossing both clocks and the
  SDRAM arbiter. Do not assume enqueuing a store means it has reached memory.
* A DMA into RDRAM must be paired with invalidation of destination lines by
  the game or by MIRLO's runtime. For partially covered 16-byte lines,
  preserve adjacent bytes and order prior CPU stores before the DMA. Refill
  during an active DMA can make a line stale again: define the ownership
  boundary and invalidate at completion where software or runtime requires it.
* Audit all writers, not just PI: RSP DMA, RDP, SI, the bridge and runtime
  accesses through uncached aliases. In particular, distinguish game's own
  libultra cache instructions from runtime-created transfers. The existing PI
  path invalidates its destination before starting, which alone is not a
  guarantee if the CPU can refill the destination before completion.
* I-cache maintenance is separate: code written to RDRAM must become visible
  before the appropriate I-cache invalidation and subsequent fetch.

## Implementation and acceptance

First add tests with `SNOOP=0`, without changing the production default. Test
partial stores, hit and index cache operations, larger-cache aliases, KSEG0,
KSEG1 and mapped addresses, FIFO saturation and clock crossing, and transfers
with partial first/last lines. Explicitly test CPU-to-device hand-off and
device-to-CPU hand-off, both with invalidation at the correct boundary and
with a refill while DMA is in progress. Cover each writer, including saves.
The existing `dma_coh_test` assumes automatic invalidation and should remain
as a snoop-enabled regression; add separate software-managed tests rather than
silently changing its expectation.

Then audit runtime-owned buffers and introduce only the missing barriers and
invalidations. Run representative games including SM64, Mario Tennis and the
Pilotwings reproducer, and verify saves on Pocket. Compare three simultaneous
Quartus seeds and the critical paths against the snoop-enabled build. The
new default is experimental until those checks pass; remove snoop RTL and its
arbiter backpressure only once the switch is justified. Games which rely on
MIRLO's previously automatic coherence without performing N64-compatible
cache maintenance remain a compatibility risk and require explicit testing.

## Experiment: a line refilled during PI DMA

`snoop_probe` starts a 1 KiB PI copy and reads the destination's final line
before the DMA reaches it. With the production configuration the read after
DMA completion sees the new value. In an otherwise identical SoC built with
`MIRLO_CPU_SNOOP=0`, it sees the old value until a hit invalidation, after
which it sees the new one. This demonstrates the hazard; it does not prove
that a particular game accesses a buffer while DMA owns it. Both runs
completed without SDRAM protocol errors.

The existing `dma_coh_test` uses aligned 16-byte neighbours. Since the line
size changed from 32 to 16 bytes, those neighbours no longer share the
destination's line; that test by itself no longer exercises this race. Add
separate cases for a genuinely shared partial line and all other DMA writers.

For mapped addresses there is a further point to investigate: a D-cache hit
operation currently selects `m_addr[DW+1:4]`, whereas mapped cached loads
use a physical index. With a 32 KiB D-cache, index bits extend above the page
offset. A mapped virtual and physical page with different index bits can
therefore select different lines. This needs a targeted test and correction
before relying on game cache instructions without snoop.

## Applying the CB-WA contract

The CB-WA branch already disables snooping and assumes the same ownership
rules as the console: software writes back the CPU's source before asking a
device to read it, and invalidates a device destination before the device
writes it. Software must not read or write the destination while the device
owns it; the previous experiment intentionally violates that rule to expose
what snoop currently masks. Partial destination lines need special care:
console software writes back the ends when necessary to preserve bytes
outside the destination. WT-NWA has already sent those stores to its FIFO,
but they must reach SDRAM before the DMA begins. The FIFO orders earlier CPU
stores before the device-register write that starts the DMA; audit every
handoff, including runtime-generated and asynchronous starts, before relying
on this property universally.

The two caches do not have identical instruction implementations. CB-WA
decodes D-cache tag, invalidate and writeback operations and implements a
TagLo path, while WT currently invalidates for every D-cache operation and
does not implement the corresponding D-cache TagLo behaviour. A game may
inspect tags, including when walking the cache, so the CB-WA software
assumption alone does not establish compatibility. For WT, report resident
lines as clean, preserve clean-line semantics for writeback-only operations,
and implement the observable tag operations without adding dirty-data RAM or
writeback machinery. Validate create-dirty-exclusive and partial stores
against software usage before choosing their exact implementation.

Do not assume CB-WA proves mapped cache maintenance is correct: its current
`m_cache_didx` also selects a hit operation using virtual address index bits.
That is particularly relevant for oversized caches whose index extends above
the page offset. A directed mapped-alias test is needed in both branches.

The experimental implementation treats hit writeback as a no-op and reports
an invalid tag on index load tag: write-through software has no dirty data to
find. This is deliberately not full architectural TagLo emulation, and is a
compatibility limitation for code inspecting tags for other purposes. Index
store tag and create-dirty-exclusive conservatively invalidate the selected
line rather than fabricate cache data. Mapped hit invalidate and hit
writeback-invalidate invalidate all possible physical page colours; the new
`mapped_cache_test` exercises both. The test for hit invalidate failed before
the change and passed after it.

As an initial smoke test, the WT SoC with snoop disabled passes
`dcache16_test`, `dma_coh_test`, `rcpcpu_test`, `si_test` and `ai_test`. These tests do
not establish game compatibility: notably the aligned-neighbour DMA test
does not exercise a shared partial cache line.
