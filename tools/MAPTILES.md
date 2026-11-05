# Offline map tiles for Cairn (microSD)

> Works for the L2 Pro, Heltec V4 touch, ThinkNode M9 and T-Deck builds: they all read the same card format. Clone or download this repository and run the commands below from its top folder (the script is [`tools/maptiles.py`](maptiles.py)). References to `examples/…` and `FRAMEWORK.md` are to files in the firmware source tree, which isn't part of this repository; this guide covers everything you need to make a card.

`tools/maptiles.py` turns map tiles into the card format Cairn's **GPS → Map** view reads from microSD,
on the L2 Pro, Heltec V4 touch, ThinkNode M9 and T-Deck. The device code lives in `examples/companion_radio/ui-l2/map/`, and the format is
specified in `examples/companion_radio/ui-l2/FRAMEWORK.md` §16 ("Offline map"). The map works fully
offline: nothing is ever downloaded on the device.

## 1. Setup (once)

The tool needs Python 3.9 or newer and Pillow. numpy is optional but makes conversion 10–20× faster.

```sh
python3 -m venv ~/.venvs/l2tiles && ~/.venvs/l2tiles/bin/pip install Pillow numpy
alias l2tiles='~/.venvs/l2tiles/bin/python3 tools/maptiles.py'     # optional
python3 tools/maptiles.py selftest          # golden vectors; should print "selftest: PASS"
```

Fetching (`fetch`, or `build` from a network source) only needs the standard library. `build`,
`synth` and `estimate --sample` need Pillow.

## 2. Prepare the card

- Use **FAT32**. The device's FAT driver has no exFAT support, and macOS formats cards over 32 GB
  (SDXC) as exFAT by default. An 8–32 GB SDHC card is the recommended size.
- Find the disk number with `diskutil list`, then run the following. **This erases the whole card.**
  ```sh
  diskutil eraseDisk FAT32 CAIRN MBRFormat /dev/diskN
  ```
- The tool never formats anything. When `--out` is a volume root that isn't FAT32, the tool refuses
  and prints this command for you. You can also build into a plain folder and copy the `cairn`
  folder onto the card yourself.
- When you write to a volume root, the tool runs `dot_clean -m`, deletes `._*` files and touches
  `.metadata_never_index`, so Spotlight and Finder leave the card clean.
- Several sets can share one card, up to 8, each in `/Maps/<name>/`. Firmware before October 2026 used `/cairn/maps/<name>/`; the current firmware moves those sets to `/Maps` by itself (renames only). The device picks the set that
  covers the view with the most detail. "Choose map…" in the Map's long-press menu pins one.

## 3. Quick start

Start with a synthetic test grid. It needs no network and no licence, and every tile shows its own
`z/x/y`, its NW corner (lat, lon) and a centre crosshair. If the projection maths is wrong anywhere,
the labels won't line up with your position and the node dots.

```sh
python3 tools/maptiles.py synth --center 35.0,-97.0 --radius-mi 5 --zoom 8-16 --out /Volumes/CAIRN
python3 tools/maptiles.py verify /Volumes/CAIRN/Maps/l2-test
```

Next, build a real map. USGS topo is public domain and needs no key, but it only covers the US:

```sh
python3 tools/maptiles.py estimate --center 35.0,-97.0 --radius-mi 10 --zoom 8-16
python3 tools/maptiles.py build --preset usgs:topo --center 35.0,-97.0 --radius-mi 10 --zoom 8-16 \
    --out /Volumes/CAIRN --name okc-topo --title "OKC topo"
python3 tools/maptiles.py verify /Volumes/CAIRN/Maps/okc-topo
```

`build` prints the estimate and asks `Proceed? [y/N]`; `--yes` skips the question. It then fetches
into the cache, converts, and writes the set atomically: everything goes into
`Maps/.okc-topo.partial/`, which is renamed only at the end. Downloads are resumable. If you hit
Ctrl-C or the request limit, the tool prints the exact command to resume, and tiles already fetched
are never requested again.

Other sources:

```sh
# Geoapify (free key from geoapify.com; OSM data). Their @2x tiles cut in four need 4x fewer requests:
export GEOAPIFY_API_KEY=...            # or --key; never printed or written anywhere
python3 tools/maptiles.py build --preset geoapify:osm-bright --hidpi-split \
    --center 35.0,-97.0 --radius-mi 10 --zoom 8-17 --out /Volumes/CAIRN --name okc-street

# A raster MBTiles file you are allowed to use (its bounds and attribution come from its metadata)
python3 tools/maptiles.py build --mbtiles region.mbtiles --from-mbtiles-bounds --zoom 8-15 \
    --out /Volumes/CAIRN --name region

# Vector (pbf) MBTiles: render them to raster on your own machine first, then fetch from localhost
docker run --rm -p 8080:8080 -v "$PWD":/data maptiler/tileserver-gl --mbtiles region.mbtiles
python3 tools/maptiles.py build --preset "local:http://127.0.0.1:8080/styles/basic-preview/{z}/{x}/{y}.png" \
    --attribution "© OpenMapTiles © OpenStreetMap contributors" --bbox -97.2,34.9,-96.8,35.2 --zoom 8-16 \
    --out /Volumes/CAIRN --name okc-vector
```

## 4. Commands

| Command | What it does |
|---|---|
| `presets` | Lists the sources, their terms URLs, the date the terms were checked (2026-10-01), max zoom, default rate and @2x support, plus the refused hosts. |
| `estimate AREA --zoom A-B [SOURCE] [--format] [--sample K]` | Prints a per-zoom table: tiles, ft/px, miles across the 320 px screen, requests still needed (tiles not yet cached), MB, time, and quota days for Geoapify. `--sample K` converts K tiles per zoom to measure MB instead of guessing, fetching them if they aren't cached. |
| `fetch AREA --zoom A-B [SOURCE] [NET]` | Downloads into the cache only. It's resumable. |
| `build AREA --zoom A-B [SOURCE] [CONVERT] --out PATH --name SLUG` | Fetches what's missing (skip with `--no-fetch`), converts, and writes `PATH/Maps/SLUG/`. `--overwrite` replaces an existing set. `--title` (24 bytes or less) is the name shown on the device. |
| `synth AREA --zoom A-B --out PATH [--name l2-test]` | Writes the offline test grid (16 colours or fewer, attribution "Synthetic test grid"). |
| `verify PATH/Maps/NAME` | Checks the manifest, `cov.*`, every block header and index, and decodes every record. It reports coverage per zoom and prints the name hash (the `map_set` pref). It exits 1 on any error. |
| `selftest` | Runs the golden vectors G1–G3 and R1, the Mercator goldens, block dedupe and the denylist. It exits 0 or 1. |

**AREA:**
- `--center LAT,LON --radius-mi R` gives a square ±R miles around the point.
- `--bbox W,S,E,N` takes decimal degrees.
- `--from-mbtiles-bounds` uses the bounds from the `--mbtiles` file.
- Areas that cross the antimeridian aren't supported.

**Zoom:**
- The maximum is 18, or the source's own maximum if that's lower (USGS stops at 16).
- The device magnifies the deepest level up to 2 more zooms ("overzoom"). For example, a z16 set
  is still usable at z17–18, but it looks blocky.

**Caps:**
- `--max-tiles` defaults to 50,000. The hard ceiling is 250,000.

**NET options:**

| Option | What it does |
|---|---|
| `--rate` | Requests per second. Defaults to the source's rate. |
| `--concurrency` | Parallel requests. Defaults to the source's setting. |
| `--max-requests N` | Stops cleanly after N requests and prints the resume command. For Geoapify the default is 11,000 per day. |
| `--timeout` | Per-request timeout, 20 s by default. |
| `--contact EMAIL` | Added to the User-Agent. |
| `--cache DIR` | Cache location. The default is `~/Library/Caches/l2maptiles/<source>/`. |
| `--retry-missing` | Asks again for tiles the cache remembers as unavailable. |
| `--hidpi-split` | Fetches z−1 @2x tiles and cuts each into 4. |

How responses are handled:
- **Retries** happen after 1, 2, 4, 8 and 16 s (±25%) and honour `Retry-After`.
- **401/403** stops the run: the key is wrong, you're over quota, or the use isn't allowed.
- **404/204/empty responses, and HTML returned with a 200,** are cached as "not available". Add
  `--retry-missing` to ask for them again.
- **Writes** are atomic (`.tmp` then rename), so killing the tool never leaves a half-written tile.

**CONVERT options:**

| Option | What it does |
|---|---|
| `--format p8rle` | The default. Up to 256 colours with run-length rows. A typical topo tile is about 15–30 KB; the worst case is 67 KB. |
| `--format rgb565` | Raw format, 128 KB per tile. It's exact colour, but the card fills 5–8× faster. |
| `--colors N` | Palette size for p8rle (256 or fewer). Tiles that already have 256 or fewer RGB565 colours keep them exactly. Others are quantized (Pillow fast octree). |
| `--filter night` | Inverts lightness and keeps hue: a dark map for night use. |
| `--filter dim:N` | Brightness at N%. |
| `--filter gray` | Greyscale. |
| `--bg auto\|#RRGGBB` | The colour under transparent pixels, and the device's fill for missing tiles. `auto` uses the tiles' most common colour. |

## 5. Sources and licences (checked 2026-10-01)

**The card you build is for your own personal use.** Don't share or sell it: most tile terms
forbid redistributing cached tiles, and Thunderforest and the single-customer plans forbid it
explicitly. Re-check a provider's terms before a big download, because terms change.

| Source | Status | Terms / notes |
|---|---|---|
| `usgs:topo`, `usgs:imagery`, `usgs:shaded` | **Default.** No key. | US Government work, public domain. Please credit "USGS The National Map". US only, max z16, the tool keeps it to 2 req/s. |
| `geoapify:<style>` | Opt-in, free API key | Geoapify's terms allow caching/offline use with attribution. The free plan has a daily credit limit, which the tool tracks per day (`--max-requests`, default 11,000). The map data is © OpenStreetMap contributors (ODbL), and the attribution "Powered by Geoapify \| © OpenMapTiles © OpenStreetMap contributors" is written into the set. |
| `thunderforest:<style>` | Opt-in, needs `--ack-plan thunderforest-bulk` | Bulk/offline use is allowed only on plans that permit it, and never for redistribution. |
| `--mbtiles FILE` | Your own data | You need the right to use it. Raster only; vector (pbf) needs rendering first (see §3). Attribution comes from the file's metadata, or `--attribution`. |
| `local:<template>` | Your own tile server | Only `localhost`, `127.0.0.1` or `::1`, e.g. tileserver-gl rendering your own MBTiles. Needs `--attribution`. |
| `--url TEMPLATE` | Anything else | Use only where the terms allow offline storage. Needs `{z}`, `{x}` and `{y}` (plus `{r}` for `@2x` and `{key}` for a key) and `--attribution`. It runs at 1 req/s. `--y-order tms` is for servers that count rows from the south. |
| tile.openstreetmap.org (and .de/.fr), OpenTopoMap, CARTO (`*.cartocdn.com`), Esri `server.arcgisonline.com`, Google, Bing (`*.virtualearth.net`), MapTiler Cloud, Stadia | **Refused, with no override** (this also applies to `--url` and `local:`) | Their usage policies or terms forbid bulk downloading or offline caching of their tiles. For OSM data, use Geoapify, or render your own vector tiles. |

The attribution shown on the map comes from, in order:
1. `--attribution`;
2. the preset;
3. the MBTiles metadata.

If none of these applies, `build` refuses. The device always shows `attribution_short` in the
map corner, and tapping it shows the full attribution and `license_url`.

## 6. Size and time

These are USGS topo numbers from `estimate` at lat 35°, 2 req/s, with typical tile sizes.
Imagery is about 2–3× bigger.

| Area | Zooms | Tiles | Card size | First download |
|---|---|---|---|---|
| ±5 mi | 8–16 | 1,503 | ~33 MB | ~13 min |
| ±10 mi | 8–16 | 5,728 | ~126 MB | ~50 min |
| ±25 mi | 8–15 | 9,021 | ~200 MB | ~1 h 20 min |
| ±50 mi | 6–14 | 8,865 | ~195 MB | ~1 h 15 min |
| ±100 mi | 6–13 | 8,860 | ~195 MB | ~1 h 15 min |

What one screen (320 px wide) shows at each zoom:

| Zoom | Width |
|---|---|
| z16 | about 0.4 mi (6.4 ft/px) |
| z14 | about 1.6 mi |
| z12 | about 6 mi |
| z10 | about 25 mi |

Each extra zoom level multiplies the tile count by about 4. A practical plan is a wide, shallow set
plus a deep set around home. Both go on the card, and the device picks whichever covers the view
with more detail.

Conversion runs at about 5–15 ms per tile with numpy. A rebuild from the cache (`--no-fetch`)
takes seconds to a few minutes.

## 7. Card format (summary; FRAMEWORK.md §16 is authoritative)

```
/Maps/<name>/manifest.txt                     <name> = [a-z0-9-]{1,24}, the set's identity
/Maps/<name>/<z>/<bx>_<by>.l2t                16x16 tiles per file: bx = tx >> 4, by = ty >> 4 (XYZ, y down)
```

Entries under `/Maps` whose names start with `.` are ignored. That covers partial builds and
AppleDouble files.

**`manifest.txt`:**
- UTF-8, LF line endings, 4096 bytes or less, 255 bytes or less per line.
- Each line is `key=value`; a line starting with `#` is a comment.
- Required keys: `l2map=1`, `format=p8rle|rgb565`, `minzoom`, `maxzoom` (18 or less),
  `bounds=W,S,E,N`, `attribution` (96 bytes or less).
- Optional keys:

| Key | Notes |
|---|---|
| `attribution_short` | 32 bytes or less |
| `title` | 24 bytes or less |
| `license_url` | 64 bytes or less |
| `bg=#RRGGBB` | |
| `dark=0\|1` | |
| `cov.<z>=x0,y0,x1,y1` | Inclusive tile range. The tool always writes it. |
| `center=lon,lat,z` | |
| `source`, `tiles`, `bytes`, `created`, `tool` | Informational |

**`.l2t` block file (little-endian):**

```
0     "L2T1"
4     u8 z    5  u8 fmt (1 = P8RLE, 2 = RGB565)    6  u16 hdr_sectors = 5
8     u32 bx  12 u32 by
16    256 x { u32 sector; u32 bytes }    slot = ((ty & 15) << 4) | (tx & 15); 0/0 = absent
2064  zeros to 2560
2560+ records, each starting at sector * 512; identical records are stored once and shared
```

**P8RLE record:**
- `'P'`, `1`, then `u16 ncolors`.
- Then `ncolors` × RGB565, high byte first.
- Then 256 rows, each a `u16 rowLen` followed by PackBits over palette indices:
  - `c` from `0x00` to `0x7F`: copy the next `c+1` bytes;
  - `c` from `0x80` to `0xFF`: repeat the next byte `c−125` times.
- Each row must decode to exactly 256 indices, every one less than `ncolors`.

The tool always writes the canonical encoding:
- runs of 3 or more become repeats (at most 130 each);
- literals run until the next 3-run, or up to 128 bytes.

**RGB565 record:** 131,072 bytes, 256×256, high byte first.

**Golden vectors** (checked by `selftest`, and by the device's `test/test_l2_maptile`):

| Id | Input row | Encoded |
|---|---|---|
| G1 | 256 × `0` | `04 00 FF 00 FB 00` |
| G2 | `0,1,…,255` | `02 01 7F 00…7F 7F 80…FF` |
| G3 | `5,5,5,7` + 252 × `9` | `08 00 80 05 00 07 FF 09 F7 09` |

For the device side (the SD rail, card detect, loader and serial log lines), see FRAMEWORK.md §16.

## 8. Troubleshooting

- **"exists: pick another --name or add --overwrite"**: the set is already on the card. Rebuilding
  with `--overwrite` swaps it in atomically.
- **Many tiles "not available"**: the area is outside the source's coverage (USGS covers the US
  only), or the zoom is beyond what the source has. If HTML responses were reported, check the key
  and quota, then use `--retry-missing`.
- **HTTP 403 / 401**: wrong key, over quota, or the provider refuses this use. The tool stops at
  once.
- **Interrupted / request limit**: run the printed resume command. For Geoapify's daily limit,
  run it again the next day.
- **To start over for a source**, delete its folder under `~/Library/Caches/l2maptiles/`.
- **The device says "Card isn't FAT32"**: reformat it (§2). "No map for this area" means no set
  covers the view; "Zoom in for map" means you're above the set's `minzoom`.
