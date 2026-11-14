"""Send a bounded input pulse to the real Chromatic through the UDP bridge."""
import argparse
import json
import socket
import time

BUTTONS = ("left", "right", "up", "down", "a", "b", "start", "select")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("buttons", nargs="*", choices=BUTTONS)
    parser.add_argument("--seconds", type=float, default=0.5)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    if not 0 < args.seconds <= 5:
        parser.error("--seconds must be greater than zero and at most 5")
    sequence = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect(("127.0.0.1", args.port))
        sock.settimeout(0.1)

        def send(buttons):
            nonlocal sequence
            sequence += 1
            sock.send(json.dumps({"sequence": sequence, "buttons": buttons}).encode())

        def receive():
            try:
                return json.loads(sock.recv(4096))
            except socket.timeout:
                return None

        try:
            print("Waiting for live ROM telemetry…", flush=True)
            deadline = time.monotonic() + 15
            before = None
            while time.monotonic() < deadline:
                send([])
                state = receive()
                if state and state["ageMs"] < 3000:
                    before = state
                    break
            if before is None:
                raise RuntimeError("No live game state. Check the Shiny cable room and bridge.")
            print(f'Connected: frame={before["frame"]}, ship={before["ship"]}, paused={before["paused"]}')
            print(f'Pressing {args.buttons or ["nothing"]} for {args.seconds}s, then releasing.', flush=True)
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                send(args.buttons)
                receive()
                time.sleep(0.025)
            # Continue sending releases while allowing delayed cable replies to arrive.
            deadline = time.monotonic() + 6
            seen = set()
            while time.monotonic() < deadline:
                send([])
                state = receive()
                if state and state["sequence"] not in seen:
                    seen.add(state["sequence"])
                    print(f'frame={state["frame"]} remoteKeys={state["remoteKeys"]} ship={state["ship"]} paused={state["paused"]}', flush=True)
                time.sleep(0.025)
        except (ConnectionRefusedError, OSError) as error:
            raise SystemExit(f"Bridge unavailable at 127.0.0.1:{args.port}: {error}") from error
        except RuntimeError as error:
            raise SystemExit(str(error)) from error
        finally:
            try:
                send([])
            except OSError:
                pass


if __name__ == "__main__":
    main()
