"""Minimal UDP agent: observe real ROM state and steer toward the canyon center."""
import json
import socket
import time


def main():
    sequence = 0
    buttons = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect(("127.0.0.1", 8765))
        sock.settimeout(0.1)
        while True:
            sock.send(json.dumps({"sequence": sequence, "buttons": buttons}).encode())
            sequence += 1
            try:
                state = json.loads(sock.recv(4096))
                buttons = []
                if state["playing"] and state["ageMs"] < 500:
                    ship = state["ship"]
                    row = min(17, (ship["y"] + 16) // 8)
                    target = state["canyon"][row]["left"] + 16
                    center = ship["x"] + 12
                    if center < target - 2:
                        buttons.append("right")
                    elif center > target + 2:
                        buttons.append("left")
                print(f'frame={state["frame"]} score={state["score"]} buttons={buttons}')
            except socket.timeout:
                buttons = []
            time.sleep(0.05)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
