r"""
CYD-Weather screenshot capture.

Double-click: choose a screen from the menu, then capture it.
Command line:
  python screenshot.py [COM_PORT] [SCREEN] [OUTPUT_FILE]

Examples:
  python screenshot.py
  python screenshot.py COM11 now
  python screenshot.py solar
  python screenshot.py COM11 5 ScreenShots\screen_usgs.bmp

Screens: current, now/0, hourly/1, forecast/2, solar/3, fires/4, usgs/5, volcanoes/6, news/7, almanac/8, scanner/9, settings/A
Default port: COM11
"""

import os
import re
import struct
import sys
import time

try:
    import serial
except ImportError:
    print("Error: pyserial is not installed. Run: python -m pip install pyserial")
    sys.exit(1)

BAUD = 115200
DEFAULT_PORT = "COM11"
W, H = 320, 240
BASE_DIR = os.path.dirname(os.path.abspath(__file__))

SCREENS = {
    "0": ("now", b"0", "ScreenShots\\screen_now.bmp"),
    "now": ("now", b"0", "ScreenShots\\screen_now.bmp"),
    "current": ("current", None, "screen.bmp"),
    "1": ("hourly", b"1", "ScreenShots\\screen_hourly.bmp"),
    "hourly": ("hourly", b"1", "ScreenShots\\screen_hourly.bmp"),
    "2": ("forecast", b"2", "ScreenShots\\screen_forecast.bmp"),
    "forecast": ("forecast", b"2", "ScreenShots\\screen_forecast.bmp"),
    "5-day": ("forecast", b"2", "ScreenShots\\screen_forecast.bmp"),
    "5day": ("forecast", b"2", "ScreenShots\\screen_forecast.bmp"),
    "3": ("solar", b"3", "ScreenShots\\screen_solar.bmp"),
    "solar": ("solar", b"3", "ScreenShots\\screen_solar.bmp"),
    "4": ("fires", b"4", "ScreenShots\\screen_fires.bmp"),
    "fires": ("fires", b"4", "ScreenShots\\screen_fires.bmp"),
    "fire": ("fires", b"4", "ScreenShots\\screen_fires.bmp"),
    "5": ("usgs", b"5", "ScreenShots\\screen_usgs.bmp"),
    "usgs": ("usgs", b"5", "ScreenShots\\screen_usgs.bmp"),
    "quake": ("usgs", b"5", "ScreenShots\\screen_usgs.bmp"),
    "quakes": ("usgs", b"5", "ScreenShots\\screen_usgs.bmp"),
    "6": ("volcanoes", b"6", "ScreenShots\\screen_volcanoes.bmp"),
    "volcanoes": ("volcanoes", b"6", "ScreenShots\\screen_volcanoes.bmp"),
    "volcano": ("volcanoes", b"6", "ScreenShots\\screen_volcanoes.bmp"),
    "7": ("news", b"7", "ScreenShots\\screen_news.bmp"),
    "news": ("news", b"7", "ScreenShots\\screen_news.bmp"),
    "8": ("almanac", b"8", "ScreenShots\\screen_almanac.bmp"),
    "almanac": ("almanac", b"8", "ScreenShots\\screen_almanac.bmp"),
    "alm": ("almanac", b"8", "ScreenShots\\screen_almanac.bmp"),
    "9": ("scanner", b"9", "ScreenShots\\screen_scanner.bmp"),
    "scanner": ("scanner", b"9", "ScreenShots\\screen_scanner.bmp"),
    "A": ("settings", b"A", "ScreenShots\\screen_settings.bmp"),
    "a": ("settings", b"A", "ScreenShots\\screen_settings.bmp"),
    "settings": ("settings", b"A", "ScreenShots\\screen_settings.bmp"),
}

COM_RE = re.compile(r"^COM\d+$", re.IGNORECASE)


def normalize_screen(value):
    if not value:
        return SCREENS["current"]
    key = value.strip().lower()
    if key not in SCREENS:
        valid = "current, now, hourly, forecast, solar, fires, usgs, volcanoes, news, almanac, scanner, settings"
        raise ValueError(f"Unknown screen '{value}'. Valid screens: {valid}")
    return SCREENS[key]


def looks_like_output(value):
    if not value:
        return False
    return any(ch in value for ch in "\\/.") or value.lower().endswith(("bmp", "png", "raw"))


def parse_args(argv):
    port = DEFAULT_PORT
    screen_arg = None
    outfile = None
    args = list(argv)

    if args and COM_RE.match(args[0]):
        port = args.pop(0).upper()

    if args:
        if looks_like_output(args[0]) and args[0].lower() not in SCREENS:
            outfile = args.pop(0)
        else:
            screen_arg = args.pop(0)

    if args:
        outfile = args.pop(0)

    if args:
        raise ValueError("Too many arguments.")

    screen_name, screen_cmd, default_out = normalize_screen(screen_arg)
    if outfile is None:
        outfile = default_out
    return port, screen_name, screen_cmd, outfile


def ask_interactive():
    print("CYD-Weather Screenshot")
    port = input(f"COM port [{DEFAULT_PORT}]: ").strip() or DEFAULT_PORT
    print("\nScreens:")
    print("  Enter = current screen")
    print("  0 = NOW")
    print("  1 = HOURLY")
    print("  2 = 5-DAY")
    print("  3 = SOLAR")
    print("  4 = FIRES")
    print("  5 = USGS")
    print("  6 = NEWS")
    print("  7 = ALMANAC")
    print("  8 = SCANNER")
    print("  9 = SETTINGS")
    screen_arg = input("Screen to capture [current]: ").strip() or "current"
    screen_name, screen_cmd, default_out = normalize_screen(screen_arg)
    outfile = input(f"Output file [{default_out}]: ").strip() or default_out
    return port.upper(), screen_name, screen_cmd, outfile


def read_exact(ser, n, timeout=30):
    data = b""
    deadline = time.time() + timeout
    while len(data) < n and time.time() < deadline:
        chunk = ser.read(min(4096, n - len(data)))
        if chunk:
            data += chunk
            pct = len(data) * 100 // n
            print(f"  {pct}%", end="\r")
    return data


def wait_for_marker(ser, markers, timeout=20):
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = ser.read(1)
        if not c:
            continue
        buf += c
        for marker in markers:
            if buf.endswith(marker):
                return marker, buf
        if len(buf) > 512:
            buf = buf[-512:]
    return None, buf


def write_bmp(filename, pixels_bgr24):
    if not os.path.isabs(filename):
        filename = os.path.join(BASE_DIR, filename)
    folder = os.path.dirname(filename)
    if folder:
        os.makedirs(folder, exist_ok=True)

    filesize = 54 + W * H * 3
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    hdr[2:6] = struct.pack("<I", filesize)
    hdr[10] = 54
    hdr[14] = 40
    hdr[18:22] = struct.pack("<I", W)
    hdr[22:26] = struct.pack("<i", -H)
    hdr[26:28] = struct.pack("<H", 1)
    hdr[28:30] = struct.pack("<H", 24)
    with open(filename, "wb") as f:
        f.write(hdr)
        f.write(pixels_bgr24)


def open_serial_no_reset(port):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.25
    ser.write_timeout = 5
    ser.rtscts = False
    ser.dsrdtr = False

    # Set modem control lines before opening. On ESP32 dev boards, DTR/RTS
    # toggles can reset or enter bootloader mode, so keep both deasserted.
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.setDTR(False)
    ser.setRTS(False)
    return ser


def capture(port, screen_name, screen_cmd, outfile):
    print(f"Opening {port} without reset...")
    ser = open_serial_no_reset(port)

    try:
        time.sleep(0.2)
        ser.reset_input_buffer()

        print("Checking device...")
        ser.write(b"R")
        ready, _ = wait_for_marker(ser, [b"READY"], timeout=2)
        if ready:
            print("Device ready.")
        else:
            print("No READY reply; continuing with current serial session.")

        ser.reset_input_buffer()

        if screen_cmd is not None:
            print(f"Switching to {screen_name.upper()}...")
            ser.write(screen_cmd)
            ser.flush()
            time.sleep(0.6)
            ser.reset_input_buffer()
        else:
            print("Capturing current screen...")

        ser.write(b"S")
        ser.flush()
        marker, _ = wait_for_marker(ser, [b"RGB332:", b"OOM:"], timeout=20)

        if marker is None:
            raise RuntimeError("No screenshot response from device.")

        if marker == b"OOM:":
            info = ser.readline().decode("ascii", errors="replace").strip()
            raise RuntimeError(f"Device ran out of RAM while capturing: {info}")

        start = time.time()
        total = W * H
        data = read_exact(ser, total)
        if len(data) < total:
            raise RuntimeError(f"Transfer stalled at {len(data)}/{total} bytes.")

        elapsed = time.time() - start
        print(f"  Done in {elapsed:.1f}s          ")
    finally:
        ser.close()

    pixels = bytearray(W * H * 3)
    for i, c in enumerate(data):
        r3 = (c >> 5) & 0x07
        g3 = (c >> 2) & 0x07
        b2 = c & 0x03
        r8 = (r3 << 5) | (r3 << 2) | (r3 >> 1)
        g8 = (g3 << 5) | (g3 << 2) | (g3 >> 1)
        b8 = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2
        pixels[i * 3 + 0] = b8
        pixels[i * 3 + 1] = g8
        pixels[i * 3 + 2] = r8

    write_bmp(outfile, pixels)
    print(f"Saved {outfile}")


def main():
    interactive = len(sys.argv) == 1 and sys.stdin.isatty()
    try:
        if interactive:
            port, screen_name, screen_cmd, outfile = ask_interactive()
        else:
            port, screen_name, screen_cmd, outfile = parse_args(sys.argv[1:])
        capture(port, screen_name, screen_cmd, outfile)
    except Exception as exc:
        print(f"Error: {exc}")
        if not interactive:
            sys.exit(1)
    finally:
        if interactive:
            input("Press Enter to close...")


if __name__ == "__main__":
    main()
