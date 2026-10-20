# artifacts/ — generated, not committed

Local outputs that can be regenerated from the device or upstream sources:

| File | Produced by |
|---|---|
| `devinfo_unlocked.img` | `dd` of `/dev/block/by-name/devinfo` as root (unlocked state; see findings §18) |
| `boot.img`, `vendor_boot.img` | `tools/host/pull-stock-boot.sh` (stock, pre-GSI) |
| `magisk_patched*.img` | Magisk app ("Select and Patch a File") |
| `vbmeta.img`, `vbmeta_system.img`, `vbmeta_oem.img`, `vbmeta_vgc.img` | `dd` of the four vbmeta partitions (stock; see findings §18) |
| `crDroid-*.img` | downloaded GSI build, flashed into `system` inside `super` |

Nothing here is required to reproduce the procedure; it is for backups and
restores. Keep `vbmeta.img` (stock byte-exact copy) — it is the safe source for
`fastboot flash vbmeta` (with `--disable-verity --disable-verification` when
booting modified images).
