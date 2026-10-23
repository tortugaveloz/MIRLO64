# Boot sweep

`sweep.py` runs every game ROM in a folder through the whole-SoC Verilator bench (`sim/mips_soc/obj_soc/tb_soc` with
`lang/n64rt/build/n64rt.bin`) and judges each boot from the run's own evidence, so compatibility can be checked on a
workstation instead of on the Pocket. Python 3, standard library only.

```
tools/sweep/sweep.py roms/                          # every .z64 / .v64 / .n64 in roms/, 8 console seconds each
tools/sweep/sweep.py roms/ --seconds 4 -j 5 --out build/sweep/quick
tools/sweep/sweep.py roms/game.z64 --seconds 12 --pad 300-320:8000   # one ROM, Start pressed at fields 300..320
```

Build the bench (`sim/mips_soc/build.sh soc`, single-threaded) and the runtime (`make -C lang/n64rt`, README.md has
the whole build) first; on a host without a GCC for MIPS, `tools/docker/` has an image with one (its Dockerfile says
how to build it, and `tools/docker/run.sh <command>` runs a build command in it). `TB_SOC=<path>` and `N64RT=<path>`
name another bench and another runtime.

## Options

| Option | Default | Effect |
|---|---|---|
| `--out <dir>` | `build/sweep/<timestamp>` | where everything goes |
| `--seconds <s>` | 8 | console seconds, passed as `-c` (62.5 M system cycles a second, about 1.04 M per NTSC field), plus 2 M cycles so the frame at the last whole second is dumped too |
| `--frame-every <n>` | 60 | dump every n-th field (`-e`) |
| `-j <n>` | CPU count less 2 | games run at once (each bench is one thread, about 1 M cycles a wall second) |
| `--saves <out>` | none | start each game with the save an earlier sweep left (`<out>/<game>/slot1.bin`, as `SLOT1`), to measure a boot with a valid save |
| `--fb` | off | judge and show the framebuffer dumps instead of the scan-out's video (both are always captured) |
| `--dumps` | off | also `RSPDUMP=<out>/<game>/dumps` for the RSP / RDP lockstep benches (8 MiB a task) |
| `--pad <spec>` | none | controller 1, passed as `PAD=` (`from-to:keys[/joy],...`, the Pocket's key bits: Start is `8000`, A `10`) |
| `--timeout <s>` | `seconds*90+120` | wall seconds a bench may go without progress (`run.txt`, `rsp.log` and the frames not growing) before it is killed; 4 times that in all |
| `--force` | off | re-run games that already have `<out>/<game>/report.json` |
| `--rejudge` | off | recompute the verdicts and reports of games already run, from their files, without the bench (games not run yet are run) |
| `--status <out>` | | print the progress of a running or finished sweep and exit (below) |
| `--compare <base> <cand>` | | compare two sweeps' frames and figures and exit (below) |

Each bench runs with `FB_N64=1 VGA=1 VGAT=1 EXCSTAT=1 FBLOG=1 RCPSTAT=60 RSPLOG=<out>/<game>/rsp.log
SLOT4=<save-type database>` and `VERIFY` unset; everything it prints goes to `run.txt`. `SLOTS_OUT=<out>/<game>` makes
every run leave its save there as `slot1.bin` (the save slot read back as the Pocket does when the core quits), which
`--saves` feeds to a later sweep. No `INTERACT` is given, so the core's settings keep their reset values (the
runtime's defaults: Start on the Pocket's Start, the stick on the D-pad), which are not the Pocket's own defaults from
`interact.json` (there "Start" is Select+Start). Bear that in mind when writing a `--pad` spec.

## Two pictures: the video and the framebuffer

Every run captures both, every `--frame-every`:

- `vNNNNNN.ppm` / `.png`, the video (`VGA=1`): what the scan-out sends to the Pocket's video pads, a pixel per pixel
  clock with DE high, so the picture as the screen gets it, with the crop, the VI's size and filters applied. 640 x 240
  (640 x 480 for an interlaced picture's woven frame). NNNNNN counts the scan-out's frames: 60 a second, 50 once a PAL
  game has set its TV system (`VGAT=1` logs each one's length, from which the harness places it in console time).
- `fNNNNNN.ppm` / `.png`, the framebuffer dump (`FB_N64=1`): 320 x 240 pixels of RGBA5551 read from memory at the
  scan-out's base, at field NNNNNN. It ignores the VI's width and depth, so a game with a 640-wide or 32-bit
  framebuffer comes out garbled or cut, and rows past the picture show whatever memory follows it.

The verdicts and the contact sheet use the video by default; `--fb` makes them use the framebuffer dumps. A run
without any video frame falls back to the framebuffer. Capturing the video costs no measurable time.

A `.v64` (16-bit byte-swapped, first bytes `37 80 40 12`) or `.n64` (word-swapped, `40 12 37 80`) is converted into
`<out>/z64/`; the format is read from the first bytes, not the extension, and the input is never modified.

## Output

- `<out>/settings.json`: the settings and the games, written at the start.
- `<out>/report.json`: the settings (with `git describe` of the tree) and one object per game.
- `<out>/report.md`: game, verdict, first non-black frame, RSP tasks, exceptions a field, the last window's frames a
  second, RSP and RDP busy and cycles a graphics task, wall time, the verdict's reason.
- `<out>/sheet.png`: the last non-black frame of each game scaled down to 160 x 120, numbered as in `report.md`
  (grey: none).
- `<out>/<game>/`: `run.txt` (the bench's whole stdout), `rsp.log`, the frames (`vNNNNNN` and `fNNNNNN`, each as PPM
  and PNG), `slot1.bin` (the save the run left) and `report.json`.

A game with a `report.json` is skipped on the next run of the same `--out`, so an interrupted sweep can be resumed;
one whose verdict is `timeout` (or a harness `error`) is run again, and so is one that Ctrl-C cut short (it
is left without a report).

## Watching a sweep

The sweep prints one line per game as it finishes and, every minute, a count of the games done, running and queued. From
another shell, `tools/sweep/sweep.py --status <out>` reads the folders and prints one line a game: its state (`queued`,
`running`, or `done` and the verdict), the frames dumped so far against those expected, the console seconds simulated so
far (from the last `excstat` line or the last frame, at 50 fields a second for a PAL game), the wall time since its
`run.txt` was created (`-` on a filesystem that does not record it; the run's own time once done) and the last `n64rt:`
line. The game list and settings come from `<out>/settings.json`, written when the sweep starts; for a folder without
one, `--seconds` and `--frame-every` give the expected counts and only the games already started are listed.

## Comparing two sweeps

`tools/sweep/sweep.py --compare <baseline out> <candidate out>` checks an RTL change against a sweep made before it,
for instance a candidate run with `TB_SOC=<the variant bench>` and the same ROMs, `--seconds` and `--frame-every`.
For each game in both it compares every video frame and every framebuffer dump with the same number byte for byte, and
gives the first differing one, how many of its pixels differ and the largest channel difference; then the verdicts side
by side and the candidate's change in FPS, RSP %, RDP % (the last 60-field window), exceptions a field and graphics
tasks. The table is printed and written to `<candidate>/compare.md`, the details to `<candidate>/compare.json`. The
bench is deterministic, and its logging switches do not change the simulation (runs with and without
`VGA`/`FBLOG`/`RCPSTAT` gave identical frames), so a change meant to be cycle-identical must give "identical"
everywhere; a change in behaviour shows where the runs part and by how much.

## Verdicts

Checked in this order; the first that matches wins, and `reason` says which evidence decided it.

| Verdict | Meaning |
|---|---|
| `timeout` | the bench made no progress for `--timeout` seconds, or ran 4 times that, and was killed (the evidence it left is still parsed) |
| `no-boot` | the runtime never printed the game's entry line (`n64rt: <name> -- ROM ... entry <addr>`), or printed a fatal line before it, or the file is not an N64 ROM |
| `crash` | a fatal runtime line after the entry (`n64rt: halted`, an exception the runtime does not handle, ...), a non-zero exit, SDRAM protocol errors, or a frozen picture with a non-interrupt exception code in the final `cause` |
| `black` | no frame dumped, or every dumped frame is near black (fewer than 0.1% of the pixels' channels above 24) |
| `frozen` | the last 3 dumped frames are byte-identical and no RSP task started in that span |
| `alive` | frames change (or stay still while RSP tasks keep coming: `picture_static` in the JSON) |

Whatever the verdict, `reason` ends with `graphics stopped after N gfx tasks (last at field F)` when graphics tasks
stopped before the last two captured frames (audio often goes on: a game waiting on the RSP or the RDP), or with
`blank EEPROM being initialised (graphics paused since field F, S s so far)` when that stop is the pause a game makes
while libultra writes a blank EEPROM at boot: the runtime printed "the cartridge's EEPROM is used (... 0 of them from
the file)", the pause begins within a few fields of that line, and it has lasted no more than 4 s (4 Kbit) or 6 s
(16 Kbit). The writes alone are 64 or 256 blocks at about 14 ms, but the pauses measured around them in the 10 s sweep
are longer (4 Kbit: a platformer 0.8 s, a racer on water 2.7 s, a shooter 3.2 s, a party racer 3.8 s; 16 Kbit: a sports
game 4.8 s), hence those limits. `eeprom_init` in the JSON has the pause for every game with a blank EEPROM. A sweep
with `--saves` has no such pause. Otherwise `reason` can end with
`no graphics task` when frames were dumped but no graphics task ran (a picture the CPU drew, or tasks of another
kind).

The fatal lines are those of `n64rt_fatal()` (which ends with `n64rt: halted`) and the loader's `no_rom()` messages,
plus the words panic, unhandled and unexpected.

## Evidence in `<game>/report.json`

- `entry`, `loader`, `cic`, `n64rt` (the first 40 `n64rt:` lines) and `fatal`.
- `summary`: the bench's last line, `<sys> sys cycles, <frames> frames, <n> SDRAM protocol errors; cpu pc .. cause ..
  epc ..`, parsed, with `exc_code` (the cause's ExcCode by name).
- `excstat`: the `[excstat fields a..b: N exceptions (.. a field) <kind> <n> ...; <n> instructions retired]` lines
  (one every 60 fields) summed: fields, total, per kind, per field, instructions retired.
- `rsp`: tasks from `rsp.log` split by type (`by_type`: OSTask 1 graphics, 2 audio, 3 video, 4 JPEG; any other value is
  an RSP run whose DMEM held no OSTask), `last_gfx_field`, and `per_period` counts between consecutive dumped frames.
  The bench writes one line per RSP run as it halts, nine columns: the system cycle it left halt, the OSTask type (1
  graphics, 2 audio), the task flags in hex (bit 0: resumed after a yield), the cycles it ran, SP_STATUS signals in hex
  as it stopped (bit 0 yield asked, 1 yielded, 2 task done), instructions, cycles with a DMA under way, cycles with RDP
  commands waiting, and the cycles from a yield request to the halt (else -1). A line at cycle 0 is the RSP leaving halt
  at reset and is ignored. Fields are mapped to cycles with the `rcpstat` lines' measured 60-field windows (a PAL game
  boots on NTSC timing for its first 40 or so fields), and with the run's average beyond them.
- `picture` (`video` or `fb`), `frames` (the frames of that picture: index, system cycle, PNG name, size, md5 of the
  pixels, black or not), `video_frames` and `fb_frames` (both kinds), `non_black_frames`, `first_non_black` and
  `first_non_black_s` (console seconds), `tail`.
- `perf`: per 60-field window (`series`, and `last`, the last whole window, which the table shows):
  - `fb_swaps` and `fps`: the frame boundaries at which the scan-out's base changed (`FBLOG=1`, a line
    `[fb frame <field> sys <cycle> base <addr>, <n> frames since the last]` per change, so at most one a field), as
    frames a console second. A move of 4 KiB or less is not counted (an interlaced picture moves its base by a row
    every field). A game that draws into one buffer only shows 0 here; a run without the `rcpstat` lines shows none.
  - `rsp_pct`, `rdp_pct`, `rdp_full_syncs`: from `RCPSTAT=60`, a line every 60 fields,
    `[rcpstat field <f>: <ms> ms; RSP running <ms> ms, RDP busy <ms> ms, <n> full syncs, CPU busy <ms> ms]`.
  - `cpu_pct`: only when `RCPSTAT_IDLE=lo,hi` (the game's idle loop's pc range) is in the environment. The bench
    counts the CPU as idle only inside that range, which differs per game, so without it the CPU column is left empty
    rather than reported as 100%.
  - `gfx`, `audio`, `gfx_mean_cycles`: the RSP log's tasks in the window and the mean cycles a graphics task ran.
- `wall_s`, `exit_code`, `timed_out`, `command`.

## Limits

- The benches share the machine: on a loaded machine (other simulations, fits, builds) each one runs well below
  1 M cycles a second, and the efficiency cores are slower still. The timeout follows progress, not wall time, for
  that reason; keep `-j` at or below the number of fast cores when timing matters.
- 8 console seconds take about 8 to 9 wall minutes a game on an idle machine (4 seconds took 6 to 8 minutes with
  10 to 14 benches running at once). Most games are still in their intro or on the title screen
  by then; `--pad` can press Start to go further.
- `frozen` and `crash` read the picture only every `--frame-every` fields; a short `--frame-every` makes them sharper
  at the cost of disk (225 KiB a PPM).
- The final `cause` register is sampled once; an interrupt pending there is normal and does not count.
