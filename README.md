# tbs6814-dkms

Standalone Linux driver for the **TBS6814 Quad ISDB-T PCIe card**, built
against the **stock upstream kernel** media core (`dvb-core`). It needs
neither TBS's `media_build` nor their patched kernel media tree.

| PCI | Value |
|---|---|
| Vendor:Device | `544d:6178` (TBS ECP3 FPGA PCIe bridge) |
| Subsystem | `6814:xxxx` |
| Frontends | 4x RAONTECH MTV23x (ISDB-T demod + tuner) on I2C buses 0-3 |

## Modules

| Module | Source | Role |
|---|---|---|
| `tbs6814.ko` | `src/tbsecp3-*.c` | PCIe bridge: IRQ, I2C masters, TS DMA, DVB adapters |
| `mtv23x.ko` | `src/mtv23x.c` | I2C frontend driver, `SYS_ISDBT` only, no firmware needed |

Only kernel dependency: the stock `dvb-core` (plus `i2c-core`/`regmap`).

## Build

```sh
make                # against the running kernel
make KVER=<version> # against another installed kernel's headers
```

## Install (DKMS)

DKMS rebuilds and installs both modules automatically for every new
kernel. On boot, udev loads `tbs6814` from the card's PCI ID, and
`tbs6814` loads `mtv23x` itself.

```sh
sudo apt install dkms linux-headers-generic
make dkms-install      # installs the committed tree (git HEAD)
dkms status tbs6814
```

After pulling new commits, run `make dkms-install` again (bump
`PACKAGE_VERSION` in `dkms.conf` for releases). `make dkms-uninstall`
removes it.

## Provenance

Derived from TBS's open-source driver:

- Repository: <https://github.com/tbsdtv/linux_media>, branch `latest`
- Commit: `2ce787de9f6e81b4294692a45de15596ae2e1322` (2026-07-24)
- Files: `drivers/media/pci/tbsecp3/*`, `drivers/media/dvb-frontends/mtv23x*`

The first commit in this repo is a verbatim copy of those files, so
`git diff <first-commit>` shows every change made here.

### Changes vs. upstream

- Reduced to the TBS6814 only: removed every other board, all other
  frontend/tuner glue, LNB/SEC control, CA (CAM), CI and ASI support
  (`tbsecp3-ca.c`, `tbsecp3-ci.c`, `tbsecp3-asi.c`), and the `swapfe`,
  `ciclock` and `cimode` module parameters.
- Module renamed `tbsecp3` → `tbs6814`, so it can't collide with TBS's
  own build.
- No changes to I2C, DMA or register programming; the TBS6814
  hardware sequence is the same as upstream's.

Upstream's dvb-core adds fields to `struct dvb_frontend_ops` and
`struct dtv_frontend_properties`. Nothing here uses them, so the modules
build against unmodified kernel headers.

## License

The source files carry upstream's "GPL version 3 or later" header and
`MODULE_LICENSE("GPL")`. Both are kept as received.

## Hardware validation

2026-11-24, Ubuntu 26.04.1, kernel `7.0.0-44-generic`, stock `dvb-core`,
built from commit `5d7ce13`, Costa Rica (ISDB-Tb) off-air signal:

- All 4 MTV23x frontends probed (I2C 13-0043, 14-0044, 15-0043,
  16-0044). MSI interrupt in use. `/dev/dvb/adapter0..3` created.
- `dvbv5-scan` over UHF 14-51 (`scan/isdbt-6mhz-uhf.conf`) found 14
  services on 7 multiplexes.
- 4 tuners streaming at the same time (20 s each, 10-14 Mbit/s): no sync
  errors. After the first ~1 s of demod settling: no TEI and no
  continuity errors on video/audio PIDs. The only CC errors are on
  broadcaster data PIDs, and they repeat identically when the same mux is
  recorded on a different tuner alone, so they come from the source.

0.2.0 revalidated after a reboot, as loaded by DKMS/udev at boot: both
modules unload and reload cleanly (`modprobe -r tbs6814 mtv23x`, three
cycles); a 30 s four-tuner `tools/quad-test.sh` run is clean (no sync,
TEI or video/audio CC errors); weak, unlockable channels report clamped
signal values; no kernel warnings.

Open observations:

- The first scan right after module load locked only once and got no
  PAT; signal stats came back as a percentage instead of dBm. A second
  scan worked normally. It didn't reproduce on a later fresh load (the
  DKMS-installed module tuned and streamed on the first try). If it
  recurs, compare against a `dvbv5-scan -v` log.
- After lock, the demod needs ~1-1.5 s to settle; packets in that window
  carry TEI. This is normal: start consuming the stream after
  `FE_HAS_LOCK` plus a short delay, or let the player drop TEI packets.
- dvb-core logs `Frontend requested software zigzag, but didn't set the
  frequency step size` once on first tune. Harmless.

## Scanning

No Costa Rica table exists in `dtv-scan-tables`. Costa Rica uses the
same 6 MHz ISDB-Tb channel plan as Brazil (centre = 473.142857 MHz +
6 MHz x (ch - 14)), so the generic `scan/isdbt-6mhz-uhf.conf` covers it:

```sh
dvbv5-scan -a 0 -o channels.conf scan/isdbt-6mhz-uhf.conf
dvbv5-zap  -a 0 -c channels.conf -r "CANAL 6 HD"
```

Your user needs to be in the `video` group to open `/dev/dvb/*`.

## Fixes vs. upstream (0.2.0)

Each is a separate commit; `git log` has the details.

- Demod I2C clients are released on teardown, after the frontend is
  detached. Upstream had this `#if 0`'d out, leaking a reference on
  `mtv23x` so it could never be unloaded.
- Attach error paths: unbound I2C clients are unregistered, and a probe
  that yields no frontend fails cleanly instead of dereferencing NULL.
- `mtv23x` probe fails when the chip check fails, instead of binding
  without a frontend.
- `mtv23x` relative signal strength is clamped (it underflowed below
  -100 dBm, e.g. `281479271743486.62%`), and a partial OFDM/TMCC lock
  reports `FE_HAS_SIGNAL | FE_HAS_CARRIER` instead of leaving the status
  unset and logging on every poll.
- DMA: explicit 32-bit DMA mask (only `DMA_ADDRL` is programmed), and no
  buffer leak after a partial allocation failure.
- I2C adapter unwind on registration failure no longer deletes the
  adapter that failed to register.
- Teardown order: adapters are detached before the IRQ is freed, and
  pending TS work is flushed before the demux is freed.
- Kernel API: `pci_alloc_irq_vectors()` instead of `pci_enable_msi()`,
  BH workqueue instead of a tasklet, `i2c_algorithm.xfer`, `strscpy()`.
  The IRQ handler returns `IRQ_NONE` when nothing is pending.

## Known issues

- All 8 bridge I2C buses are registered, although the card uses 0-3.
  Harmless; left as upstream programs the hardware.

## Testing

`tools/quad-test.sh [SECONDS] [FREQ0..FREQ3]` records the full TS from
all four tuners at the same time and checks it with `tools/tscheck.py`
(sync, TEI, per-PID continuity). The default frequencies are good
multiplexes at the development site; pass your own elsewhere.
