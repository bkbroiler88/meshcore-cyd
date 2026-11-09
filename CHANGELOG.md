# Changelog

CYD-specific changes on top of the [meshcore-dev/meshcore](https://github.com/meshcore-dev/meshcore)
snapshot (`81dbae5`, 2026-10-15). Dates are when the change landed on `main`.

## 2026-11-09

- Companion + repeater: **Settings > Orientation** — Landscape, Landscape 180, Portrait, Portrait 180
  on both panels (replaces *Flip display 180*; a flipped board comes up as Landscape 180). One
  touch calibration serves every orientation. Portrait keeps the landscape font and row sizes;
  settings rows go two-line (label above the controls), and the status bar, list names, tiles,
  tab labels and spectrum text are fitted to the narrower width.
- New env `Xiao_nrf52_bkmesh_companion_radio_ble` for the BKBroiler Mesh V2.1 board (XIAO
  nRF52840 + E22-900M30S): GPS on the NFC pads (P0.09 MCU TX, P0.10 MCU RX, 9600), I2C on
  D6 = SDA / D7 = SCL, battery from a MAX17048 gauge (0x36), TX limit 30 dBm (chip at 20 +
  10 dB PA). The E22's TXEN must be tied to its DIO2 or the PA never switches in.
  Images: `dist/xiao_nrf52_bkmesh_v2_companion_ble.uf2` (drag-and-drop) and `_dfu.zip`.
- `dist/` CYD images refreshed.

## 2026-11-05

- Companion: contact list now shows every stored contact (was capped at 64 rows); the store
  stays at 160, the board's DRAM limit with Bluedroid.
- Companion: **Settings > Rotate contacts when full** — evict the oldest non-favourite node
  instead of rejecting new ones (`AUTO_ADD_OVERWRITE_OLDEST`).
- Companion: **Settings > Flip display 180** — NVS-persisted rotation with a separate touch
  calibration slot per orientation.
- Companion: unread tab badge moved to the Contacts-tab corner, clear of the label.
- Companion: offline SD map screen (`MapView`) reading the Cairn tile card format
  (`/Maps/<set>/`, P8RLE / RGB565); vendored `tools/maptiles.py` + `MAPTILES.md` (MIT) to build
  a card. Map is companion-only.
- **Browser flasher** (`flasher/`, ESP Web Tools) for all four images; live at
  `https://bkbroiler88.github.io/meshcore-cyd/flasher/`.
- `dist/` merged images refreshed to match.

## 2026-10-23

- Serial screenshot dump in the firmware + `tools/screenshot.py`; repeater status and settings
  images added to the README.
- Repeater colour UI (`examples/simple_repeater/ui-cyd`): status with airtime budget, neighbours,
  stats, and BOOT-locked settings.
- Companion colour UI (`examples/companion_radio/ui-cyd`): home, contacts, channels, chat with
  on-screen keyboard and delivery ticks, stats, settings; five themes; spectrum analyser;
  finger-sized settings rows; dual-core split (mesh on core 0, UI on core 1).
- Variant `variants/lcdwiki_cyd/` for the 2.8" (ST7789) and 3.5" (ST7796) boards: LovyanGFX panel
  and XPT2046 touch, NVS 4-point calibration, runtime colour inversion.
- PA-gain handling (`LORA_PA_GAIN_DB=10`): the app shows and accepts 30 dBm antenna power while the
  SX1262 is driven at 20.
- GPS on the companion envs (module TX to GPIO 3, 9600 baud; console RX detached).
- Published as an independent snapshot (no upstream history).
