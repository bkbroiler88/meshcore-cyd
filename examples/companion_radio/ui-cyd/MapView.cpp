#include "MapView.h"
#include "ui_theme.h"
#include "target.h"
#include <helpers/TxtDataHelpers.h>
#include <math.h>

#define MAP_NO_SET_BG 0x2104
#define SD_FREQ 16000000

static double lon2x(double lon, int z) { return (lon + 180.0) / 360.0 * (256.0 * (double)(1UL << z)); }
static double lat2y(double lat, int z) {
  if (lat > 85.0511) lat = 85.0511;
  if (lat < -85.0511) lat = -85.0511;
  double s = sin(lat * M_PI / 180.0);
  return (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * (256.0 * (double)(1UL << z));
}
static double x2lon(double x, int z) { return x / (256.0 * (double)(1UL << z)) * 360.0 - 180.0; }
static double y2lat(double y, int z) {
  double n = M_PI - 2.0 * M_PI * y / (256.0 * (double)(1UL << z));
  return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

static uint16_t parseHex565(const char* v) {
  if (*v == '#') v++;
  uint32_t c = strtoul(v, NULL, 16);
  return rgb565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

MapView::MapView(LGFX_CYD* g, int top, int bottom) : _g(g), _top(top), _bottom(bottom) {
  memset(_sets, 0, sizeof(_sets));
}

MapView::~MapView() {
  if (_blk) _blk.close();
  if (_mounted) SD.end();
}

// ---------------------------------------------------------------- card + manifests

bool MapView::loadManifest(const char* dir, MapSet& m) {
  char path[64];
  snprintf(path, sizeof(path), "/Maps/%s/manifest.txt", dir);
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  bool ok_magic = false, ok_bounds = false;
  m.fmt = 0; m.minz = 0; m.maxz = 0; m.bg = MAP_NO_SET_BG;
  m.title[0] = 0; m.attr[0] = 0;
  StrHelper::strncpy(m.name, dir, sizeof(m.name));
  char line[256];
  int n = 0, total = 0;
  while (total < 4096) {
    int c = f.read();
    if (c >= 0) total++;
    if (c < 0 || c == '\n') {
      line[n] = 0; n = 0;
      char* eq = strchr(line, '=');
      if (line[0] != '#' && eq) {
        *eq = 0;
        const char* k = line; const char* v = eq + 1;
        if (!strcmp(k, "l2map")) ok_magic = (atoi(v) == 1);
        else if (!strcmp(k, "format")) m.fmt = !strcmp(v, "p8rle") ? 1 : (!strcmp(v, "rgb565") ? 2 : 0);
        else if (!strcmp(k, "minzoom")) m.minz = atoi(v);
        else if (!strcmp(k, "maxzoom")) m.maxz = atoi(v);
        else if (!strcmp(k, "bounds")) ok_bounds = sscanf(v, "%f,%f,%f,%f", &m.w, &m.s, &m.e, &m.n) == 4;
        else if (!strcmp(k, "title")) StrHelper::strncpy(m.title, v, sizeof(m.title));
        else if (!strcmp(k, "attribution_short")) StrHelper::strncpy(m.attr, v, sizeof(m.attr));
        else if (!strcmp(k, "attribution") && !m.attr[0]) StrHelper::strncpy(m.attr, v, sizeof(m.attr));
        else if (!strcmp(k, "bg")) m.bg = parseHex565(v);
      }
      if (c < 0) break;
    } else if (n < (int)sizeof(line) - 1) {
      line[n++] = (char)c;
    }
  }
  f.close();
  if (!m.title[0]) StrHelper::strncpy(m.title, m.name, sizeof(m.title));
  return ok_magic && ok_bounds && m.fmt && m.maxz <= 18 && m.minz <= m.maxz;
}

bool MapView::begin() {
  if (!SD.begin(SDCARD_CS, cyd_vspi(), SD_FREQ, "/sd", 2)) { _err = "No SD card"; return false; }
  _mounted = true;
  File root = SD.open("/Maps");
  if (!root || !root.isDirectory()) { _err = "No /Maps folder on card"; return false; }
  for (File e = root.openNextFile(); e && _nsets < MAP_MAX_SETS; e = root.openNextFile()) {
    const char* nm = e.name();
    const char* slash = strrchr(nm, '/');
    if (slash) nm = slash + 1;
    bool dir = e.isDirectory();
    char name[25];
    StrHelper::strncpy(name, nm, sizeof(name));
    e.close();
    if (!dir || name[0] == '.' || strlen(nm) > 24) continue;
    if (loadManifest(name, _sets[_nsets])) _nsets++;
  }
  root.close();
  if (_nsets == 0) { _err = "No map sets in /Maps"; return false; }
  return true;
}

bool MapView::centerFromSets(double* lat, double* lon) const {
  if (_nsets == 0) return false;
  *lat = (_sets[0].s + _sets[0].n) / 2.0;
  *lon = (_sets[0].w + _sets[0].e) / 2.0;
  return true;
}

// ---------------------------------------------------------------- view

void MapView::setCenter(double lat, double lon) {
  _cx = lon2x(lon, _z);
  _cy = lat2y(lat, _z);
}

void MapView::zoomBy(int d) {
  int lo = 1, hi = 20;
  if (_nsets) {
    lo = 18; hi = 0;
    for (int i = 0; i < _nsets; i++) {
      if (_sets[i].minz < lo) lo = _sets[i].minz;
      if (_sets[i].maxz + 2 > hi) hi = _sets[i].maxz + 2;
    }
    if (lo > 1) lo--;    // one level out past the data so "Zoom in for map" is reachable
  }
  int nz = _z + d;
  if (nz < lo) nz = lo;
  if (nz > hi) nz = hi;
  while (nz > _z) { _cx *= 2; _cy *= 2; _z++; }
  while (nz < _z) { _cx /= 2; _cy /= 2; _z--; }
}

void MapView::panBy(int dx, int dy) {
  double world = 256.0 * (double)(1UL << _z);
  _cx = constrain(_cx - dx, 0.0, world);
  _cy = constrain(_cy - dy, 0.0, world);
  follow = false;
}

void MapView::pickSet() {
  double lat = y2lat(_cy, _z), lon = x2lon(_cx, _z);
  int best = -1, bestDz = -1;
  bool covered = false;
  for (int i = 0; i < _nsets; i++) {
    const MapSet& m = _sets[i];
    if (lon < m.w || lon > m.e || lat < m.s || lat > m.n) continue;
    covered = true;
    if (_z < m.minz || _z > m.maxz + 2) continue;
    int dz = _z < m.maxz ? _z : m.maxz;
    if (dz > bestDz) { bestDz = dz; best = i; }
  }
  if (best != _active && _blk) { _blk.close(); _blkSet = -1; }
  _active = best;
  _status = best >= 0 ? "" : (covered ? "Zoom in for map" : "No map for this area");
}

void MapView::invalidate() {
  pickSet();
  int viewH = _bottom - _top;
  double left = _cx - GW / 2.0, topw = _cy - viewH / 2.0;
  _tx0 = (int)floor(left / 256.0);
  _ty0 = (int)floor(topw / 256.0);
  int tx1 = (int)floor((left + GW - 1) / 256.0);
  int ty1 = (int)floor((topw + viewH - 1) / 256.0);
  _tcols = tx1 - _tx0 + 1;
  _total = _tcols * (ty1 - _ty0 + 1);
  _next = 0;
}

bool MapView::project(double lat, double lon, int* x, int* y) const {
  int viewH = _bottom - _top;
  double px = lon2x(lon, _z) - (_cx - GW / 2.0);
  double py = lat2y(lat, _z) - (_cy - viewH / 2.0);
  *x = (int)floor(px);
  *y = _top + (int)floor(py);
  return *x >= 0 && *x < GW && *y >= _top && *y < _bottom;
}

bool MapView::step() {
  if (_next >= _total) return false;
  int i = _next++;
  int tx = _tx0 + i % _tcols, ty = _ty0 + i / _tcols;
  int viewH = _bottom - _top;
  int sx = (int)floor(tx * 256.0 - (_cx - GW / 2.0));
  int sy = _top + (int)floor(ty * 256.0 - (_cy - viewH / 2.0));
  if (_active < 0) fillClip(sx, sy, 256, 256, MAP_NO_SET_BG);
  else drawTile(tx, ty, sx, sy);
  return _next == _total;
}

void MapView::fillClip(int x, int y, int w, int h, uint16_t c) {
  int x1 = x + w, y1 = y + h;
  if (x < 0) x = 0;
  if (y < _top) y = _top;
  if (x1 > GW) x1 = GW;
  if (y1 > _bottom) y1 = _bottom;
  if (x1 > x && y1 > y) _g->fillRect(x, y, x1 - x, y1 - y, c);
}

// ---------------------------------------------------------------- block files + reader

bool MapView::openBlock(int z, uint32_t bx, uint32_t by) {
  if (_blkSet == _active && _blkZ == z && _blkBx == bx && _blkBy == by) return !_blkMissing;
  if (_blk) _blk.close();
  _blkSet = _active; _blkZ = z; _blkBx = bx; _blkBy = by;
  char path[72];
  snprintf(path, sizeof(path), "/Maps/%s/%d/%lu_%lu.l2t", _sets[_active].name, z, (unsigned long)bx, (unsigned long)by);
  _blkMissing = !SD.exists(path);
  if (!_blkMissing) {
    _blk = SD.open(path, FILE_READ);
    uint8_t h[16];
    _blkMissing = !_blk || _blk.read(h, 16) != 16 || memcmp(h, "L2T1", 4) != 0 || h[4] != z;
    if (_blkMissing && _blk) _blk.close();
  }
  return !_blkMissing;
}

bool MapView::rdBegin(uint32_t offset, uint32_t len) {
  _rpos = _rlen = 0;
  _rleft = len;
  return _blk.seek(offset);
}

bool MapView::rdFill() {
  if (_rleft == 0) return false;
  int want = _rleft < sizeof(_rbuf) ? (int)_rleft : (int)sizeof(_rbuf);
  int got = _blk.read(_rbuf, want);
  if (got <= 0) { _rleft = 0; return false; }
  _rleft -= got; _rlen = got; _rpos = 0;
  return true;
}

int MapView::rdByte() {
  if (_rpos >= _rlen && !rdFill()) return -1;
  return _rbuf[_rpos++];
}

bool MapView::rdRead(uint8_t* dst, int n) {
  while (n > 0) {
    if (_rpos >= _rlen && !rdFill()) return false;
    int k = _rlen - _rpos;
    if (k > n) k = n;
    memcpy(dst, _rbuf + _rpos, k);
    _rpos += k; dst += k; n -= k;
  }
  return true;
}

bool MapView::rdSkip(uint32_t n) {
  uint32_t inBuf = (uint32_t)(_rlen - _rpos);
  if (n <= inBuf) { _rpos += n; return true; }
  n -= inBuf;
  _rpos = _rlen = 0;
  if (n > _rleft) return false;
  _rleft -= n;
  return _blk.seek(_blk.position() + n);
}

// One display tile at view zoom _z, top-left at screen (sx, sy). Past the set's maxzoom the
// parent tile's sub-square is magnified (k = 2 or 4).
bool MapView::drawTile(int tx, int ty, int sx, int sy) {
  const MapSet& m = _sets[_active];
  uint32_t n = 1UL << _z;
  if (tx < 0 || ty < 0 || (uint32_t)tx >= n || (uint32_t)ty >= n) { fillClip(sx, sy, 256, 256, m.bg); return false; }
  int dz = _z < m.maxz ? _z : m.maxz;
  int sh = _z - dz, k = 1 << sh, span = 256 >> sh;
  uint32_t stx = (uint32_t)tx >> sh, sty = (uint32_t)ty >> sh;
  int ox = (tx & (k - 1)) * span, oy = (ty & (k - 1)) * span;

  int j0 = sx < 0 ? -sx : 0;
  int j1 = (GW - sx) < 256 ? (GW - sx) : 256;
  int i0 = (_top - sy) > 0 ? (_top - sy) : 0;               // first visible display row
  int i1 = (_bottom - sy) < 256 ? (_bottom - sy) : 256;
  if (j1 <= j0 || i1 <= i0) return true;
  int r0 = oy + i0 / k, r1 = oy + (i1 - 1) / k;             // source rows that are needed

  if (!openBlock(dz, stx >> 4, sty >> 4)) { fillClip(sx, sy, 256, 256, m.bg); return false; }
  uint8_t ent[8];
  int slot = ((sty & 15) << 4) | (stx & 15);
  if (!_blk.seek(16 + slot * 8) || _blk.read(ent, 8) != 8) { fillClip(sx, sy, 256, 256, m.bg); return false; }
  uint32_t sector = ent[0] | (ent[1] << 8) | (ent[2] << 16) | ((uint32_t)ent[3] << 24);
  uint32_t bytes  = ent[4] | (ent[5] << 8) | (ent[6] << 16) | ((uint32_t)ent[7] << 24);
  if (sector == 0 || bytes == 0 || !rdBegin(sector * 512UL, bytes)) { fillClip(sx, sy, 256, 256, m.bg); return false; }

  lgfx::rgb565_t* line = (lgfx::rgb565_t*)_line;
  int nc = 0;
  if (m.fmt == 1) {
    uint8_t h[4];
    if (!rdRead(h, 4) || h[0] != 'P' || h[1] != 1) goto corrupt;
    nc = h[2] | (h[3] << 8);
    if (nc < 1 || nc > 256) goto corrupt;
    for (int c = 0; c < nc; c++) {
      uint8_t p[2];
      if (!rdRead(p, 2)) goto corrupt;
      _pal[c] = (p[0] << 8) | p[1];
    }
  }

  for (int r = 0; r <= r1; r++) {
    bool need = r >= r0;
    if (m.fmt == 1) {
      uint8_t lb[2];
      if (!rdRead(lb, 2)) goto corrupt;
      int rl = lb[0] | (lb[1] << 8);
      if (!need) { if (!rdSkip(rl)) goto corrupt; continue; }
      int o = 0, used = 0;
      while (used < rl && o < 256) {
        int c = rdByte(); used++;
        if (c < 0) goto corrupt;
        if (c < 0x80) {
          int cnt = c + 1;
          if (o + cnt > 256 || used + cnt > rl) goto corrupt;
          if (!rdRead(_idx + o, cnt)) goto corrupt;
          o += cnt; used += cnt;
        } else {
          int cnt = c - 125;
          int v = rdByte(); used++;
          if (v < 0 || o + cnt > 256) goto corrupt;
          memset(_idx + o, v, cnt); o += cnt;
        }
      }
      if (o != 256 || used != rl) goto corrupt;
      for (int j = j0; j < j1; j++) {
        uint8_t ix = _idx[ox + j / k];
        line[j].raw = ix < nc ? _pal[ix] : m.bg;
      }
    } else {
      if (!need) { if (!rdSkip(512)) goto corrupt; continue; }
      uint8_t* raw = (uint8_t*)_pal;                     // palette unused for RGB565
      if (!rdSkip(ox * 2) || !rdRead(raw, span * 2) || !rdSkip((256 - ox - span) * 2)) goto corrupt;
      for (int j = j0; j < j1; j++) {
        int c = j / k;
        line[j].raw = (raw[2 * c] << 8) | raw[2 * c + 1];
      }
    }
    for (int rr = 0; rr < k; rr++) {
      int i = (r - oy) * k + rr;
      if (i < i0 || i >= i1) continue;
      _g->pushImage(sx + j0, sy + i, j1 - j0, 1, line + j0);
    }
  }
  return true;

corrupt:
  fillClip(sx, sy, 256, 256, m.bg);
  return false;
}
