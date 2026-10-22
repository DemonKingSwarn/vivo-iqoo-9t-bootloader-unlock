# Device-tree map (iQOO 9T / SM8475 "Cape")

Generated from the committed DTBs with `tools/host/decompile_dtbs.py`; the
decompiled sources live in `DTB/dts/` (12 MB of text) and the raw inventory of
the live tree in `DTB/dts/inventory-live.tsv`.

```bash
tools/host/decompile_dtbs.py      # DTB/*.dtb -> DTB/dts/**, README, inventory
```

## 1. What each blob is

| Group | Files | Meaning |
|---|---|---|
| `DTB/vendor_boot/` | 6 | base SoC trees shipped by ABL (`Cape`, `CapeP`, `Diwali*`, `Waipio*`) |
| `DTB/dtbo/` | 11 | board overlays; two are `Cape QRD with PM8008 PD2217F_EX` (00, 01), the rest are Diwali SKUs |
| `DTB/live/` | 1 | merged tree read from `/sys/firmware/fdt` on the running phone — the authoritative one |

The live tree has 5248 nodes (117 disabled) and is the merge of a `vendor_boot`
Cape base plus a cape dtbo.

**Which overlay is ours:** `01_..._Cape_QRD_with_PM8008_PD2217F_EX`. Only that
one carries the display stack (`qcom,dsi-display`, `novatek,vis-display`, the
`pd2217` panel). Overlay `00` is the same board without the panel. The live
tree contains `qcom,mdss_dsi_pd2217_amb678bj02_s6e3fac_fhdplus_cmd`, so `01`
was applied at boot (`msm_drm.dsi_display0=...::lcm_software_id=0xa9` in the
vendor cmdline agrees).

## 2. Tree layout of the live tree

```
/
├── chosen                  bootargs (vendor), stdout uart@99c000, kaslr
├── reserved-memory         aop cmd-db @0x80860000; splash @0xb8000000 (+0x2b00000);
│                           many dynamic `alloc-ranges` pools (no fixed address)
├── soc
│   ├── qcom,spmi@c42d000   SPMI arbiter -> pmk8350/pm8350/pm8350b/pm8350c/pmr735a/pmr735b/pm8450
│   ├── qcom,mdss_mdp@ae00000, mdss_dsi_ctrl0/1@ae94000/0xae96000, dsi_phy0/1
│   ├── ufshc@1d84000, ufsphy_mem@1d80000, ice @1d88000
│   ├── ssusb@a600000 (dwc-usb3-msm) + snps,dwc3, hsphy@88e3000 (eUSB2), usb_nop_phy
│   ├── qcom,pmic_glink (+ucsi, altmode)        Type-C role/alt-mode via ADSP
│   ├── i2c@990000  gt9916@5d (goodix,gt9897), tcm_i2c@2c (synaptics-3618)
│   ├── i2c@994000  eusb2_repeater@4f (nxp), eusb2_repeater@3e (ti)
│   ├── qup_uart@99c000 (console), @980000/@a98000/@894000 (HS)
│   ├── qcom,cnss-qca6490@b0000000             WiFi/BT (QCA6490, CNSS glue)
│   ├── spf_core_platform/lpass-cdc + wsa/rx/tx/va macros, wcd938x codec
│   └── cameras, IPA, GPU, video, compute, etc. (not needed yet)
└── vendor root nodes: tp_module{1..4}, presskey_module{1,2}, goodix_gf9518,
    fg_common, fuelsummary, cms_v5, vivo_usb, max20328, fan53526_i2c, ...
```

## 3. Port-relevant peripheral map

Status is against our fork (7.2.11): `works` = mainline driver + binding,
`needs dts` = driver exists but node/work is missing, `vendor only` = no
mainline driver yet.

### Storage / USB (milestone 1 — already described in findings §21)

| Device | Node | Compatible / reg | Status |
|---|---|---|---|
| UFS HC | `soc/ufshc@1d84000` | `qcom,ufshc`, 3 reg ranges | works (generic `qcom,ufshc`) |
| UFS PHY | `soc/ufsphy_mem@1d80000` | `qcom,ufs-phy-qmp-v4-cape`, `0x1d80000+0x2000` | works via `qcom,sm8475-qmp-ufs-phy` |
| ICE | `soc/ice@1d88000` | `qcom,sm8450-inline-crypto-engine` | works |
| USB controller | `soc/ssusb@a600000` → `snps,dwc3` | `qcom,dwc-usb3-msm` | works as `usb_1` (peripheral) |
| HS PHY | `soc/hsphy@88e3000` | `qcom,usb-snps-eusb2-phy` | needs vendor compatible (patch in fork) |
| eUSB2 repeaters | `i2c@994000/…@4f`, `…@3e` | `nxp,eusb2-repeater`, `ti,eusb2-repeater` | vendor only (I2C tuning) |
| Type-C / role | `soc/qcom,pmic_glink/qcom,ucsi` | `qcom,ucsi-glink` | needs dts (pmic-glink + ADSP) |

### Display (milestone 2)

| Device | Node | Compatible / reg | Status |
|---|---|---|---|
| MDP / KMS | `soc/qcom,mdss_mdp@ae00000` | `qcom,sde-kms` | needs dts (sm8450 mdss works upstream; SDE version check) |
| DSI ctrl 0/1 | `soc/mdss_dsi_ctrl0@ae94000`, `…1@ae96000` | `qcom,dsi-ctrl-hw-v2.6` | driver exists (`qcom,sm8450-dsi-ctrl`) |
| DSI PHY 0/1 | `soc/mdss_dsi_phy0@ae94900`, `…1@ae96900` | `qcom,dsi-phy-v4.3.2` | driver exists (`qcom,sm8450-dsi-phy-5nm`) |
| Panel | overlay 01, `pd2217_amb678bj02_s6e3fac_fhdplus_cmd` | vendor msm bindings | **vendor only** — needs a mainline DRM panel driver; full init sequences are in `DTB/dts/dtbo/01_*.dts` |
| Backlight | panel-embedded (no separate node found) | — | via panel driver |
| Display rails | pm8008i/pm8008j (`i2c@…`), pm8350c ldo/bob | `qcom,i2c-pmic` | needs dts (pm8008 driver exists upstream) |

The panel is 1080×2400 command-mode, dual DSI capable; the init table is one
long `qcom,mdss-dsi-on-command` blob plus per-region gamma/page switches
(`de` register paging). That blob is directly reusable as a panel driver's
init sequence.

### Touch / input

| Device | Node | Compatible | Status |
|---|---|---|---|
| Touch module wrapper | `soc/tp_module1..4` | `vivo,touch-panel-module` | vendor only (4 SKU variants) |
| Goodix touch | `soc/i2c@990000/gt9916@5d` | `goodix,gt9897` (I2C) | driver exists (`goodix_berlin_i2c`, but its I2C match table only lists `goodix,gt9916`) |
| Synaptics touch | `soc/i2c@990000/tcm_i2c@2c` | `synaptics-3618` | vendor only (alternative SKU) |
| Side "vforce" keys | `soc/presskey_module1/2` | `vivo,vforce-module` | vendor only |
| Fingerprint | `soc/goodix_gf9518` | `goodix,udfingerprint` (SPI) | vendor only |
| Haptics | `pm8350b/hv-haptics@f000` | `qcom,hv-haptics` | vendor only |

Only one of Goodix/Synaptics is populated per SKU; the wrapper node and
`vivo,panel-project-name` in the panel node (`pd2217`) identify the variant.

### Power / charging

| Device | Node | Compatible | Status |
|---|---|---|---|
| PMIC set | `soc/qcom,spmi@c42d000/*` | `qcom,spmi-pmic` | works (`pm8350*`, `pm8450`, `pmk8350`, `pmr735a/b`) |
| Regulators | 44 `rpmh-regulator-*` nodes | `qcom,rpmh-vrm-regulator` / `-arc-regulator` | works (pm8350/c10/l5/l7/l9 already used) |
| Charger | `pm8350b` + `cms_v5` | `vivo,charger-monitor-v5` | vendor only |
| Fuel gauge | `fg_common`, `fuelsummary` | `vivo,fg-common` | vendor only |
| Type-C / PD | pmic-glink/ucsi | `qcom,ucsi-glink` | needs dts |

### Connectivity / audio (later)

| Device | Node | Compatible | Status |
|---|---|---|---|
| WiFi/BT | `soc/qcom,cnss-qca6490@b0000000` | `qcom,cnss-qca6490` | vendor glue; silicon is WCN6855-class → ath11k + firmware + remoteproc work |
| LPASS codec | `soc/spf_core_platform/lpass-cdc` | `qcom,sm8450-lpass-cdc` | driver exists |
| Speaker/headset amps | `wsa-macro`, `rx/tx/va-macro`, `wcd938x` | `qcom,sm8450-lpass-*` | drivers exist |
| USB-C audio switch | `fsa4480` (I2C) | `qcom,fsa4480` | works |
| Cameras | `cam_cci*`, `qcom,cam-sensor0..5`, eeprom/actuator/flash | vendor | vendor only (largest remaining job) |

## 4. SM8475 vs SM8450 deltas found so far

Derived by comparing the live tree with `arch/arm64/boot/dts/qcom/sm8450.dtsi`:

1. **UFS PHY** at `0x1d80000` (size `0x2000`) vs `0x1d87000` (`0x1000`), and it
   is the HS-G4 capable SM8475 programming — already handled in the board DTS.
2. **HS USB PHY** is the SNPS eUSB2 block (`0x88e3000`, `0x154`, eUSB2
   repeaters behind it) rather than Waipio's 7 nm UTMI PHY — handled.
3. **Panel is device-specific** (`pd2217`) and not in any upstream tree.
4. **Touch is I2C Goodix GT9897/GT9916 or Synaptics 3618**, wrapped in
   `vivo,touch-panel-module`; nothing upstream for the wrapper.
5. **WiFi is CNSS/QCA6490**, not a discrete ath11k PCI device; upstream needs
   the WCN6855 remoteproc/PMU path instead of the vendor `cnss` driver.

## 5. Suggested order from here

1. Finish milestone 1 (RAM boot, UFS probe, USB console).
2. UFS root (deploy a rootfs to `userdata` or use a GSI `userdata` image).
3. Display: enable `mdss`/`dsi`/`dsi_phy` in the board DTS and write a panel
   driver from the overlay's init sequences. This also retires the simpledrm
   splash hack.
4. Touch (Goodix first — the driver exists, just needs the right compatible and
   the module rails/reset GPIOs from `tp_module`).
5. Charger/fuel gauge, then Type-C/role switch (pmic-glink), then audio, WiFi.
6. Cameras last.
