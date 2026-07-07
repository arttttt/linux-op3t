# OnePlus 3T mainline port

Device-enablement patch series that turns a mainline Linux base into a working
**OnePlus 3T (msm8996pro, Snapdragon 821)** headless bot-server kernel.

## Layout
- `patches/` — the series (`git format-patch` output), applied in numeric order.
- `apply.sh` — apply the series onto a mainline base tag (see header for usage).

## Current base
**v6.12.95** (6.12 LTS). Branch `op3t/6.12` = `v6.12.95` + this series;
builds with GCC and boots on device (USB gadget + telnet debug-shell reached).

Why 6.12 and not newer: our own power research found the 6.12 → 7.x kernel jump
brings **zero** msm8996 power changes, while nothing past 6.13 currently boots on
this device (an unresolved early-boot regression lands in the 6.13 → 6.14 window).
So 6.12 LTS is the stable base; the idle-power work is carried as patches/config
on top, not gated on a newer kernel.

## Porting to a new base
    ./op3t-port/apply.sh v6.12.100 op3t/6.12.100
Then resolve any `git am` conflicts from base drift and rebuild. Notes:
- **6.12.x stable is an API mix** — some 6.14 changes are backported (e.g. dwc3
  `usb-psy-name`/EPROBE_DEFER), others are not (e.g. `device_find_child` stays
  non-const). The `fixup(6.12.95)` patch adapts qrtr for exactly that mix; a
  different base may need the fixup adjusted or dropped.
- **macOS**: the kernel tree has case-colliding netfilter headers
  (`xt_mark.h`/`xt_MARK.h`, etc.) that a case-insensitive FS cannot check out.
  Keep them `skip-worktree` and never `git add -A`.

## Deliberately skipped (re-add if a use case appears)
- **dwc3 VBUS / OTG-out** — not needed for an always-plugged server; conflicted
  heavily with the 6.12.95 dwc3 backport.
- **slimbus SSR-notifier reordering** — audio path, unused headless.

## Unused hardware = power off, not ripped out
For idle power we keep hardware described in DT but ensure unused blocks
(camera, sensors, etc.) are power-gated — via config + relying on the kernel's
`clk_disable_unused` / genpd unused-domain gating (so do **not** add
`clk_ignore_unused` / `pd_ignore_unused` to the cmdline). Display is kept.

## Build
pmbootstrap aport at `build/aports/linux-op3t/` in the project repo (GCC,
`config-op3t.aarch64`). The boot image banner must read
`Linux version <base>-msm8996 ... gcc`.
