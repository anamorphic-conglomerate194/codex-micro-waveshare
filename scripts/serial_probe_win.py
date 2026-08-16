# Windows equivalent of scripts/serial_probe.py: open a serial port, reset
# the device via DTR if requested, stream output, and look for a marker.
import argparse
import sys
import time

import serial

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument("--expect")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    expected = args.expect.encode() if args.expect is not None else None
    search_tail = b""
    found = expected is None

    ser = serial.Serial(args.port, args.baud, timeout=0.5)
    try:
        if not args.no_reset:
            ser.dtr = False
            time.sleep(0.15)
            ser.dtr = True

        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            chunk = ser.read(4096)
            if chunk:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
                if expected is not None:
                    searchable = search_tail + chunk
                    if expected in searchable:
                        found = True
                    search_tail = searchable[-len(expected):]
                continue
            time.sleep(0.05)
    finally:
        ser.close()

    if args.expect is not None:
        print("\n%s marker: %s" % (args.expect,
                                   "FOUND" if found else "NOT FOUND (timeout)"))
        return 0 if found else 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
