# MIRLO 64

MIRLO 64 is an N64 core made specifically to fit in the Analogue Pocket. It intends to replicate as best as possible the N64 architecture (see [Building the hardware](#building-the-hardware)).

## Installing on the Pocket

1. Download `tortuga.Mirlo64_<version>.zip` from the [releases](https://github.com/tortugaveloz/MIRLO64/releases).
2. Unzip it onto the **root** of the Pocket's SD card, merging with the folders already there. It adds only Mirlo's own files:

   ```
   Assets/mirlo/common/          n64rt.sys (the runtime) and n64savedb.txt; where Mirlo's games go
   Cores/tortuga.Mirlo64/        the core: mirlo.rev (the bitstream), core.json, ...
   Platforms/mirlo.json          the platform, under Console
   Platforms/_images/mirlo.bin   its image
   ```

3. Put the games (games in `.z64` format) in `Assets/mirlo/common/`.
4. On the Pocket: **openFPGA → Console → Nintendo 64**, then pick a game.

Saves (`.sav`) appear in `Saves/mirlo/common/` when you quit a game. To update the core, unzip a newer release the same way; it replaces the files in `Cores/tortuga.Mirlo64/`.

## License

MIRLO 64 is released under the [Apache License 2.0](LICENSE). It includes and builds on third-party work under its own licenses: see [NOTICE](NOTICE).
