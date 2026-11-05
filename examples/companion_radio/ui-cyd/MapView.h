#pragma once

// Offline raster map for the CYD colour UI. Reads the card format of Cairn's
// tools/maptiles.py (/Maps/<set>/manifest.txt + <z>/<bx>_<by>.l2t block files, P8RLE or
// RGB565 256x256 tiles). Heap-allocated while the map screen is open; tiles stream one
// row at a time, so the working set is a few KB plus the SD/FAT mount.

#include <Arduino.h>
#include <SD.h>
#include "CYDDisplay.h"

#define MAP_MAX_SETS 8

struct MapSet {
  char name[25];
  char title[25];
  char attr[33];
  uint8_t fmt;        // 1 P8RLE, 2 RGB565
  uint8_t minz, maxz;
  float w, s, e, n;
  uint16_t bg;
};

class MapView {
public:
  MapView(LGFX_CYD* g, int top, int bottom);
  ~MapView();

  bool begin();                       // mount the card and scan /Maps
  const char* error() const { return _err; }

  void setCenter(double lat, double lon);
  bool centerFromSets(double* lat, double* lon) const;
  void zoomBy(int d);
  void panBy(int dx, int dy);         // screen pixels; content follows the finger
  void invalidate();                  // re-plan and re-render every visible tile
  bool busy() const { return _next < _total; }
  bool step();                        // render one tile; true when the frame just completed
  bool project(double lat, double lon, int* x, int* y) const;   // false when off the map area

  int zoom() const { return _z; }
  const MapSet* activeSet() const { return _active >= 0 ? &_sets[_active] : NULL; }
  const char* status() const { return _status; }   // "" or "No map for this area" etc.
  int setCount() const { return _nsets; }

  bool follow = true;                 // keep own position centred

private:
  LGFX_CYD* _g;
  int _top, _bottom;
  MapSet _sets[MAP_MAX_SETS];
  int _nsets = 0, _active = -1;
  bool _mounted = false;
  const char* _err = "";
  const char* _status = "";

  double _cx = 0, _cy = 0;            // view centre in world pixels at zoom _z
  int _z = 12;
  int _tx0, _ty0, _tcols, _next = 0, _total = 0;

  // open block file cache
  File _blk;
  int _blkSet = -1, _blkZ = -1;
  uint32_t _blkBx = 0, _blkBy = 0;
  bool _blkMissing = false;

  // buffered reader over _blk
  uint8_t _rbuf[1024];
  int _rpos = 0, _rlen = 0;
  uint32_t _rleft = 0;                // record bytes not yet pulled into _rbuf

  uint16_t _pal[256];
  uint8_t _idx[256];
  uint16_t _line[256];

  bool loadManifest(const char* dir, MapSet& m);
  void pickSet();
  bool openBlock(int z, uint32_t bx, uint32_t by);
  bool rdBegin(uint32_t offset, uint32_t len);
  bool rdFill();
  int  rdByte();
  bool rdRead(uint8_t* dst, int n);
  bool rdSkip(uint32_t n);
  bool drawTile(int tx, int ty, int sx, int sy);
  void fillClip(int x, int y, int w, int h, uint16_t c);
};
