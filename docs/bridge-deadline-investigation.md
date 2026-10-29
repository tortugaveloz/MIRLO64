# Pocket bridge deadline investigation

The Pocket requires read data before the next bridge read strobe. Its public
[bus documentation](https://www.analogue.co/developer/docs/openfpga/bus-communication)
gives no minimum time between successive strobes, so the core cannot claim an
unconditional deadline guarantee from that document.
The experiments below are observations for explicitly chosen traffic, not
proved bounds and not a measurement of a real Pocket.

## Starvation when a DMA already owns the shared port

The bridge and PI/AI/SI share `port_apf`. Once `pa_dma` is set, the port
remains owned by that DMA until it receives an acknowledgement. In the first
bridge-priority implementation the SDRAM port was urgent only when
`wishbone_master_cyc && !pa_dma`. With CPU and RSP urgent continuously, a DMA
already asking for SDRAM could wait indefinitely. A new bridge request could
not use the port either. `sdr_deadline_test` reproduces this using a synthetic
continuously requesting DMA and CPU/RSP requesters. This is an adversarial
traffic model, not evidence that a particular game generated that traffic.

The candidate RTL instructs the arbiter to preempt other bursts for the
shared port whenever the bridge has an outstanding cycle, including while
`pa_dma` owns the port. The DMA can complete and the bridge can then use it.
It also prevents starting a new DMA when bridge work is already visible in
the system-clock FIFO or being processed by the master. This does not stop a
DMA that acquired the port earlier, so the test runs pending protection
without the preemption as a negative control.

## Reproducing the experiments

`sh sim/apf/sdr_deadline_test.sh` exercises the real bridge master, port,
arbiter, SDRAM controller and PSRAM controller. By default it uses the simple
FIFO model intended for CI, which omits the clock-domain crossing delays. Set
`ALTERA_MF` to Intel's `altera_mf.v` for the vendor dual-clock FIFO simulation.
The SDRAM PHY/chip is represented by a fixed `RL=5` read-data pipeline; this
does not validate physical SDRAM data, pin timing, or metastability. The
shared-port DMA is modelled after the RTL's ownership handshake, not by the
full PI, AI and SI engines. CPU and RSP request continuously with changing
rows and banks. The SDRAM controller uses normal refresh and timing values.

With Intel's FIFO model, a 62.5 MHz system clock, a 74.25 MHz bridge clock,
32,768 sequential reads, and the candidate priority:

* Without shared DMA traffic, a read period of 60 bridge clocks passed in
  eight sampled clock phases. The largest observed data latency was 52 bridge
  clocks after the falling edge following the read strobe. A period of 52
  failed three reads in one phase.
* With a continuously requesting synthetic SDRAM DMA and competing urgent
  ports, the previous priority starved the bridge. Giving the DMA priority
  while the bridge waits allowed 32,768 reads to pass at periods of 80, 90
  and 100 clocks. A period of 70 missed ten deadlines in one run before the
  pending-queue protection. With that protection, 32,768 reads passed at a
  period of 80 in eight sampled clock phases; the largest observed latency
  was 69 bridge clocks after the falling edge following the strobe. With the
  final candidate, period 75 passed in one run, but period 70 missed one
  deadline, period 68 missed two, and period 65 missed 83.
* With a continuously requesting DMA routed through the real asynchronous
  PSRAM controller, 32,768 reads passed at periods 65, 70, 75, 80 and 100;
  the largest latency observed across those runs was 63 bridge clocks.
* Under that same PSRAM DMA traffic, 32,768 writes passed at periods 24, 28,
  32 and 40 clocks, while periods 20 and 16 overflowed the four-word write
  FIFO. With the candidate pending protection, periods 10, 12, 14, 16, 18,
  20 and 24 passed in the corresponding prototype; period 10 was repeated on
  the candidate RTL and passed. These are sustained streams, not measurements
  of the Pocket's writes; the full set of periods has not yet been repeated
  against the candidate RTL.

The periods that pass these samples must not be interpreted as a contractual
minimum or a worst-case bound. The real platform's minimum strobe separation,
all possible arbitration/refresh states, startup and shutdown transitions,
and analog synchronizer behaviour remain unverified. Write bursts have a
separate FIFO capacity and throughput limit and require their own analysis.

## Save snapshot consistency

[Analogue documents](https://www.analogue.co/developer/docs/openfpga/core-boot-process)
a reset-enter command before it reads nonvolatile slots at shutdown. MIRLO's
`core_top.sv` intentionally does not reset the SoC on that signal, so ongoing
game or DMA writes may overlap the bridge's save read.
Whether that occurs for any game and how to quiesce safely without disturbing
boot or restart still require investigation. Giving the bridge priority does
not by itself establish that the save is a coherent snapshot.
