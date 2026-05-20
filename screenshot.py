"""
CYD-Weather screenshot capture — captures whatever screen is currently showing.
Usage:  python screenshot.py [COM_PORT] [OUTPUT_FILE]
Default port: COM11
Default file: screen.bmp
"""

import serial, struct, sys, time

PORT     = sys.argv[1] if len(sys.argv) > 1 else "COM11"
OUTFILE  = sys.argv[2] if len(sys.argv) > 2 else "screen.bmp"
BAUD     = 115200
W, H     = 320, 240

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
    hdr[22:26] = struct.pack('<i', -H)
    hdr[26:28] = struct.pack('<H', 1)
    hdr[28:30] = struct.pack('<H', 24)
    with open(filename, 'wb') as f:
        f.write(hdr)
        f.write(pixels_bgr24)

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

# Send 'R' — firmware responds READY after setup() completes.
# If device is already running, READY comes back within milliseconds.
# If device is still booting, we wait up to 90s.
print("Waiting for device (send R)...")
ser.write(b'R')
ready, _ = wait_for_marker(ser, [b'READY'], timeout=90)
if ready:
    print("Device ready.")
else:
    print("No READY received — proceeding anyway.")
time.sleep(0.3)
ser.reset_input_buffer()

ser.write(b'S')

marker, _ = wait_for_marker(ser, [b'RGB332:', b'OOM:'], timeout=20)

if marker is None:
    print("No response from device.")
    ser.close()
    sys.exit(1)

if marker == b'OOM:':
    info = ser.readline().decode('ascii', errors='replace').strip()
    print(f"OOM: {info}")
    ser.close()
    sys.exit(1)

start = time.time()
total = W * H
data = read_exact(ser, total)
ser.close()

if len(data) < total:
    print(f"Transfer stalled at {len(data)}/{total} bytes.")
    sys.exit(1)

elapsed = time.time() - start
print(f"  Done in {elapsed:.1f}s          ")

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

write_bmp(OUTFILE, pixels)
print(f"Saved {OUTFILE}")
