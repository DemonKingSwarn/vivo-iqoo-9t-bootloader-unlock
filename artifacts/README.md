# artifacts/ — generated, not committed

Local outputs that can be regenerated:

| File | Produced by |
|---|---|
| `devinfo_before.img` | `tools/host/rootctl.py backup-devinfo` (pristine 4096-byte devinfo) |
| `boot*.img`, `init_boot*.img` | `tools/host/pull-stock-boot.sh` |
| `magisk_patched*.img` | Magisk app ("Select and Patch a File") |

Nothing here is required to reproduce the procedure; it is for backups and
restores. Treat `devinfo_before.img` as important if you ever want to re-lock.
