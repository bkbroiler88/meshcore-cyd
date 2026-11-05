#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Part of Cairn (https://github.com/cvhviz/Cairn), the MeshCore fork. MIT, like the rest of the repository.
"""Prepare offline map tiles for Cairn's map view (L2 Pro, Heltec V4 touch, ThinkNode M9, T-Deck; microSD card).

  python3 tools/maptiles.py presets
  python3 tools/maptiles.py estimate --center 35.0,-97.0 --radius-mi 10 --zoom 8-16
  python3 tools/maptiles.py synth    --center 35.0,-97.0 --radius-mi 5 --zoom 8-15 --out /Volumes/CAIRN
  python3 tools/maptiles.py build    --center 35.0,-97.0 --radius-mi 10 --zoom 8-16 \
                                        --out /Volumes/CAIRN --name okc-topo --title "OKC topo"
  python3 tools/maptiles.py verify   /Volumes/CAIRN/Maps/okc-topo
  python3 tools/maptiles.py selftest

The card layout and the .l2t / P8RLE formats are specified in
examples/companion_radio/ui-l2/FRAMEWORK.md (section 16, "Offline map") and summarised in
tools/MAPTILES.md. In short:

  /Maps/<name>/manifest.txt                     key=value, LF, <= 4096 bytes
  (firmware before October 2026 read /cairn/maps; the current firmware moves that folder to /Maps itself)
  /Maps/<name>/<z>/<bx>_<by>.l2t                16 x 16 tiles per file; bx = tx >> 4, by = ty >> 4

  .l2t (little-endian): "L2T1" u8 z, u8 fmt (1 P8RLE, 2 RGB565), u16 hdr_sectors = 5, u32 bx, u32 by,
        256 x {u32 sector, u32 bytes} (slot = ((ty & 15) << 4) | (tx & 15)), zero pad to 2560,
        then 512-aligned records (identical records are shared).
  P8RLE: 'P', 1, u16 ncolors, ncolors x RGB565 (high byte first), 256 x {u16 rowLen, PackBits}
        PackBits: c 0x00..0x7F = c+1 literal bytes, c 0x80..0xFF = next byte repeated c-125 times.

Needs Pillow for build / synth / verify / estimate --sample (numpy optional, faster). Fetching only
needs the standard library. Licence rules: only sources whose terms allow storing tiles offline are
offered (see `presets`); the big free OSM / CARTO / Esri / Google / Bing / MapTiler / Stadia tile
hosts are refused outright. The card you make is for your own use.
"""
import argparse
import datetime
import gzip
import hashlib
import io
import json
import math
import os
import plistlib
import random
import re
import shlex
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

TOOL = 'maptiles.py 1.0'
UA_BASE = 'CairnMapTiles/1.0 (Cairn personal offline map%s)'
TERMS_CHECKED = '2026-10-01'
PIP_HINT = 'python3 -m venv ~/.venvs/l2tiles && ~/.venvs/l2tiles/bin/pip install Pillow numpy'

# ----------------------------------------------------------------------------------------- format
SECTOR = 512
HDR_SECTORS = 5
BLOCK_HDR_BYTES = HDR_SECTORS * SECTOR          # 2560
BLOCK_INDEX_OFF = 16
FMT_P8RLE, FMT_RGB565, FMT_MVT = 1, 2, 3
FMT_NAMES = {'p8rle': FMT_P8RLE, 'rgb565': FMT_RGB565, 'mvt': FMT_MVT}
RGB565_BYTES = 131072
P8RLE_MAX_BYTES = 69632
MVT_MAX_BYTES = 655360                          # one vector tile as fetched (gzip); the device's read buffer
MANIFEST_MAX = 4096
LINE_MAX = 255
ZOOM_MAX_CARD = 18                              # manifest maxzoom limit (device overzooms +2)
NAME_RE = re.compile(r'^[a-z0-9-]{1,24}$')
DEFAULT_BG = '#202124'

try:
    import numpy as np                           # optional: 10-20x faster colour conversion
except ImportError:                              # pragma: no cover
    np = None

Image = ImageDraw = ImageFont = ImageOps = ImageEnhance = None


class Fail(Exception):
    """A user-facing error: printed without a traceback, exit status 2."""


def need_pil():
    global Image, ImageDraw, ImageFont, ImageOps, ImageEnhance
    if Image is not None:
        return
    try:
        from PIL import Image as _I, ImageDraw as _D, ImageFont as _F, ImageOps as _O, ImageEnhance as _E
    except ImportError:
        raise Fail('this command needs Pillow (numpy is optional but faster):\n  ' + PIP_HINT)
    Image, ImageDraw, ImageFont, ImageOps, ImageEnhance = _I, _D, _F, _O, _E


def blockslot(tx, ty):
    return ((ty & 15) << 4) | (tx & 15)


def fnv1a32(s):
    h = 2166136261
    for b in s.encode('utf-8') if isinstance(s, str) else s:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def packbits_row(row):
    """Canonical PackBits for one row of palette indices (bytes, any length; 256 on the card)."""
    out = bytearray()
    n = len(row)
    i = 0
    while i < n:
        b = row[i]
        j = i + 1
        while j < n and j - i < 130 and row[j] == b:
            j += 1
        r = j - i
        if r >= 3:                                   # repeat: c - 125 = r (3..130)
            out.append(r + 125)
            out.append(b)
            i = j
            continue
        start = i                                    # literal: stop where a 3-run starts or at 128
        i += 1
        while i < n and i - start < 128 and not (i + 2 < n and row[i] == row[i + 1] == row[i + 2]):
            i += 1
        out.append(i - start - 1)
        out += row[start:i]
    return bytes(out)


def unpackbits_row(data, pos, end, ncolors, out):
    """Decode one row (256 indices) from data[pos:end] into bytearray out; returns None or an error."""
    while pos < end:
        c = data[pos]
        pos += 1
        if c < 0x80:
            k = c + 1
            if pos + k > end:
                return 'literal past row end'
            seg = data[pos:pos + k]
            pos += k
        else:
            k = c - 125
            if pos >= end:
                return 'repeat past row end'
            seg = bytes((data[pos],)) * k
            pos += 1
        if len(out) + k > 256:
            return 'row longer than 256'
        if max(seg) >= ncolors:
            return 'index >= ncolors'
        out += seg
    if len(out) != 256:
        return 'row shorter than 256'
    return None


def encode_p8rle(pal565, indices):
    """pal565: list of plain RGB565 ints (<= 256); indices: 65536 bytes row-major."""
    if not 1 <= len(pal565) <= 256:
        raise ValueError('palette size')
    out = bytearray(b'P\x01')
    out += struct.pack('<H', len(pal565))
    for v in pal565:
        out += struct.pack('>H', v)
    for r in range(256):
        enc = packbits_row(indices[r * 256:(r + 1) * 256])
        out += struct.pack('<H', len(enc))
        out += enc
    return bytes(out)


def decode_p8rle(rec):
    """Returns (pal565 list, 65536-byte indices) or raises ValueError (the device's corrupt rules)."""
    if len(rec) < 6 or rec[0] != 0x50 or rec[1] != 1:
        raise ValueError('bad P8RLE header')
    if len(rec) > P8RLE_MAX_BYTES:
        raise ValueError('record larger than %d' % P8RLE_MAX_BYTES)
    nc = struct.unpack_from('<H', rec, 2)[0]
    if not 1 <= nc <= 256:
        raise ValueError('ncolors %d' % nc)
    pos = 4 + 2 * nc
    if pos > len(rec):
        raise ValueError('truncated palette')
    pal = [struct.unpack_from('>H', rec, 4 + 2 * i)[0] for i in range(nc)]
    idx = bytearray()
    for r in range(256):
        if pos + 2 > len(rec):
            raise ValueError('truncated at row %d' % r)
        rl = struct.unpack_from('<H', rec, pos)[0]
        pos += 2
        if pos + rl > len(rec):
            raise ValueError('truncated at row %d' % r)
        row = bytearray()
        err = unpackbits_row(rec, pos, pos + rl, nc, row)
        if err:
            raise ValueError('row %d: %s' % (r, err))
        idx += row
        pos += rl
    if pos != len(rec):
        raise ValueError('%d trailing bytes' % (len(rec) - pos))
    return pal, bytes(idx)


def build_block(z, fmt, bx, by, records):
    """records: {slot: record bytes}. Returns the whole .l2t file (records deduped by SHA-1)."""
    hdr = bytearray(BLOCK_HDR_BYTES)
    hdr[0:4] = b'L2T1'
    struct.pack_into('<BBHII', hdr, 4, z, fmt, HDR_SECTORS, bx, by)
    body = bytearray()
    seen = {}
    sector = HDR_SECTORS
    for slot in sorted(records):
        rec = records[slot]
        h = hashlib.sha1(rec).digest()
        if h in seen:
            sec = seen[h]
        else:
            sec = sector
            seen[h] = sec
            body += rec
            pad = (-len(rec)) % SECTOR
            body += b'\0' * pad
            sector += (len(rec) + pad) // SECTOR
        struct.pack_into('<II', hdr, BLOCK_INDEX_OFF + 8 * slot, sec, len(rec))
    return bytes(hdr) + bytes(body), len(seen)


# -------------------------------------------------------------------------------------- mercator
# Same integer / double math as ui-l2/map/MapMath.h, so tile ranges agree with the device.
MAX_LAT = 85.0511287798


def ud(deg):
    return int(round(deg * 1e6))


def lon_to_x(lon_ud):
    v = (lon_ud + 180000000) % 360000000
    return (v << 32) // 360000000


def lat_to_y(lat_ud):
    lat = lat_ud * 1e-6
    lat = max(-MAX_LAT, min(MAX_LAT, lat))
    s = math.sin(lat * math.pi / 180.0)
    v = (0.5 - math.log((1.0 + s) / (1.0 - s)) / (4.0 * math.pi)) * 4294967296.0
    v = max(0.0, min(4294967295.0, v))
    return int(v)


def x_to_lon(mx):
    return ((mx * 360000000) >> 32) - 180000000


def y_to_lat(my):
    n = math.pi * (1.0 - 2.0 * (my / 4294967296.0))
    return int(round(math.atan(math.sinh(n)) * 180.0 / math.pi * 1e6))


def tile_at(m, z):
    return 0 if z <= 0 else m >> (32 - z)


def metres_per_px(lat, z):
    return 156543.03392 * math.cos(lat * math.pi / 180.0) / float(1 << z)


def tile_range(bounds, z):
    """Inclusive (x0, y0, x1, y1) of the tiles at z touching bounds (W, S, E, N)."""
    w, s, e, n = bounds
    last = (1 << z) - 1
    x0 = 0 if w <= -180 else tile_at(lon_to_x(ud(w)), z)
    x1 = last if e >= 180 else tile_at(max(lon_to_x(ud(e)) - 1, 0), z)
    y0 = tile_at(lat_to_y(ud(n)), z)
    y1 = tile_at(max(lat_to_y(ud(s)) - 1, 0), z)
    x1, y1 = max(x0, min(x1, last)), max(y0, min(y1, last))
    return x0, y0, x1, y1


def tile_bounds(z, x, y):
    """(W, S, E, N) of one tile, degrees."""
    def lat(t):
        return math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * t / float(1 << z)))))
    return (x / float(1 << z) * 360 - 180, lat(y + 1), (x + 1) / float(1 << z) * 360 - 180, lat(y))


# --------------------------------------------------------------------------------------- sources
USGS_TERMS = 'https://www.usgs.gov/information-policy-and-services/copyrights-and-credits'
GEOAPIFY_TERMS = 'https://www.geoapify.com/terms-and-conditions/'
THUNDERFOREST_TERMS = 'https://www.thunderforest.com/terms/'
OPENFREEMAP_TERMS = 'https://openfreemap.org/tos/'
OPENFREEMAP_TILEJSON = 'https://tiles.openfreemap.org/planet'
# OpenFreeMap's terms: an app may download areas for offline use when each download is at most 3,000
# tiles or 50 MB, with at most 4 parallel requests and a User-Agent naming the app. Larger: ask them.
OPENFREEMAP_MAX_TILES = 3000
OPENFREEMAP_MAX_MB = 50
USGS_URL = 'https://basemap.nationalmap.gov/arcgis/rest/services/%s/MapServer/tile/{z}/{y}/{x}'
USGS = {
    'topo':    ('USGSTopo', 'USGS The National Map', 'USGS', 22000, 'USGS topo (US only)'),
    'imagery': ('USGSImageryOnly', 'USGS The National Map: Orthoimagery', 'USGS', 60000,
                'USGS aerial imagery (US only)'),
    'shaded':  ('USGSShadedReliefOnly', 'USGS The National Map: 3DEP shaded relief', 'USGS', 30000,
                'USGS shaded relief (US only)'),
}
GEOAPIFY_STYLES = ('osm-bright', 'osm-bright-grey', 'osm-bright-smooth', 'osm-carto', 'osm-liberty',
                   'klokantech-basic', 'positron', 'dark-matter', 'dark-matter-dark-grey', 'toner')
THUNDERFOREST_STYLES = ('outdoors', 'cycle', 'transport', 'transport-dark', 'landscape', 'atlas',
                        'neighbourhood', 'pioneer', 'spinal-map', 'mobile-atlas')

# Hosts whose terms forbid bulk / offline tile downloads. No override; applies to --url and local:.
DENY = [
    ('tile.openstreetmap.org', 'the OSMF tile usage policy forbids bulk and offline downloading'),
    ('tile.openstreetmap.de', 'a volunteer-run OSM server; bulk downloading is not allowed'),
    ('tile.openstreetmap.fr', 'a volunteer-run OSM server; bulk downloading is not allowed'),
    ('tile.opentopomap.org', 'OpenTopoMap asks people not to bulk-download its tiles'),
    ('cartocdn.com', 'CARTO basemap terms forbid bulk / offline caching without an enterprise licence'),
    ('server.arcgisonline.com', 'Esri terms forbid offline use of its basemaps outside ArcGIS apps'),
    ('google.com', 'Google Maps terms forbid downloading or caching tiles'),
    ('googleapis.com', 'Google Maps terms forbid downloading or caching tiles'),
    ('virtualearth.net', 'Bing Maps terms forbid bulk downloading and offline caching'),
    ('api.maptiler.com', 'MapTiler Cloud terms forbid bulk download / offline caching of its tiles'),
    ('tiles.stadiamaps.com', 'Stadia Maps terms forbid bulk downloading for offline use'),
]


def check_denied(url):
    host = (urllib.parse.urlsplit(url).hostname or '').lower().rstrip('.')
    for d, why in DENY:
        if host == d or host.endswith('.' + d):
            return why
    return None


def redact(url, key):
    return url.replace(key, 'REDACTED') if key else url


class Source(object):
    def __init__(self, sid, label, kind, template=None, max_zoom=18, rate=1.0, concurrency=1,
                 attribution=None, attribution_short=None, license_url=None, terms=None, hidpi=False,
                 key_env=(), needs_key=False, est_bytes=25000, dark=False, tms=False, daily_quota=False,
                 preset=None, vector=False):
        self.sid, self.label, self.kind, self.template = sid, label, kind, template
        self.max_zoom, self.rate, self.concurrency = max_zoom, rate, concurrency
        self.attribution, self.attribution_short, self.license_url = attribution, attribution_short, license_url
        self.terms, self.hidpi, self.key_env, self.needs_key = terms, hidpi, key_env, needs_key
        self.est_bytes, self.dark, self.tms, self.daily_quota = est_bytes, dark, tms, daily_quota
        self.preset = preset or sid
        self.vector = vector            # map data (MVT), stored as fetched and drawn by the device
        self.mb = None                  # sqlite connection for kind == 'mbtiles'
        self.key = None

    def url(self, z, x, y, r=''):
        if self.tms:
            y = (1 << z) - 1 - y
        u = self.template.replace('{z}', str(z)).replace('{x}', str(x)).replace('{y}', str(y))
        u = u.replace('{r}', r)
        return u.replace('{key}', urllib.parse.quote(self.key or '', safe=''))


def preset_table():
    rows = []
    for k, (svc, attr, short, est, label) in USGS.items():
        rows.append(('usgs:' + k, label, 16, 2.0, False, '-', USGS_TERMS))
    rows.append(('geoapify:<style>', 'Geoapify (free key)', 18, 4.0, True, 'GEOAPIFY_API_KEY', GEOAPIFY_TERMS))
    rows.append(('thunderforest:<style>', 'Thunderforest (paid plan)', 18, 2.0, True,
                 'THUNDERFOREST_API_KEY', THUNDERFOREST_TERMS))
    rows.append(('openfreemap', 'OpenFreeMap vector (worldwide)', 14, 8.0, False, '-', OPENFREEMAP_TERMS))
    rows.append(('local:<template>', 'your own server on localhost', 18, 50.0, True, '-', '(yours)'))
    return rows


def make_source(a, need_attr=False):
    """Build the Source from the SOURCE options (a = argparse namespace)."""
    url = getattr(a, 'url', None)
    mbt = getattr(a, 'mbtiles', None)
    preset = getattr(a, 'preset', None)
    if sum(bool(v) for v in (url, mbt, preset)) > 1:
        raise Fail('give only one of --preset, --url, --mbtiles')
    if mbt:
        src = open_mbtiles(mbt)
    elif url:
        src = template_source('url', url, rate=1.0, concurrency=1)
        if getattr(a, 'y_order', 'xyz') == 'tms':
            src.tms = True
    else:
        src = preset_source(preset or 'usgs:topo', a)
    # attribution: --attribution, else preset, else MBTiles metadata, else refuse (build)
    if getattr(a, 'attribution', None):
        src.attribution = a.attribution
    if getattr(a, 'attribution_short', None):
        src.attribution_short = a.attribution_short
    if getattr(a, 'license_url', None):
        src.license_url = a.license_url
    if need_attr and not src.attribution:
        raise Fail('no attribution known for this source: pass --attribution "..." (the map data\'s '
                   'required credit, e.g. "© OpenStreetMap contributors")')
    # key
    if src.needs_key:
        key = getattr(a, 'key', None)
        for env in src.key_env + ('L2TILES_KEY',):
            key = key or os.environ.get(env)
        if not key and '{key}' in (src.template or ''):
            raise Fail('%s needs an API key: --key, or set %s' % (src.label, ' / '.join(src.key_env + ('L2TILES_KEY',))))
        src.key = key
    if getattr(a, 'hidpi_split', False):
        if not src.hidpi:
            raise Fail('--hidpi-split needs a source with @2x tiles (geoapify:, thunderforest:, or a '
                       'template containing {r})')
        src.sid += '@2x'
    return src


def openfreemap_template():
    """The current planet build's tile URL, from OpenFreeMap's TileJSON (the build date is in the path)."""
    req = urllib.request.Request(OPENFREEMAP_TILEJSON, headers={'User-Agent': UA_BASE % ''})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            tj = json.loads(resp.read().decode('utf-8'))
        tpl = tj['tiles'][0]
    except Exception as e:
        raise Fail('could not read OpenFreeMap\'s tile list (%s): %s' % (OPENFREEMAP_TILEJSON, e))
    if not tpl.startswith('https://tiles.openfreemap.org/') or '{z}' not in tpl:
        raise Fail('unexpected OpenFreeMap tile URL %r' % tpl)
    return tpl


def preset_source(p, a):
    kind, _, arg = p.partition(':')
    if kind == 'openfreemap':
        return Source('openfreemap', 'OpenFreeMap (vector)', 'http', openfreemap_template(), max_zoom=14,
                      rate=8.0, concurrency=4,
                      attribution='OpenFreeMap © OpenMapTiles © OpenStreetMap contributors',
                      attribution_short='© OpenStreetMap', license_url='openstreetmap.org/copyright',
                      terms=OPENFREEMAP_TERMS, est_bytes=30000, preset='openfreemap', vector=True)
    if kind == 'usgs':
        if arg not in USGS:
            raise Fail('unknown preset %r: usgs:topo, usgs:imagery or usgs:shaded' % p)
        svc, attr, short, est, label = USGS[arg]
        return Source('usgs-' + arg, label, 'http', USGS_URL % svc, max_zoom=16, rate=2.0, concurrency=1,
                      attribution=attr, attribution_short=short, license_url='usgs.gov/information-policy-and-services',
                      terms=USGS_TERMS, est_bytes=est, preset=p)
    if kind == 'geoapify':
        if not re.match(r'^[a-z0-9-]+$', arg):
            raise Fail('geoapify:<style>, e.g. geoapify:osm-bright (styles: %s)' % ', '.join(GEOAPIFY_STYLES))
        return Source('geoapify-' + arg, 'Geoapify ' + arg, 'http',
                      'https://maps.geoapify.com/v1/tile/%s/{z}/{x}/{y}{r}.png?apiKey={key}' % arg,
                      max_zoom=18, rate=4.0, concurrency=2,
                      attribution='Powered by Geoapify | © OpenMapTiles © OpenStreetMap contributors',
                      attribution_short='Geoapify © OpenStreetMap', license_url='openstreetmap.org/copyright',
                      terms=GEOAPIFY_TERMS, hidpi=True, key_env=('GEOAPIFY_API_KEY',), needs_key=True,
                      est_bytes=24000, dark='dark' in arg, daily_quota=True, preset=p)
    if kind == 'thunderforest':
        if not re.match(r'^[a-z0-9-]+$', arg):
            raise Fail('thunderforest:<style>, e.g. thunderforest:outdoors (styles: %s)' % ', '.join(THUNDERFOREST_STYLES))
        if getattr(a, 'ack_plan', None) != 'thunderforest-bulk':
            raise Fail('Thunderforest only allows bulk / offline downloads on a plan that permits them, and '
                       'never for redistribution. If your plan allows it, add --ack-plan thunderforest-bulk.')
        return Source('thunderforest-' + arg, 'Thunderforest ' + arg, 'http',
                      'https://tile.thunderforest.com/%s/{z}/{x}/{y}{r}.png?apikey={key}' % arg,
                      max_zoom=18, rate=2.0, concurrency=2,
                      attribution='Maps © Thunderforest, Data © OpenStreetMap contributors',
                      attribution_short='Thunderforest © OpenStreetMap', license_url='openstreetmap.org/copyright',
                      terms=THUNDERFOREST_TERMS, hidpi=True, key_env=('THUNDERFOREST_API_KEY',), needs_key=True,
                      est_bytes=24000, dark='dark' in arg, preset=p)
    if kind == 'local':
        host = (urllib.parse.urlsplit(arg).hostname or '').lower()
        if host not in ('localhost', '127.0.0.1', '::1'):
            raise Fail('local:<template> must point at localhost / 127.0.0.1 / ::1 (got host %r); use --url '
                       'for other servers' % host)
        return template_source('local', arg, rate=50.0, concurrency=4, preset='local')
    raise Fail('unknown preset %r (see `presets`)' % p)


def template_source(kind, tpl, rate, concurrency, preset=None):
    parts = urllib.parse.urlsplit(tpl)
    if parts.scheme not in ('http', 'https') or not parts.hostname:
        raise Fail('tile URL template must be http(s)://host/...{z}...{x}...{y}...')
    for ph in ('{z}', '{x}', '{y}'):
        if ph not in tpl:
            raise Fail('tile URL template needs {z}, {x} and {y} (missing %s)' % ph)
    why = check_denied(tpl)
    if why:
        raise Fail('refusing %s: %s.\nUse --preset usgs:topo (public domain, US) or --preset geoapify:<style> '
                   '(free key, allows offline use with attribution) instead.' % (parts.hostname, why))
    h = hashlib.sha1(tpl.encode()).hexdigest()[:10]
    src = Source('%s-%s' % (kind, h), '%s %s' % (kind, parts.hostname), 'http', tpl, max_zoom=18,
                 rate=rate, concurrency=concurrency, hidpi='{r}' in tpl, preset=preset or 'url')
    if '{key}' in tpl:
        src.needs_key = True
    return src


def open_mbtiles(path):
    if not os.path.isfile(path):
        raise Fail('no such MBTiles file: %s' % path)
    con = sqlite3.connect('file:%s?mode=ro' % urllib.parse.quote(os.path.abspath(path)), uri=True,
                          check_same_thread=False)
    try:
        meta = dict(con.execute('SELECT name, value FROM metadata').fetchall())
    except sqlite3.Error as e:
        raise Fail('%s is not an MBTiles file (%s)' % (path, e))
    fmt = (meta.get('format') or '').lower()
    row = con.execute('SELECT tile_data FROM tiles LIMIT 1').fetchone()
    gz = row is not None and bytes(row[0][:2]) == b'\x1f\x8b'
    if fmt in ('pbf', 'mvt') or gz:
        raise Fail('%s holds vector tiles (pbf); the device needs raster. Render them first, e.g.\n'
                   '  docker run --rm -p 8080:8080 -v "$PWD":/data maptiler/tileserver-gl --mbtiles %s\n'
                   'then: --preset "local:http://127.0.0.1:8080/styles/<style>/{z}/{x}/{y}.png" '
                   '--attribution "<the data\'s credit>"' % (path, os.path.basename(path)))
    zmax = meta.get('maxzoom')
    if zmax is None:
        zmax = con.execute('SELECT MAX(zoom_level) FROM tiles').fetchone()[0] or 0
    attr = meta.get('attribution')
    if attr:
        attr = re.sub(r'<[^>]*>', '', attr).replace('&copy;', '©').replace('&amp;', '&')
        attr = re.sub(r'\s+', ' ', attr).strip() or None
    src = Source('mbtiles-' + hashlib.sha1(os.path.abspath(path).encode()).hexdigest()[:10],
                 'MBTiles ' + os.path.basename(path), 'mbtiles', max_zoom=min(int(zmax), ZOOM_MAX_CARD),
                 attribution=attr, est_bytes=25000, preset='mbtiles:' + os.path.basename(path))
    src.mb = con
    src.mb_lock = threading.Lock()
    src.mb_bounds = None
    if meta.get('bounds'):
        try:
            b = tuple(float(v) for v in meta['bounds'].split(','))
            if len(b) == 4:
                src.mb_bounds = b
        except ValueError:
            pass
    return src


def synth_source():
    return Source('synth', 'synthetic test grid', 'synth', max_zoom=ZOOM_MAX_CARD,
                  attribution='Synthetic test grid', attribution_short='Synthetic test grid',
                  est_bytes=2500, dark=True, preset='synth')


# ------------------------------------------------------------------------------------- tile cache
def default_cache_root():
    if sys.platform == 'darwin':
        return os.path.expanduser('~/Library/Caches/l2maptiles')
    return os.path.join(os.environ.get('XDG_CACHE_HOME', os.path.expanduser('~/.cache')), 'l2maptiles')


def is_vector(data):
    """A vector tile as fetched: gzip, or a bare protobuf (never an HTML / JSON error page)."""
    return data[:2] == b'\x1f\x8b' or data.lstrip()[:1] not in (b'<', b'{')


def is_image(data):
    return (data[:8] == b'\x89PNG\r\n\x1a\n' or data[:3] == b'\xff\xd8\xff' or data[:4] == b'GIF8'
            or (data[:4] == b'RIFF' and data[8:12] == b'WEBP'))


class Cache(object):
    def __init__(self, root, src, retry_missing=False):
        self.dir = os.path.join(root or default_cache_root(), src.sid)
        self.retry_missing = retry_missing

    def path(self, z, x, y):
        return os.path.join(self.dir, str(z), str(x), '%d.tile' % y)

    def has(self, z, x, y):
        p = self.path(z, x, y)
        return os.path.exists(p) or (not self.retry_missing and os.path.exists(p[:-5] + '.missing'))

    def get(self, z, x, y):
        try:
            with open(self.path(z, x, y), 'rb') as f:
                return f.read()
        except IOError:
            return None

    def put(self, z, x, y, data):
        p = self.path(z, x, y) if data is not None else self.path(z, x, y)[:-5] + '.missing'
        d = os.path.dirname(p)
        if not os.path.isdir(d):
            os.makedirs(d, exist_ok=True)
        tmp = '%s.%d.%d.tmp' % (p, os.getpid(), threading.get_ident())
        with open(tmp, 'wb') as f:
            f.write(data or b'')
        os.replace(tmp, p)

    def state_path(self):
        return os.path.join(self.dir, 'state.json')

    def load_state(self):
        try:
            with open(self.state_path()) as f:
                return json.load(f)
        except (IOError, ValueError):
            return {}

    def save_state(self, st):
        if not os.path.isdir(self.dir):
            os.makedirs(self.dir, exist_ok=True)
        tmp = self.state_path() + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(st, f, indent=1, sort_keys=True)
        os.replace(tmp, self.state_path())


def fetch_keys(src, tiles):
    """The (z, x, y) the network must deliver for these card tiles (parents @2x for --hidpi-split)."""
    if src.sid.endswith('@2x'):
        seen = set()
        out = []
        for z, x, y in tiles:
            k = (z - 1, x >> 1, y >> 1)
            if k not in seen:
                seen.add(k)
                out.append(k)
        return out
    return list(tiles)


class Stopped(Exception):
    pass


class Fetcher(object):
    RETRY = (1, 2, 4, 8, 16)

    def __init__(self, src, cache, a):
        self.src, self.cache = src, cache
        self.rate = a.rate or src.rate
        self.conc = max(1, a.concurrency or src.concurrency)
        if src.vector:
            self.conc = min(self.conc, 4)           # OpenFreeMap: at most 4 parallel requests
        self.timeout = a.timeout
        contact = getattr(a, 'contact', None)
        self.ua = UA_BASE % ('; ' + contact if contact else '')
        self.max_requests = a.max_requests if a.max_requests is not None else (11000 if src.daily_quota else None)
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.next_t = 0.0
        self.n = dict(requests=0, fetched=0, missing=0, html=0, failed=0, retries=0)
        self.abort = None
        self.budget_hit = False
        self.interrupted = False
        self.state = cache.load_state()
        today = datetime.date.today().isoformat()
        if self.state.get('day') != today:
            self.state['day'] = today
            self.state['requests_today'] = 0
        self.r_flag = '@2x' if src.sid.endswith('@2x') else ''

    def budget_left(self):
        if self.max_requests is None:
            return None
        used = self.state.get('requests_today', 0) if self.src.daily_quota else self.n['requests']
        return self.max_requests - used

    def take(self):
        with self.lock:
            left = self.budget_left()
            if left is not None and left <= 0:             # this worker retires; the others finish
                self.budget_hit = True
                return False
            self.n['requests'] += 1
            self.state['requests_today'] = self.state.get('requests_today', 0) + 1
            self.state['total_requests'] = self.state.get('total_requests', 0) + 1
            t = max(time.monotonic(), self.next_t)
            self.next_t = t + 1.0 / self.rate
        wait = t - time.monotonic()
        if wait > 0 and self.stop.wait(wait):
            with self.lock:                                   # stopped before sending: not a request
                self.n['requests'] -= 1
                self.state['requests_today'] -= 1
                self.state['total_requests'] -= 1
            raise Stopped()
        return True

    def one(self, z, x, y):
        url = self.src.url(z, x, y, self.r_flag)
        for attempt in range(len(self.RETRY) + 1):
            if self.stop.is_set() or not self.take():
                raise Stopped()
            delay = None
            data = status = None
            try:
                hdrs = {'User-Agent': self.ua, 'Accept': 'image/*'}
                if self.src.vector:             # kept gzipped: that is how the card stores it
                    hdrs = {'User-Agent': self.ua, 'Accept': 'application/x-protobuf, */*', 'Accept-Encoding': 'gzip'}
                req = urllib.request.Request(url, headers=hdrs)
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    data = resp.read()
                    status = resp.status
            except urllib.error.HTTPError as e:
                if e.code in (401, 403):
                    self.abort = ('HTTP %d from %s: the key is wrong, over quota, or this use is not '
                                  'allowed. Stopping.' % (e.code, redact(url, self.src.key)))
                    self.stop.set()
                    raise Stopped()
                if e.code in (408, 429) or e.code >= 500:
                    ra = e.headers.get('Retry-After') if e.headers else None
                    if ra and ra.strip().isdigit():
                        delay = min(float(ra.strip()), 300.0)
                else:
                    return self.done(z, x, y, None, 'missing')          # 404 / 410 / other 4xx
            except (urllib.error.URLError, socket.timeout, ConnectionError, OSError):
                pass
            if status is not None:
                if status == 204 or not data:
                    return self.done(z, x, y, None, 'missing')
                if not (is_vector(data) if self.src.vector else is_image(data)):
                    return self.done(z, x, y, None, 'html')
                return self.done(z, x, y, data, 'fetched')
            if attempt == len(self.RETRY):
                break
            if delay is None:
                delay = self.RETRY[attempt] * random.uniform(0.75, 1.25)
            with self.lock:
                self.n['retries'] += 1
            if self.stop.wait(delay):
                raise Stopped()
        with self.lock:
            self.n['failed'] += 1
        return False

    def done(self, z, x, y, data, what):
        self.cache.put(z, x, y, data)
        with self.lock:
            self.n[what] += 1
        return True

    def run(self, keys, label='fetch'):
        todo = [k for k in keys if not self.cache.has(*k)]
        cached = len(keys) - len(todo)
        it = iter(todo)
        it_lock = threading.Lock()
        prog = Progress(len(todo), label)

        def worker():
            while not self.stop.is_set():
                with it_lock:
                    k = next(it, None)
                if k is None:
                    return
                try:
                    self.one(*k)
                except Stopped:
                    return
                prog.tick()
        threads = [threading.Thread(target=worker, daemon=True) for _ in range(self.conc)]
        for t in threads:
            t.start()
        try:
            while any(t.is_alive() for t in threads):
                for t in threads:
                    t.join(0.25)
        except KeyboardInterrupt:
            self.interrupted = True
            self.stop.set()
            for t in threads:
                t.join(2.0)
        prog.end()
        self.state.update(source=self.src.preset, template=redact(self.src.template or '', self.src.key),
                          updated=datetime.datetime.now().isoformat(timespec='seconds'))
        self.cache.save_state(self.state)
        left = len([k for k in todo if not self.cache.has(*k)])
        log('%s: %d cached already, %d fetched, %d not available (404/empty), %d HTML, %d failed, '
            '%d requests (%d retries)%s' % (label, cached, self.n['fetched'], self.n['missing'], self.n['html'],
                                            self.n['failed'], self.n['requests'], self.n['retries'],
                                            ', %d still to fetch' % left if left else ''))
        if self.n['html']:
            log('warning: %d responses were not images (HTML / error pages): check the URL and key; they are '
                'cached as not available, so re-run with --retry-missing once fixed' % self.n['html'])
        return left


def resume_cmd():
    out = []
    skip = False
    for v in sys.argv[1:]:
        if skip:
            out.append('REDACTED')
            skip = False
            continue
        if v == '--key':
            skip = True
        elif v.startswith('--key='):
            v = '--key=REDACTED'
        out.append(v)
    return 'python3 %s %s' % (shlex.quote(os.path.relpath(sys.argv[0])), ' '.join(shlex.quote(v) for v in out))


# --------------------------------------------------------------------------------------- helpers
def log(msg):
    sys.stderr.write(msg + '\n')
    sys.stderr.flush()


class Progress(object):
    def __init__(self, total, label):
        self.total, self.label, self.n, self.t0 = total, label, 0, time.time()
        self.last = 0.0
        self.tty = sys.stderr.isatty()
        self.lock = threading.Lock()

    def tick(self, k=1):
        with self.lock:
            self.n += k
            now = time.time()
            if now - self.last < (0.25 if self.tty else 10.0) and self.n < self.total:
                return
            self.last = now
            el = now - self.t0
            eta = el / self.n * (self.total - self.n) if self.n else 0
            msg = '%s %d/%d  %.0f%%  %s elapsed, %s left' % (self.label, self.n, self.total,
                                                               100.0 * self.n / max(1, self.total),
                                                               fmt_time(el), fmt_time(eta))
            if self.tty:
                sys.stderr.write('\r' + msg + '   ')
            else:
                sys.stderr.write(msg + '\n')
            sys.stderr.flush()

    def end(self):
        if self.tty and self.n:
            sys.stderr.write('\n')


def fmt_time(s):
    s = int(s + 0.5)
    if s < 60:
        return '%ds' % s
    if s < 3600:
        return '%dm%02ds' % (s // 60, s % 60)
    return '%dh%02dm' % (s // 3600, (s % 3600) // 60)


def parse_latlon(s):
    try:
        lat, lon = (float(v) for v in s.split(','))
    except ValueError:
        raise Fail('--center wants LAT,LON in decimal degrees, e.g. 35.0,-97.0')
    if not (-85 <= lat <= 85 and -180 <= lon <= 180):
        raise Fail('--center out of range')
    return lat, lon


def parse_area(a, src=None):
    """Returns ((W, S, E, N), (lat, lon) centre or None)."""
    given = [bool(a.center), bool(a.bbox), bool(a.from_mbtiles_bounds)]
    if sum(given) != 1:
        raise Fail('give one area: --center LAT,LON --radius-mi R | --bbox W,S,E,N | --from-mbtiles-bounds')
    if a.center:
        lat, lon = parse_latlon(a.center)
        if not a.radius_mi or a.radius_mi <= 0:
            raise Fail('--center needs --radius-mi R (miles)')
        dlat = a.radius_mi / 69.0
        dlon = a.radius_mi / (69.17 * math.cos(math.radians(lat)))
        b = (max(-180.0, lon - dlon), max(-MAX_LAT, lat - dlat), min(180.0, lon + dlon), min(MAX_LAT, lat + dlat))
        return b, (lat, lon)
    if a.bbox:
        try:
            b = tuple(float(v) for v in a.bbox.split(','))
            assert len(b) == 4
        except (ValueError, AssertionError):
            raise Fail('--bbox wants W,S,E,N in decimal degrees')
    else:
        if src is None or src.kind != 'mbtiles' or not src.mb_bounds:
            raise Fail('--from-mbtiles-bounds needs --mbtiles PATH whose metadata has bounds')
        b = src.mb_bounds
    w, s, e, n = b
    if w >= e:
        raise Fail('bbox W must be < E (areas crossing the antimeridian are not supported)')
    if s >= n:
        raise Fail('bbox S must be < N')
    if not (-180 <= w <= 180 and -180 <= e <= 180 and -90 <= s <= 90 and -90 <= n <= 90):
        raise Fail('bbox out of range')
    b = (w, max(s, -MAX_LAT), e, min(n, MAX_LAT))
    return b, None


def parse_zoom(s, src):
    m = re.match(r'^(\d+)(?:-(\d+))?$', s or '')
    if not m:
        raise Fail('--zoom wants A-B, e.g. 8-16')
    z0 = int(m.group(1))
    z1 = int(m.group(2)) if m.group(2) else z0
    cap = min(ZOOM_MAX_CARD, src.max_zoom)
    if z0 > z1:
        raise Fail('--zoom A-B with A <= B')
    if z1 > cap:
        raise Fail('max zoom for %s is %d (the device then overzooms 2 more levels)' % (src.label, cap))
    if src.sid.endswith('@2x') and z0 < 1:
        raise Fail('--hidpi-split needs zoom >= 1')
    return z0, z1


def ranges_for(bounds, z0, z1):
    out = []
    for z in range(z0, z1 + 1):
        x0, y0, x1, y1 = tile_range(bounds, z)
        out.append((z, (x0, y0, x1, y1), (x1 - x0 + 1) * (y1 - y0 + 1)))
    return out


def iter_tiles(ranges):
    for z, (x0, y0, x1, y1), _ in ranges:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                yield z, x, y


def check_caps(ranges, a):
    total = sum(n for _, _, n in ranges)
    cap = getattr(a, 'max_tiles', 50000)
    if cap > 250000:
        raise Fail('--max-tiles cannot exceed 250000')
    if total > cap:
        raise Fail('%d tiles is more than --max-tiles %d: shrink the area or the zoom range (or raise '
                   '--max-tiles, up to 250000)' % (total, cap))
    return total


def confirm(a):
    if getattr(a, 'yes', False):
        return
    if not sys.stdin.isatty():
        raise Fail('not a terminal: add --yes to proceed')
    sys.stderr.write('Proceed? [y/N] ')
    sys.stderr.flush()
    if sys.stdin.readline().strip().lower() not in ('y', 'yes'):
        raise Fail('cancelled')


def clip_bytes(s, n):
    s = re.sub(r'[\r\n]+', ' ', s or '').strip()
    b = s.encode('utf-8')[:n]
    return b.decode('utf-8', 'ignore')


# ------------------------------------------------------------------------------------ conversion
def hex_to_rgb(s):
    m = re.match(r'^#?([0-9a-fA-F]{6})$', s or '')
    if not m:
        raise Fail('colour must be #RRGGBB (got %r)' % s)
    v = int(m.group(1), 16)
    return (v >> 16) & 255, (v >> 8) & 255, v & 255


def rgb_to_565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def c565_to_rgb(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


class Converter(object):
    def __init__(self, fmt='p8rle', colors=256, filt='none', comp_bg=(255, 255, 255)):
        need_pil()
        self.fmt = FMT_NAMES[fmt]
        if not 2 <= colors <= 256:
            raise Fail('--colors must be 2..256')
        self.colors = colors
        self.filt = filt
        self.dim = None
        if filt.startswith('dim:'):
            try:
                self.dim = int(filt[4:])
            except ValueError:
                self.dim = -1
            if not 10 <= self.dim <= 100:
                raise Fail('--filter dim:N wants N = 10..100 (percent brightness)')
        elif filt not in ('none', 'night', 'gray'):
            raise Fail('--filter none|night|dim:N|gray')
        self.comp_bg = comp_bg
        self.hist = {}
        self.luma = [0.0, 0]
        self.sampled = 0

    def image(self, data=None, img=None, quad=None):
        if img is None:
            img = Image.open(io.BytesIO(data))
            img.load()
        if img.mode != 'RGB':
            rgba = img.convert('RGBA')
            base = Image.new('RGBA', rgba.size, self.comp_bg + (255,))
            img = Image.alpha_composite(base, rgba).convert('RGB')
        if quad is not None:                                  # --hidpi-split: 512 @2x parent quadrant
            if img.size != (512, 512):
                img = img.resize((512, 512), Image.LANCZOS)
            qx, qy = quad
            img = img.crop((qx * 256, qy * 256, qx * 256 + 256, qy * 256 + 256))
        if img.size != (256, 256):
            img = img.resize((256, 256), Image.LANCZOS)
        if self.filt == 'gray':
            img = ImageOps.grayscale(img).convert('RGB')
        elif self.filt == 'night':                            # invert lightness, keep hue, then dim
            hsv = ImageOps.invert(img).convert('HSV')
            h, s, v = hsv.split()
            h = h.point(lambda p: (p + 128) & 255)
            img = Image.merge('HSV', (h, s, v)).convert('RGB')
            img = ImageEnhance.Brightness(img).enhance(0.85)
        elif self.dim is not None and self.dim < 100:
            img = ImageEnhance.Brightness(img).enhance(self.dim / 100.0)
        return img

    def _sample(self, v565):
        if self.sampled >= 64:
            return
        self.sampled += 1
        if np is not None:
            cnt = np.bincount(v565.ravel(), minlength=65536)
            top = np.argsort(cnt)[-8:]
            for t in top:
                self.hist[int(t)] = self.hist.get(int(t), 0) + int(cnt[t])
        else:
            for v in v565[::17]:
                self.hist[v] = self.hist.get(v, 0) + 1

    def record(self, img):
        if np is not None:
            a = np.asarray(img, dtype=np.uint16)
            v = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
            self.luma[0] += float((a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114).mean())
        else:
            px = list(img.getdata())
            v = [rgb_to_565(*p) for p in px]
            self.luma[0] += sum(p[0] * 0.299 + p[1] * 0.587 + p[2] * 0.114 for p in px[::7]) / len(px[::7])
        self.luma[1] += 1
        self._sample(v)
        if self.fmt == FMT_RGB565:
            if np is not None:
                return v.astype('>u2').tobytes()
            return b''.join(struct.pack('>H', x) for x in v)
        if np is not None:
            uniq, inv = np.unique(v, return_inverse=True)
            if len(uniq) > self.colors:
                q = img.quantize(colors=self.colors, method=Image.Quantize.FASTOCTREE)
                pl = (q.getpalette() or [])[:768]
                pal = np.array(pl + [0] * (768 - len(pl)), dtype=np.uint16).reshape(-1, 3)
                p565 = ((pal[:, 0] >> 3) << 11) | ((pal[:, 1] >> 2) << 5) | (pal[:, 2] >> 3)
                v2 = p565[np.asarray(q, dtype=np.uint8)]
                uniq, inv = np.unique(v2, return_inverse=True)        # merge duplicate entries
            idx = inv.reshape(-1).astype(np.uint8).tobytes()
            return encode_p8rle([int(u) for u in uniq], idx)
        uniq = sorted(set(v))
        if len(uniq) > self.colors:
            q = img.quantize(colors=self.colors, method=Image.Quantize.FASTOCTREE)
            pl = q.getpalette()
            v = [rgb_to_565(pl[3 * i], pl[3 * i + 1], pl[3 * i + 2]) for i in q.getdata()]
            uniq = sorted(set(v))
        pos = dict((c, i) for i, c in enumerate(uniq))
        return encode_p8rle(uniq, bytes(pos[c] for c in v))

    def auto_bg(self):
        if not self.hist:
            return DEFAULT_BG
        v = max(self.hist.items(), key=lambda kv: kv[1])[0]
        return '#%02X%02X%02X' % c565_to_rgb(v)

    def is_dark(self):
        return self.luma[1] > 0 and self.luma[0] / self.luma[1] < 96


# ------------------------------------------------------------------------------------- synthetic
_synth_fonts = []


def synth_image(z, x, y, bg):
    need_pil()
    if not _synth_fonts:
        for size in (20, 11):
            try:
                _synth_fonts.append(ImageFont.load_default(size=size))
            except TypeError:                                 # Pillow < 10.1: bitmap font
                _synth_fonts.append(ImageFont.load_default())
    big, small = _synth_fonts
    bgc = hex_to_rgb(bg)
    tint = (40, 52, 78) if z % 2 == 0 else (40, 70, 52)       # zoom parity
    im = Image.new('RGB', (256, 256), tint if (x + y) % 2 else bgc)
    d = ImageDraw.Draw(im)
    d.fontmode = '1'                                          # no anti-aliasing: few colours
    grid = (70, 74, 82)
    for i in range(32, 256, 32):
        d.line([(i, 0), (i, 255)], fill=grid)
        d.line([(0, i), (255, i)], fill=grid)
    edge = (230, 120, 40)
    d.rectangle([0, 0, 255, 255], outline=edge)
    cross = (240, 70, 70)
    d.line([(116, 128), (140, 128)], fill=cross)
    d.line([(128, 116), (128, 140)], fill=cross)
    txt = '%d/%d/%d' % (z, x, y)
    tw = d.textlength(txt, font=big)
    d.text((128 - tw / 2, 84), txt, fill=(235, 235, 235), font=big)
    w, s, e, n = tile_bounds(z, x, y)
    d.text((4, 4), '%.4f, %.4f' % (n, w), fill=(180, 190, 200), font=small)   # NW corner lat, lon
    return im


# ------------------------------------------------------------------------------------- card / fs
def volume_info(path):
    """diskutil plist for the volume mounted exactly at path, or None if path is not a volume root."""
    if not os.path.ismount(path) or sys.platform != 'darwin' or os.path.realpath(path) == '/':
        return None
    try:
        out = subprocess.run(['diskutil', 'info', '-plist', path], capture_output=True, timeout=20).stdout
        return plistlib.loads(out)
    except Exception:
        return {}


def check_volume(out):
    """Refuse non-FAT volume roots; returns True when out is a volume root."""
    info = volume_info(out)
    if info is None:
        return False
    if not info:
        log('warning: could not read `diskutil info` for %s; make sure the card is FAT32' % out)
        return True
    fs_type = (info.get('FilesystemType') or '').lower()
    fs_name = info.get('FilesystemName') or info.get('FilesystemUserVisibleName') or fs_type
    disk = info.get('ParentWholeDisk') or 'diskN'
    size = info.get('TotalSize') or info.get('Size') or 0
    if size > 32 * 1024 ** 3:
        log('warning: %s is %.0f GB; 8-32 GB SDHC cards are what the device was planned around. A bigger '
            'card must still be FAT32 (macOS formats SDXC as exFAT by default).' % (out, size / 1e9))
    if fs_type != 'msdos':
        extra = (' exFAT cannot be read by the device (its FAT driver is built without exFAT).'
                 if 'exfat' in fs_type or 'exfat' in fs_name.lower() else '')
        raise Fail('%s is %s, not FAT32.%s Reformat the card (THIS ERASES IT; check the disk number with '
                   '`diskutil list` first):\n  diskutil eraseDisk FAT32 CAIRN MBRFormat /dev/%s\n'
                   'or write to a plain folder and copy it onto the card yourself.' % (out, fs_name, extra, disk))
    if 'FAT32' not in fs_name.upper():
        log('warning: %s is %s; FAT32 is the tested layout' % (out, fs_name))
    return True


def finish_card(out, setdir, is_volume):
    if sys.platform == 'darwin' and shutil.which('dot_clean'):
        subprocess.run(['dot_clean', '-m', setdir], capture_output=True)
    for root, dirs, files in os.walk(setdir):
        for f in files:
            if f.startswith('._'):
                try:
                    os.remove(os.path.join(root, f))
                except OSError:
                    pass
    if is_volume:
        try:
            open(os.path.join(out, '.metadata_never_index'), 'a').close()
        except OSError:
            pass


# -------------------------------------------------------------------------------------- manifest
def manifest_text(m):
    order = ['l2map', 'format', 'minzoom', 'maxzoom', 'bounds', 'attribution', 'attribution_short', 'title',
             'license_url', 'bg', 'dark', 'center']
    lines = ['# Cairn offline map set: tools/maptiles.py, format in ui-l2/FRAMEWORK.md section 16']
    for k in order:
        if m.get(k) not in (None, ''):
            lines.append('%s=%s' % (k, m[k]))
    for k in sorted((k for k in m if k.startswith('cov.')), key=lambda k: int(k[4:])):
        lines.append('%s=%s' % (k, m[k]))
    for k in ('source', 'tiles', 'bytes', 'created', 'tool'):
        if m.get(k) not in (None, ''):
            lines.append('%s=%s' % (k, m[k]))
    txt = '\n'.join(lines) + '\n'
    data = txt.encode('utf-8')
    if len(data) > MANIFEST_MAX or any(len(l.encode('utf-8')) > LINE_MAX for l in lines):
        raise Fail('manifest too large (internal error)')
    return data


def parse_manifest(data):
    """Returns (dict, [errors])."""
    errs = []
    if len(data) > MANIFEST_MAX:
        errs.append('manifest is %d bytes (> %d)' % (len(data), MANIFEST_MAX))
    try:
        txt = data.decode('utf-8')
    except UnicodeDecodeError:
        return {}, ['manifest is not UTF-8']
    if '\r' in txt:
        errs.append('manifest has CR line endings (LF only)')
    m = {}
    for n, line in enumerate(txt.split('\n'), 1):
        if len(line.encode('utf-8')) > LINE_MAX:
            errs.append('line %d longer than %d bytes' % (n, LINE_MAX))
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        if '=' not in s:
            errs.append('line %d is not key=value' % n)
            continue
        k, v = s.split('=', 1)
        m[k.strip()] = v.strip()
    return m, errs


def check_manifest(m, errs):
    """Validates like the device; returns (fmt, zmin, zmax, bounds, cov {z: (x0,y0,x1,y1)})."""
    def lim(k, n):
        if k in m and len(m[k].encode('utf-8')) > n:
            errs.append('%s longer than %d bytes' % (k, n))
    for k in ('l2map', 'format', 'minzoom', 'maxzoom', 'bounds', 'attribution'):
        if k not in m:
            errs.append('missing required key %s' % k)
    if m.get('l2map') not in (None, '1'):
        errs.append('l2map=%s (want 1)' % m.get('l2map'))
    fmt = FMT_NAMES.get(m.get('format', ''))
    if 'format' in m and fmt is None:
        errs.append('format=%s (want p8rle, rgb565 or mvt)' % m['format'])
    try:
        zmin, zmax = int(m.get('minzoom', '0')), int(m.get('maxzoom', '0'))
        if not 0 <= zmin <= zmax <= ZOOM_MAX_CARD:
            errs.append('need 0 <= minzoom <= maxzoom <= %d' % ZOOM_MAX_CARD)
    except ValueError:
        errs.append('minzoom / maxzoom not integers')
        zmin = zmax = 0
    bounds = None
    try:
        bounds = tuple(float(v) for v in m.get('bounds', '').split(','))
        if len(bounds) != 4 or not (bounds[0] < bounds[2] and bounds[1] < bounds[3]):
            raise ValueError
    except ValueError:
        if 'bounds' in m:
            errs.append('bounds=%s (want W,S,E,N with W<E, S<N)' % m.get('bounds'))
        bounds = None
    lim('attribution', 96)
    lim('attribution_short', 32)
    lim('title', 24)
    lim('license_url', 64)
    if 'bg' in m and not re.match(r'^#[0-9a-fA-F]{6}$', m['bg']):
        errs.append('bg=%s (want #RRGGBB)' % m['bg'])
    if 'dark' in m and m['dark'] not in ('0', '1'):
        errs.append('dark=%s (want 0 or 1)' % m['dark'])
    cov = {}
    for z in range(zmin, zmax + 1):
        k = 'cov.%d' % z
        if k in m:
            try:
                c = tuple(int(v) for v in m[k].split(','))
                if len(c) != 4:
                    raise ValueError
                cov[z] = c
            except ValueError:
                errs.append('%s=%s (want x0,y0,x1,y1)' % (k, m[k]))
        elif bounds:
            cov[z] = tile_range(bounds, z)
    return fmt, zmin, zmax, bounds, cov


# ---------------------------------------------------------------------------------------- commands
def estimate_rows(src, ranges, a, cache, fmt):
    """Per-zoom estimate rows and totals. With a.sample, converts K tiles per zoom to measure size."""
    rows = []
    rate = getattr(a, 'rate', None) or src.rate
    maxreq = getattr(a, 'max_requests', None)
    if maxreq is None and src.daily_quota:
        maxreq = 11000
    sample = getattr(a, 'sample', 0) or 0
    conv = None
    if sample and not src.vector:              # vector tiles are stored as fetched: nothing to convert
        conv = Converter(fmt, getattr(a, 'colors', 256), getattr(a, 'filter', 'none'))
    lat = a._centre_lat
    for z, (x0, y0, x1, y1), n in ranges:
        tiles = [(z, x, y) for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)]
        if src.kind == 'http':
            keys = fetch_keys(src, tiles)
            req = sum(1 for k in keys if cache is None or not cache.has(*k))
        else:
            req = 0
        per = None
        if conv is not None:
            pick = random.Random(z).sample(tiles, min(sample, len(tiles)))
            sizes = []
            for k in pick:
                img = load_tile_image(src, cache, k, a, fetch_ok=True)
                if img is not None:
                    quad = (k[1] & 1, k[2] & 1) if src.sid.endswith('@2x') else None
                    sizes.append(len(conv.record(conv.image(img=img, quad=quad))))
            if sizes:
                per = sum(sizes) / float(len(sizes))
        guessed = per is None
        if per is None:
            per = RGB565_BYTES if fmt == 'rgb565' else src.est_bytes
        per_card = math.ceil(per / SECTOR) * SECTOR
        blocks = ((x1 >> 4) - (x0 >> 4) + 1) * ((y1 >> 4) - (y0 >> 4) + 1)
        mb = (n * per_card + blocks * BLOCK_HDR_BYTES) / 1e6
        mpp = metres_per_px(lat, z)
        secs = req / rate + n * 0.02
        quota = ('%d' % max(1, math.ceil(req / float(maxreq)))) if (src.daily_quota and maxreq and req) else '-'
        rows.append(dict(z=z, tiles=n, ftpx=mpp * 3.28084, mi=320 * mpp / 1609.344, req=req, mb=mb,
                         secs=secs, quota=quota, guessed=guessed))
    return rows


def print_estimate(src, bounds, rows, a):
    w, s, e, n = bounds
    print('Area  W %.5f  S %.5f  E %.5f  N %.5f   source %s' % (w, s, e, n, src.label))
    print(' zoom     tiles    ft/px   mi/320px   requests      MB      time  quota-days')
    tot = dict(tiles=0, req=0, mb=0.0, secs=0.0)
    for r in rows:
        print(' %4d  %8d  %7.1f  %9.2f  %9d  %6s%s  %8s  %10s' % (
            r['z'], r['tiles'], r['ftpx'], r['mi'], r['req'], '%.1f' % r['mb'], '~' if r['guessed'] else ' ',
            fmt_time(r['secs']), r['quota']))
        for k in tot:
            tot[k] += r[k]
    maxreq = getattr(a, 'max_requests', None) or 11000
    qd = '%d' % math.ceil(tot['req'] / float(maxreq)) if (src.daily_quota and tot['req']) else ''
    print(' total %8d  %7s  %9s  %9d  %6s%s  %8s  %10s' % (tot['tiles'], '', '', tot['req'], '%.1f' % tot['mb'],
                                                          '~' if any(r['guessed'] for r in rows) else ' ',
                                                          fmt_time(tot['secs']), qd))
    if any(r['guessed'] for r in rows):
        print(' (~ = guessed from typical tile sizes; --sample K converts K tiles per zoom to measure)')
    if src.kind == 'http':
        print(' requests = tiles not in the cache; time assumes %.1f req/s' % (getattr(a, 'rate', None) or src.rate))
    sys.stdout.flush()
    return tot


def load_tile_image(src, cache, k, a, fetch_ok=False, parents=None):
    """PIL image (any size/mode) or None for one card tile; quadrant crop handled by the caller."""
    z, x, y = k
    if src.kind == 'synth':
        return synth_image(z, x, y, a._bg if getattr(a, '_bg', None) else DEFAULT_BG)
    if src.kind == 'mbtiles':
        with src.mb_lock:
            row = src.mb.execute('SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?',
                                 (z, x, (1 << z) - 1 - y)).fetchone()
        if not row or not row[0]:
            return None
        return open_image(bytes(row[0]))
    fk = fetch_keys(src, [k])[0]
    if parents is not None and fk in parents:
        return parents[fk]
    data = cache.get(*fk)
    if data is None and fetch_ok and not cache.has(*fk):
        f = Fetcher(src, cache, a)
        f.run([fk], 'sample')
        data = cache.get(*fk)
    img = open_image(data) if data else None
    if parents is not None:
        if len(parents) > 64:
            parents.pop(next(iter(parents)))
        parents[fk] = img
    return img


def open_image(data):
    need_pil()
    try:
        img = Image.open(io.BytesIO(data))
        img.load()
        return img
    except Exception:
        return None


def common_setup(a, need_attr=False, synth=False):
    src = synth_source() if synth else make_source(a, need_attr=need_attr)
    bounds, centre = parse_area(a, src)
    z0, z1 = parse_zoom(a.zoom, src)
    ranges = ranges_for(bounds, z0, z1)
    if src.sid.startswith('usgs-') and not (bounds[0] < -64 and bounds[2] > -180 and bounds[1] < 72 and bounds[3] > 17):
        log('warning: USGS maps cover the US only; this area will come back empty (try geoapify:)')
    a._centre_lat = centre[0] if centre else (bounds[1] + bounds[3]) / 2.0
    a._centre = centre
    cache = Cache(getattr(a, 'cache', None), src, getattr(a, 'retry_missing', False)) if src.kind == 'http' else None
    return src, bounds, (z0, z1), ranges, cache


def cmd_presets(a):
    print('Tile sources (licence terms checked %s; re-check them before a big download):\n' % TERMS_CHECKED)
    print(' %-22s %-30s %5s %7s %4s  %-22s %s' % ('preset', 'what', 'max z', 'req/s', '@2x', 'key', 'terms'))
    for p, label, mz, rate, hidpi, key, terms in preset_table():
        print(' %-22s %-30s %5d %7.1f %4s  %-22s %s' % (p, label, mz, rate, 'yes' if hidpi else 'no', key, terms))
    print('\n geoapify styles: %s' % ', '.join(GEOAPIFY_STYLES))
    print(' thunderforest styles: %s (needs --ack-plan thunderforest-bulk)' % ', '.join(THUNDERFOREST_STYLES))
    print(' also: --mbtiles FILE (raster MBTiles you are allowed to use), --url TEMPLATE --attribution TEXT')
    print('\nRefused hosts (no override):')
    for d, why in DENY:
        print('  %-26s %s' % (d, why))
    print('\nThe card you build is for your own use: do not share it (most terms forbid redistribution).')
    return 0


def cmd_estimate(a):
    src, bounds, zr, ranges, cache = common_setup(a)
    rows = estimate_rows(src, ranges, a, cache, a.format)
    print_estimate(src, bounds, rows, a)
    total = sum(r['tiles'] for r in rows)
    if total > a.max_tiles:
        print(' note: %d tiles is over --max-tiles %d; build would refuse' % (total, a.max_tiles))
    return 0


def cmd_fetch(a):
    src, bounds, zr, ranges, cache = common_setup(a)
    if src.kind != 'http':
        raise Fail('fetch is only for network sources (MBTiles / synth need no download)')
    check_caps(ranges, a)
    rows = estimate_rows(src, ranges, a, cache, getattr(a, 'format', 'p8rle'))
    print_estimate(src, bounds, rows, a)
    log('cache: %s' % cache.dir)
    confirm(a)
    return do_fetch(src, cache, ranges, a)


def do_fetch(src, cache, ranges, a):
    f = Fetcher(src, cache, a)
    left = f.run(fetch_keys(src, list(iter_tiles(ranges))))
    if f.abort:
        raise Fail(f.abort)
    if f.interrupted or f.budget_hit:
        why = 'interrupted' if f.interrupted else 'request limit reached (--max-requests %s%s)' % (
            f.max_requests, ' per day' if src.daily_quota else '')
        log('%s; %d tiles still to fetch. Everything fetched so far is cached. Resume with:\n  %s'
            % (why, left, resume_cmd()))
        return 3
    return 0


def cmd_build(a, synth=False):
    if not NAME_RE.match(a.name or ''):
        raise Fail('--name must be 1-24 of a-z 0-9 - (it is the folder name and the set\'s identity)')
    if getattr(a, 'title', None) and len(a.title.encode('utf-8')) > 24:
        raise Fail('--title is at most 24 bytes')
    src, bounds, (z0, z1), ranges, cache = common_setup(a, need_attr=not synth, synth=synth)
    vector = src.vector
    if vector:
        a.format = 'mvt'
    else:
        need_pil()
    total = check_caps(ranges, a)
    if vector and total > OPENFREEMAP_MAX_TILES:
        raise Fail('OpenFreeMap allows offline downloads of at most %d tiles (or %d MB) each; this area is %d '
                   'tiles. Shrink the area or the zoom range (vector tiles stop at z14 and the device draws '
                   'z15-18 from them), or ask OpenFreeMap first (%s).' % (OPENFREEMAP_MAX_TILES,
                                                                        OPENFREEMAP_MAX_MB, total, OPENFREEMAP_TERMS))
    out = os.path.abspath(a.out)
    if not os.path.isdir(out):
        raise Fail('--out %s is not a folder (the card root, e.g. /Volumes/CAIRN)' % out)
    is_vol = check_volume(out)
    l2map = os.path.join(out, 'Maps')   # the card's map folder (ui-l2 CardLayout.h)
    final = os.path.join(l2map, a.name)
    if os.path.exists(final) and not a.overwrite:
        raise Fail('%s exists: pick another --name or add --overwrite' % final)
    if not synth:
        rows = estimate_rows(src, ranges, a, cache, a.format)
        print_estimate(src, bounds, rows, a)
        confirm(a)
        if src.kind == 'http' and not a.no_fetch:
            rc = do_fetch(src, cache, ranges, a)
            if rc:
                return rc
    if a.bg == 'auto':
        comp = (32, 33, 36) if (src.dark or a.filter == 'night') else (255, 255, 255)
    else:
        comp = hex_to_rgb(a.bg)
    a._bg = DEFAULT_BG if a.bg == 'auto' else a.bg
    conv = None if vector else Converter(a.format, a.colors, a.filter, comp)
    fmt = FMT_MVT if vector else conv.fmt
    partial = os.path.join(l2map, '.%s.partial' % a.name)
    if os.path.exists(partial):
        shutil.rmtree(partial)
    os.makedirs(partial)
    prog = Progress(total, 'convert')
    stats = dict(tiles=0, records=0, bytes=0, blocks=0, absent=0, bad=0)
    parents = {}
    hidpi = src.sid.endswith('@2x')
    try:
        for z, (x0, y0, x1, y1), _ in ranges:
            zdir = os.path.join(partial, str(z))
            os.makedirs(zdir)
            for by in range(y0 >> 4, (y1 >> 4) + 1):
                for bx in range(x0 >> 4, (x1 >> 4) + 1):
                    recs = {}
                    for ty in range(max(y0, by << 4), min(y1, (by << 4) + 15) + 1):
                        for tx in range(max(x0, bx << 4), min(x1, (bx << 4) + 15) + 1):
                            if vector:                  # stored as fetched: the device draws it
                                data = cache.get(z, tx, ty)
                                prog.tick()
                                if not data:
                                    stats['absent'] += 1
                                elif len(data) > MVT_MAX_BYTES:
                                    stats['bad'] += 1
                                    log('\nwarning: tile %d/%d/%d is %d bytes, over the device limit %d' % (
                                        z, tx, ty, len(data), MVT_MAX_BYTES))
                                else:
                                    recs[blockslot(tx, ty)] = data
                                continue
                            img = load_tile_image(src, cache, (z, tx, ty), a, parents=parents)
                            prog.tick()
                            if img is None:
                                stats['absent'] += 1
                                continue
                            try:
                                im = conv.image(img=img, quad=((tx & 1, ty & 1) if hidpi else None))
                                recs[blockslot(tx, ty)] = conv.record(im)
                            except Exception as e:                       # undecodable image
                                stats['bad'] += 1
                                if stats['bad'] <= 5:
                                    log('\nwarning: tile %d/%d/%d not usable: %s' % (z, tx, ty, e))
                    if not recs:
                        continue
                    data, nrec = build_block(z, fmt, bx, by, recs)
                    with open(os.path.join(zdir, '%d_%d.l2t' % (bx, by)), 'wb') as f:
                        f.write(data)
                    stats['tiles'] += len(recs)
                    stats['records'] += nrec
                    stats['bytes'] += len(data)
                    stats['blocks'] += 1
            if not os.listdir(zdir):
                os.rmdir(zdir)
    except KeyboardInterrupt:
        prog.end()
        shutil.rmtree(partial, ignore_errors=True)
        log('interrupted; nothing written (downloaded tiles stay cached). Re-run:\n  %s' % resume_cmd())
        return 130
    prog.end()
    if stats['tiles'] == 0:
        shutil.rmtree(partial, ignore_errors=True)
        raise Fail('no tiles available for this area / zoom (nothing written)')
    if vector:                                  # the device's style decides the colours
        bg, dark = '#F2EFE9', 0
    else:
        bg = conv.auto_bg() if a.bg == 'auto' else a.bg.upper()
        dark = 1 if (src.dark or a.filter == 'night' or conv.is_dark()) else 0
    c = a._centre or ((bounds[1] + bounds[3]) / 2.0, (bounds[0] + bounds[2]) / 2.0)
    m = {
        'l2map': '1', 'format': a.format, 'minzoom': z0, 'maxzoom': z1,
        'bounds': '%.6f,%.6f,%.6f,%.6f' % bounds,
        'attribution': clip_bytes(src.attribution, 96),
        'attribution_short': clip_bytes(src.attribution_short or src.attribution, 32),
        'title': clip_bytes(a.title or a.name, 24),
        'license_url': clip_bytes(src.license_url or '', 64),
        'bg': bg, 'dark': dark,
        'center': '%.6f,%.6f,%d' % (c[1], c[0], max(z0, min(z1, z1 - 2))),
        'source': src.preset if src.kind != 'http' else redact(src.preset, src.key),
        'tiles': stats['tiles'], 'bytes': stats['bytes'],
        'created': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), 'tool': TOOL,
    }
    for z, cov, _ in ranges:
        m['cov.%d' % z] = '%d,%d,%d,%d' % cov
    with open(os.path.join(partial, 'manifest.txt'), 'wb') as f:
        f.write(manifest_text(m))
    if os.path.exists(final):
        old = os.path.join(l2map, '.%s.old' % a.name)
        if os.path.exists(old):
            shutil.rmtree(old)
        os.rename(final, old)
        os.rename(partial, final)
        shutil.rmtree(old, ignore_errors=True)
    else:
        os.rename(partial, final)
    finish_card(out, final, is_vol)
    print('wrote %s: %d tiles (%d unique records) in %d block files, %.1f MB, z%d-%d, %s%s' % (
        final, stats['tiles'], stats['records'], stats['blocks'], stats['bytes'] / 1e6, z0, z1, a.format,
        ', %d tiles not available' % stats['absent'] if stats['absent'] else ''))
    print('name hash (map_set) 0x%08X; check it with: python3 tools/maptiles.py verify %s' % (fnv1a32(a.name), final))
    return 0


def cmd_synth(a):
    a.preset = a.url = a.mbtiles = None
    a.from_mbtiles_bounds = False
    a.no_fetch = True
    a.colors = 16
    a.filter = 'none'
    a.yes = True
    if a.bg == 'auto':
        a.bg = DEFAULT_BG
    return cmd_build(a, synth=True)


def cmd_verify(a):
    setdir = os.path.abspath(a.path.rstrip('/'))
    name = os.path.basename(setdir)
    errs, warns = [], []
    if not NAME_RE.match(name):
        errs.append('folder name %r is not [a-z0-9-]{1,24}' % name)
    parent = os.path.dirname(setdir)
    legacy = os.path.basename(parent) == 'maps' and os.path.basename(os.path.dirname(parent)) == 'cairn'
    if os.path.basename(parent) != 'Maps' and not legacy:
        warns.append('the set should sit at <card>/Maps/%s' % name)
    try:
        with open(os.path.join(setdir, 'manifest.txt'), 'rb') as f:
            mdata = f.read()
    except IOError:
        raise Fail('no manifest.txt in %s' % setdir)
    m, merr = parse_manifest(mdata)
    errs += merr
    fmt, zmin, zmax, bounds, cov = check_manifest(m, errs)
    for z in range(zmin, zmax + 1):
        if bounds and z in cov and 'cov.%d' % z in m and cov[z] != tile_range(bounds, z):
            warns.append('cov.%d %s differs from bounds %s' % (z, cov[z], tile_range(bounds, z)))
    t0 = time.time()
    tiles = recs = files = nbytes = 0
    per_z = {}
    for root, dirs, fs in os.walk(setdir):
        for f in fs:
            if f.startswith('._'):
                warns.append('AppleDouble file %s (run dot_clean -m)' % os.path.join(root, f))
    for entry in sorted(os.listdir(setdir)):
        p = os.path.join(setdir, entry)
        if entry == 'manifest.txt' or entry.startswith('.'):
            continue
        if not (os.path.isdir(p) and entry.isdigit()):
            warns.append('unexpected entry %s' % entry)
            continue
        z = int(entry)
        if not zmin <= z <= zmax:
            warns.append('zoom folder %d outside minzoom..maxzoom' % z)
            continue
        c = cov.get(z)
        for bf in sorted(os.listdir(p)):
            if bf.startswith('.'):
                continue
            mm = re.match(r'^(\d+)_(\d+)\.l2t$', bf)
            if not mm:
                warns.append('unexpected file %d/%s' % (z, bf))
                continue
            bx, by = int(mm.group(1)), int(mm.group(2))
            where = '%d/%s' % (z, bf)
            with open(os.path.join(p, bf), 'rb') as fh:
                data = fh.read()
            files += 1
            nbytes += len(data)
            if len(data) < BLOCK_HDR_BYTES or data[:4] != b'L2T1':
                errs.append('%s: bad magic / short header' % where)
                continue
            hz, hfmt, hs, hbx, hby = struct.unpack_from('<BBHII', data, 4)
            if (hz, hs, hbx, hby) != (z, HDR_SECTORS, bx, by):
                errs.append('%s: header z=%d sectors=%d bx=%d by=%d does not match' % (where, hz, hs, hbx, hby))
                continue
            if hfmt != fmt:
                errs.append('%s: fmt %d differs from the manifest' % (where, hfmt))
                continue
            if any(data[BLOCK_INDEX_OFF + 2048:BLOCK_HDR_BYTES]):
                warns.append('%s: header padding not zero' % where)
            done = {}
            ntiles = 0
            for slot in range(256):
                sec, nb = struct.unpack_from('<II', data, BLOCK_INDEX_OFF + 8 * slot)
                if sec == 0 or nb == 0:
                    continue
                tx, ty = (bx << 4) | (slot & 15), (by << 4) | (slot >> 4)
                tw = '%s z/x/y %d/%d/%d' % (where, z, tx, ty)
                if sec < HDR_SECTORS or sec * SECTOR + nb > len(data):
                    errs.append('%s: record outside the file (sector %d, %d bytes)' % (tw, sec, nb))
                    continue
                if ((fmt == FMT_P8RLE and nb > P8RLE_MAX_BYTES) or (fmt == FMT_RGB565 and nb != RGB565_BYTES)
                        or (fmt == FMT_MVT and nb > MVT_MAX_BYTES)):
                    errs.append('%s: record size %d invalid for the format' % (tw, nb))
                    continue
                if c and not (c[0] <= tx <= c[2] and c[1] <= ty <= c[3]):
                    warns.append('%s: outside cov.%d (the device will not show it)' % (tw, z))
                ntiles += 1
                if (sec, nb) not in done:
                    rec = data[sec * SECTOR:sec * SECTOR + nb]
                    ok = True
                    if fmt == FMT_P8RLE:
                        try:
                            decode_p8rle(rec)
                        except ValueError as e:
                            errs.append('%s: corrupt P8RLE: %s' % (tw, e))
                            ok = False
                    elif fmt == FMT_MVT and rec[:2] == b'\x1f\x8b':
                        try:
                            gzip.decompress(rec)
                        except Exception as e:
                            errs.append('%s: corrupt gzip: %s' % (tw, e))
                            ok = False
                    done[(sec, nb)] = ok
                    recs += 1
            if ntiles == 0:
                warns.append('%s: holds no tiles' % where)
            tiles += ntiles
            per_z[z] = per_z.get(z, 0) + ntiles
    el = time.time() - t0
    print('set %s  (name hash 0x%08X)' % (name, fnv1a32(name)))
    print(' title "%s"  format %s  z%d-%d  bg %s  dark %s' % (m.get('title', name), m.get('format'), zmin, zmax,
                                                             m.get('bg', DEFAULT_BG), m.get('dark', '0')))
    print(' attribution "%s"' % m.get('attribution', ''))
    print(' %d block files, %d tiles, %d unique records, %.1f MB, checked in %.1fs' % (files, tiles, recs,
                                                                                     nbytes / 1e6, el))
    for z in range(zmin, zmax + 1):
        c = cov.get(z)
        want = (c[2] - c[0] + 1) * (c[3] - c[1] + 1) if c else 0
        print('  z%-2d %7d / %-7d tiles (%5.1f%%)' % (z, per_z.get(z, 0), want, 100.0 * per_z.get(z, 0) / max(1, want)))
    for w in warns[:30]:
        print(' warning: ' + w)
    if len(warns) > 30:
        print(' ... %d more warnings' % (len(warns) - 30))
    for e in errs[:50]:
        print(' ERROR: ' + e)
    if len(errs) > 50:
        print(' ... %d more errors' % (len(errs) - 50))
    print('OK' if not errs else 'FAILED (%d errors)' % len(errs))
    return 0 if not errs else 1


def cmd_selftest(a):
    fails = []

    def check(name, cond, detail=''):
        if not cond:
            fails.append('%s %s' % (name, detail))
        print(' %-44s %s' % (name, 'ok' if cond else 'FAIL ' + detail))

    def row_bytes(row):
        e = packbits_row(bytes(row))
        return struct.pack('<H', len(e)) + e

    g1 = bytes([0] * 256)
    check('G1 256 x 0', row_bytes(g1) == bytes.fromhex('0400FF00FB00'), row_bytes(g1).hex())
    g2 = bytes(range(256))
    exp2 = bytes.fromhex('0201') + b'\x7f' + bytes(range(128)) + b'\x7f' + bytes(range(128, 256))
    check('G2 0..255', row_bytes(g2) == exp2, row_bytes(g2)[:8].hex())
    g3 = bytes([5, 5, 5, 7] + [9] * 252)
    check('G3 5,5,5,7, 252 x 9', row_bytes(g3) == bytes.fromhex('0800800500 07FF09F709'.replace(' ', '')),
          row_bytes(g3).hex())
    r1 = encode_p8rle([0x1234], bytes(65536))
    exp_r1 = bytes.fromhex('500101001234') + bytes.fromhex('0400FF00FB00') * 256
    check('R1 record bytes (1542)', r1 == exp_r1 and len(r1) == 1542, '%d bytes' % len(r1))
    pal, idx = decode_p8rle(r1)
    check('R1 decodes to 0x1234 (memory 12 34)', pal == [0x1234] and idx == bytes(65536)
          and struct.pack('>H', pal[0]) == b'\x12\x34')
    for name, row in (('G1', g1), ('G2', g2), ('G3', g3)):
        out = bytearray()
        e = packbits_row(row)
        check('%s round trip' % name, unpackbits_row(e, 0, len(e), 256, out) is None and bytes(out) == row)
    rnd = random.Random(1)
    ok = True
    for _ in range(300):
        row = bytes(rnd.choice((0, 1, 2)) if rnd.random() < 0.7 else rnd.randrange(256) for _ in range(256))
        e = packbits_row(row)
        out = bytearray()
        if unpackbits_row(e, 0, len(e), 256, out) or bytes(out) != row or len(e) > 258:
            ok = False
    check('random rows round trip, <= 258 bytes', ok)
    worst = encode_p8rle(list(range(256)), bytes(range(256)) * 256)
    check('worst case 67076 <= 69632', len(worst) == 67076, '%d' % len(worst))

    def corrupt(name, rec, nc=4):
        try:
            decode_p8rle(rec)
            check('reject ' + name, False)
        except ValueError:
            check('reject ' + name, True)
    good = encode_p8rle([1, 2, 3, 4], bytes(65536))
    corrupt('short row', bytes.fromhex('50010100FFFF') + bytes.fromhex('0200FE00') * 256)
    corrupt('run past 256', bytes.fromhex('50010100FFFF') + bytes.fromhex('0600FF00FF00FF00') * 256)
    corrupt('index >= ncolors', bytes.fromhex('50010100FFFF') + bytes.fromhex('0400FF01FB01') * 256)
    corrupt('truncated record', good[:-3])
    corrupt('bad magic', b'Q' + good[1:])
    corrupt('trailing bytes', good + b'\0')

    # Mercator goldens (shared with test/test_l2_maptile)
    x = lon_to_x(-97000000)
    y = lat_to_y(35000000)
    check('lonToX(-97) = 0x3B05B05B', x == 0x3B05B05B, '0x%08X' % x)
    check('latToY(35) = 0x6566AADF +- 2', abs(y - 0x6566AADF) <= 2, '0x%08X' % y)
    check('z14 tile (3777, 6489)', (tile_at(x, 14), tile_at(y, 14)) == (3777, 6489),
          str((tile_at(x, 14), tile_at(y, 14))))
    check('z14 block (236, 405)', (tile_at(x, 14) >> 4, tile_at(y, 14) >> 4) == (236, 405))
    check('m/px z15 ~ 3.913', abs(metres_per_px(35.0, 15) - 3.913) < 0.001, '%.4f' % metres_per_px(35.0, 15))
    t = (tile_at(lon_to_x(-77036500), 16), tile_at(lat_to_y(38897700), 16))
    check('z16 (38.8977, -77.0365) = (18743, 25070)', t == (18743, 25070), str(t))
    rt = all(abs(x_to_lon(lon_to_x(v)) - v) <= 10 and abs(y_to_lat(lat_to_y(w)) - w) <= 10
             for v, w in ((-97000000, 35000000), (179999999, -85000000), (0, 0), (-180000000, 60123456)))
    check('round trips within 10 microdeg', rt)
    check('blockslot(0x123, 0x45) = 0x53', blockslot(0x123, 0x45) == 0x53)
    check('fnv1a32 "" / "a"', fnv1a32('') == 0x811C9DC5 and fnv1a32('a') == 0xE40C292C)
    data, nrec = build_block(14, FMT_P8RLE, 236, 405, {0: r1, 1: r1, 255: good})
    s0 = struct.unpack_from('<II', data, BLOCK_INDEX_OFF)
    s1 = struct.unpack_from('<II', data, BLOCK_INDEX_OFF + 8)
    s255 = struct.unpack_from('<II', data, BLOCK_INDEX_OFF + 8 * 255)
    check('block: dedupe, 512-aligned, header', nrec == 2 and s0 == s1 == (5, 1542) and s255[0] == 9
          and data[:4] == b'L2T1' and struct.unpack_from('<BBHII', data, 4) == (14, 1, 5, 236, 405)
          and len(data) % 512 == 0)
    check('denylist (tile.openstreetmap.org, a.basemaps.cartocdn.com)',
          bool(check_denied('https://tile.openstreetmap.org/{z}/{x}/{y}.png'))
          and bool(check_denied('https://a.basemaps.cartocdn.com/x/{z}/{x}/{y}.png'))
          and not check_denied('https://basemap.nationalmap.gov/x/{z}/{y}/{x}'))
    print('selftest: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
    return 0 if not fails else 1


# ------------------------------------------------------------------------------------------ main
def build_parser():
    p = argparse.ArgumentParser(prog='maptiles.py', description=__doc__.split('\n\n')[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog='Details: tools/MAPTILES.md.  Run "COMMAND -h" for a command\'s options.')
    sub = p.add_subparsers(dest='cmd', metavar='COMMAND')

    area = argparse.ArgumentParser(add_help=False)
    g = area.add_argument_group('AREA (one of)')
    g.add_argument('--center', metavar='LAT,LON', help='centre in decimal degrees (use with --radius-mi)')
    g.add_argument('--radius-mi', type=float, metavar='R', help='half-width of the square around --center, miles')
    g.add_argument('--bbox', metavar='W,S,E,N', help='bounding box in decimal degrees')
    g.add_argument('--from-mbtiles-bounds', action='store_true', help='use the bounds in the --mbtiles metadata')
    g.add_argument('--zoom', required=True, metavar='A-B', help='zoom range, e.g. 8-16 (max 18, and the source\'s max)')

    source = argparse.ArgumentParser(add_help=False)
    g = source.add_argument_group('SOURCE (default --preset usgs:topo)')
    g.add_argument('--preset', metavar='P', help='usgs:topo|usgs:imagery|usgs:shaded, geoapify:<style>, '
                   'thunderforest:<style>, local:<template on localhost>')
    g.add_argument('--url', metavar='TEMPLATE', help='custom https template with {z} {x} {y} (and {r} for @2x, '
                   '{key}); needs --attribution; 1 req/s')
    g.add_argument('--y-order', choices=('xyz', 'tms', 'zyx'), default='xyz',
                   help='row numbering of --url: xyz (y down from north, default; zyx = same) or tms (y up)')
    g.add_argument('--mbtiles', metavar='PATH', help='raster MBTiles file (rows are TMS-flipped automatically)')
    g.add_argument('--key', help='API key (or env GEOAPIFY_API_KEY / THUNDERFOREST_API_KEY / L2TILES_KEY); never printed')
    g.add_argument('--ack-plan', metavar='PLAN', help='thunderforest-bulk: your Thunderforest plan allows bulk download')
    g.add_argument('--attribution', metavar='TEXT', help='data credit shown on the map (<= 96 bytes)')
    g.add_argument('--attribution-short', metavar='TEXT', help='short credit for the map corner (<= 32 bytes)')
    g.add_argument('--license-url', metavar='URL', help='shown in the credit dialog (<= 64 bytes)')

    net = argparse.ArgumentParser(add_help=False)
    g = net.add_argument_group('NET')
    g.add_argument('--rate', type=float, help='requests per second (default: the source\'s)')
    g.add_argument('--concurrency', type=int, help='parallel requests (default: the source\'s)')
    g.add_argument('--max-requests', type=int, metavar='N', help='stop cleanly after N requests (geoapify: '
                   'default 11000 per day) and print the resume command')
    g.add_argument('--timeout', type=float, default=20.0, help='per-request timeout, seconds (20)')
    g.add_argument('--contact', metavar='EMAIL_OR_URL', help='added to the User-Agent')
    g.add_argument('--cache', metavar='DIR', help='tile cache root (default %s)' % default_cache_root())
    g.add_argument('--retry-missing', action='store_true', help='ask again for tiles the cache remembers as '
                   'not available (404 / empty / HTML)')
    g.add_argument('--hidpi-split', action='store_true', help='fetch z-1 @2x tiles and cut each into 4 '
                   '(4x fewer requests; labels come out smaller)')

    conv = argparse.ArgumentParser(add_help=False)
    g = conv.add_argument_group('CONVERT')
    g.add_argument('--format', choices=('p8rle', 'rgb565'), default='p8rle',
                   help='p8rle (256-colour, compressed; default) or rgb565 (raw 128 KB per tile)')
    g.add_argument('--colors', type=int, default=256, help='palette size for p8rle, <= 256 (256)')
    g.add_argument('--filter', default='none', metavar='F', help='none | night | dim:N (N%% brightness) | gray')
    g.add_argument('--bg', default='auto', metavar='C', help='auto | #RRGGBB: colour under transparent pixels '
                   'and for missing tiles on the device')

    caps = argparse.ArgumentParser(add_help=False)
    caps.add_argument('--max-tiles', type=int, default=50000, help='refuse bigger jobs (50000; ceiling 250000)')
    caps.add_argument('--yes', '-y', action='store_true', help='do not ask "Proceed?"')

    sub.add_parser('presets', help='list tile sources, their terms and limits')
    e = sub.add_parser('estimate', parents=[area, source, net, caps], help='tiles / MB / time per zoom',
                       conflict_handler='resolve')
    e.add_argument('--format', choices=('p8rle', 'rgb565'), default='p8rle')
    e.add_argument('--colors', type=int, default=256, help=argparse.SUPPRESS)
    e.add_argument('--filter', default='none', help=argparse.SUPPRESS)
    e.add_argument('--sample', type=int, default=0, metavar='K',
                   help='convert K tiles per zoom to measure the size (fetches them if not cached)')
    sub.add_parser('fetch', parents=[area, source, net, caps], help='download into the cache (resumable)')
    b = sub.add_parser('build', parents=[area, source, net, conv, caps], help='write a map set onto the card')
    b.add_argument('--out', required=True, metavar='PATH', help='card root (e.g. /Volumes/CAIRN) or a folder')
    b.add_argument('--name', required=True, metavar='SLUG', help='folder name, [a-z0-9-]{1,24}')
    b.add_argument('--title', help='name shown on the device (<= 24 bytes; default --name)')
    b.add_argument('--no-fetch', action='store_true', help='use only tiles already in the cache')
    b.add_argument('--overwrite', action='store_true', help='replace an existing set of the same name')
    s = sub.add_parser('synth', parents=[area], help='OFFLINE test grid (no network, no licence)')
    s.add_argument('--out', required=True, metavar='PATH', help='card root (e.g. /Volumes/CAIRN) or a folder')
    s.add_argument('--name', default='l2-test', metavar='SLUG', help='folder name (l2-test)')
    s.add_argument('--title', default='Test grid', help='name shown on the device (Test grid)')
    s.add_argument('--format', choices=('p8rle', 'rgb565'), default='p8rle', help='record format (p8rle)')
    s.add_argument('--bg', default='auto', metavar='C', help='#RRGGBB background (#202124)')
    s.add_argument('--overwrite', action='store_true', help='replace an existing set of the same name')
    s.add_argument('--max-tiles', type=int, default=50000, help='refuse bigger jobs (50000)')
    v = sub.add_parser('verify', help='decode every record; check manifest / cov / index')
    v.add_argument('path', metavar='PATH/Maps/NAME')
    sub.add_parser('selftest', help='golden vectors G1-G3, R1, Mercator; exit 0/1')
    return p


def join_negative_values(argv):
    """'--bbox -97.1,...' -> '--bbox=-97.1,...' (argparse would take the value for an option)."""
    out = []
    i = 0
    while i < len(argv):
        v = argv[i]
        if v in ('--bbox', '--center') and i + 1 < len(argv) and re.match(r'^-\d', argv[i + 1]):
            out.append('%s=%s' % (v, argv[i + 1]))
            i += 2
            continue
        out.append(v)
        i += 1
    return out


def main(argv=None):
    p = build_parser()
    a = p.parse_args(join_negative_values(sys.argv[1:] if argv is None else list(argv)))
    if not a.cmd:
        p.print_help()
        return 2
    fn = {'presets': cmd_presets, 'estimate': cmd_estimate, 'fetch': cmd_fetch, 'build': cmd_build,
          'synth': cmd_synth, 'verify': cmd_verify, 'selftest': cmd_selftest}[a.cmd]
    try:
        return fn(a)
    except Fail as e:
        log('error: %s' % e)
        return 2
    except KeyboardInterrupt:
        log('\ninterrupted')
        return 130


if __name__ == '__main__':
    sys.exit(main())
