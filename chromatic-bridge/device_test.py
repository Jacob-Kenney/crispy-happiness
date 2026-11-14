import argparse
import json
import re
import time

from serial import Serial
from serial.tools.list_ports import comports


def wait_for(port, pattern, seconds):
    deadline = time.monotonic() + seconds
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(port.read(1024))
        if match := re.search(pattern, received):
            return bytes(match[0]), bytes(received)
    raise TimeoutError(f"No expected response over USB. Last output: {bytes(received[-300:])!r}")


def main():
    parser = argparse.ArgumentParser(description="Run the bridge self-test over Chromatic USB serial")
    parser.add_argument("--port")
    args = parser.parse_args()
    ports = [p.device for p in comports() if p.vid == 0x374E and p.pid and 0x101 <= p.pid <= 0x108]
    if not args.port and len(ports) != 1:
        parser.error("select one connected Chromatic with --port")
    with Serial(args.port or ports[0], 115200, timeout=0.25, write_timeout=2) as port:
        port.dtr = False
        port.rts = False
        for attempt in range(5):
            port.write(b"\r\nfwversion\r\n")
            try:
                version = json.loads(wait_for(port, rb'\{\s*"mcu"[^\r\n]+\}', 2.5)[0])
                break
            except TimeoutError:
                if attempt == 4:
                    raise
        print("Running firmware:", version, flush=True)
        if version.get("mcu") not in ("v1.1.12-bridge", "v1.1.12-bridge-persist"):
            raise SystemExit("The Chromatic is not running the bridge firmware")
        for attempt in range(3):
            port.write(b"bridge selftest\r\n")
            result, log = wait_for(port, rb"bridge selftest (?:PASS|FAIL):[^\r\n]+\r?\n", 25)
            print(result.decode().strip(), flush=True)
            if b"FAIL" in result:
                print(log.decode(errors="replace"), end="")
                raise SystemExit(1)
            if attempt < 2:
                time.sleep(1)


if __name__ == "__main__":
    main()
