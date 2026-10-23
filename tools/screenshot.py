#!/usr/bin/env python3
"""Grab a screenshot from a CYD running the colour UI.

The firmware streams "SCREENSHOT <w> <h>" + newline, then w*h RGB565 big-endian pixels,
then "SCREENSHOT_END". Trigger it either by long-pressing the node name in the status bar
(companion and repeater) or, on the repeater, with the serial command "screenshot".
This script waits for the stream, converts it to PNG (no external deps) and saves it.

usage: python tools/screenshot.py COM3 docs/images/repeater-status.png [timeout_s] [serial_command]
"""
import serial, struct, sys, time, zlib

CR = chr(13)
LF = chr(10)


def png(path, w, h, rgb565):
    raw = bytearray()
    for y in range(h):
        raw.append(0)   # filter: none
        row = rgb565[y * w * 2:(y + 1) * w * 2]
        for i in range(0, len(row), 2):
            v = (row[i] << 8) | row[i + 1]   # readRect returns the panel's big-endian order
            r = (v >> 11) & 31
            g = (v >> 5) & 63
            b = v & 31
            raw += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    sig = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
    out = sig + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(out)


def main():
    port = sys.argv[1]
    path = sys.argv[2]
    timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 60
    cmd = sys.argv[4] if len(sys.argv) > 4 else None
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 1
    s.dtr = False
    s.rts = False     # do not reset the board on open
    s.open()
    if cmd:
        s.read(4096)
        s.write((cmd + CR).encode())
        print("sent", cmd)
    print("waiting for SCREENSHOT header...")
    buf = b''
    end = time.time() + timeout
    marker = b'SCREENSHOT '
    nl = LF.encode()
    while time.time() < end:
        buf += s.read(4096)
        i = buf.find(marker)
        if i >= 0 and nl in buf[i:]:
            j = buf.index(nl, i)
            hdr = buf[i:j].decode(errors='replace').split()
            if len(hdr) == 3:
                w, h = int(hdr[1]), int(hdr[2])
                data = bytearray(buf[j + 1:])
                need = w * h * 2
                while len(data) < need and time.time() < end:
                    data += s.read(min(65536, need - len(data)))
                s.close()
                if len(data) < need:
                    print("short read: %d of %d bytes" % (len(data), need))
                    sys.exit(1)
                png(path, w, h, data[:need])
                print("saved %s (%dx%d)" % (path, w, h))
                return
    s.close()
    print("timed out")
    sys.exit(1)


if __name__ == '__main__':
    main()
