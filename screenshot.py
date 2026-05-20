"""
CYD-Weather screenshot capture — saves all 4 screens as BMP files.
Usage:  python screenshot.py [COM_PORT]
Default port: COM11

Saves: screen_now.bmp, screen_hourly.bmp, screen_forecast.bmp, screen_settings.bmp
"""

import serial, struct, sys, time

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM11"
BAUD = 115200
W, H = 320, 240

SCREENS = [
    ('0', 'screen_now.bmp'),
    ('1', 'screen_hourly.bmp'),
    ('2', 'screen_forecast.bmp'),
    ('3', 'screen_settings.bmp'),
]

def read_exact(ser, n, timeout=30):
    data = b''
    deadline = time.time() + timeout
    while len(data) < n and time.time() < deadline:
        chunk = ser.read(min(4096, n - len(data)))
        if chunk:
            data += chunk
            pct = len(data) * 100 // n
            print(f"  {pct}%", end='\r')
    return data

def wait_for_marker(ser, markers, timeout=20):
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = ser.read(1)
        if not c:
            return None, buf
        buf += c
        for m in markers:
            if buf.endswith(m):
                return m, buf
        if len(buf) > 512:
            buf = buf[-512:]
    return None, buf

def write_bmp(filename, pixels_bgr24):
    filesize = 54 + W * H * 3
    hdr = bytearray(54)
    hdr[0:2]   = b'BM'
    hdr[2:6]   = struct.pack('<I', filesize)
    hdr[10]    = 54
    hdr[14]    = 40
    hdr[18:22] = struct.pack('<I', W)
    hdr[22:26] = struct.pack('<i', -H)   # negative = top-down
    hdr[26:28] = struct.pack('<H', 1)
    hdr[28:30] = struct.pack('<H', 24)
    with open(filename, 'wb') as f:
        f.write(hdr)
        f.write(pixels_bgr24)

def capture(ser, filename):
    ser.reset_input_buffer()
    ser.write(b'S')

    marker, _ = wait_for_marker(ser, [b'RGB332:', b'BMP:', b'OOM:'])

    if marker is None:
        print("  No response from device.")
        return False

    if marker == b'OOM:':
        info = ser.readline().decode('ascii', errors='replace').strip()
        print(f"  OOM: {info}")
        return False

    start = time.time()

    if marker == b'RGB332:':
        # 8-bit RGB332: 1 byte per pixel, RRRGGGBB
        total = W * H
        data = read_exact(ser, total)
        if len(data) < total:
            print(f"  Transfer stalled at {len(data)}/{total} bytes.")
            return False
        elapsed = time.time() - start
        print(f"  Done in {elapsed:.1f}s          ")
        # Convert RGB332 (RRRGGGBB) -> BGR24 for BMP
        pixels = bytearray(W * H * 3)
        for i, c in enumerate(data):
            r3 = (c >> 5) & 0x07
            g3 = (c >> 2) & 0x07
            b2 = c & 0x03
            r8 = (r3 << 5) | (r3 << 2) | (r3 >> 1)
            g8 = (g3 << 5) | (g3 << 2) | (g3 >> 1)
            b8 = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2
            pixels[i*3+0] = b8
            pixels[i*3+1] = g8
            pixels[i*3+2] = r8

    else:  # BMP: — legacy 16-bit RGB565 path
        total = W * H * 2
        data = read_exact(ser, total)
        if len(data) < total:
            print(f"  Transfer stalled at {len(data)}/{total} bytes.")
            return False
        elapsed = time.time() - start
        print(f"  Done in {elapsed:.1f}s          ")
        pixels = bytearray(W * H * 3)
        for i in range(W * H):
            c = struct.unpack_from('<H', data, i * 2)[0]
            pixels[i*3+0] = (c & 0x1F) << 3           # B
            pixels[i*3+1] = ((c >> 5) & 0x3F) << 2    # G
            pixels[i*3+2] = (c >> 11) << 3             # R

    write_bmp(filename, pixels)
    print(f"  Saved {filename}")
    return True

print(f"Opening {PORT} (no reset)...")
try:
    ser = serial.Serial(PORT, BAUD, timeout=20, dsrdtr=False, rtscts=False)
    ser.dtr = False
    ser.rts = False
except serial.SerialException as e:
    print(f"Error: {e}")
    sys.exit(1)

time.sleep(0.3)
ser.reset_input_buffer()

for screen_cmd, filename in SCREENS:
    print(f"Screen {screen_cmd} -> {filename}")
    ser.write(screen_cmd.encode())
    time.sleep(0.8)
    if not capture(ser, filename):
        print(f"  FAILED -- skipping")

ser.close()
print("\nDone. Open the .bmp files in Windows Photos or Paint.")
