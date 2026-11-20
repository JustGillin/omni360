#!/usr/bin/env python3
"""PC-side WebSocket bridge for the Omni360 Xbox 360 input plugin."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass, field


DEFAULT_DATA_PORT = 36062
CONTROL_PORT = 36063
SLOT_COUNT = 4
SEND_INTERVAL = 1.0 / 50.0
MAX_HANDSHAKE = 8192

BUTTON_ALIASES = {
    "DPAD_UP": "UP",
    "DPAD_DOWN": "DOWN",
    "DPAD_LEFT": "LEFT",
    "DPAD_RIGHT": "RIGHT",
    "SELECT": "BACK",
    "MENU": "START",
    "HOME": "GUIDE",
}
FACE_BITS = {"X": 0x10, "A": 0x20, "B": 0x40, "Y": 0x80}
MISC_BITS = {
    "LB": 0x01,
    "RB": 0x02,
    "LT": 0x04,
    "RT": 0x08,
    "BACK": 0x10,
    "START": 0x20,
    "L3": 0x40,
    "R3": 0x80,
}
VALID_BUTTONS = set(FACE_BITS) | set(MISC_BITS) | {
    "UP", "DOWN", "LEFT", "RIGHT", "GUIDE"
}
AXIS_RANGES = {
    "LX": (-1.0, 1.0),
    "LY": (-1.0, 1.0),
    "RX": (-1.0, 1.0),
    "RY": (-1.0, 1.0),
    "LT": (0.0, 1.0),
    "RT": (0.0, 1.0),
}


def normalize_button(value: object) -> str:
    name = str(value).strip().upper().replace("-", "_")
    name = BUTTON_ALIASES.get(name, name)
    if name not in VALID_BUTTONS:
        raise ValueError("unknown button: " + str(value))
    return name


def encode_axis(value: float) -> int:
    return max(0, min(255, int(round((value + 1.0) * 127.5))))


def encode_trigger(value: float) -> int:
    return max(0, min(255, int(round(value * 255.0))))


@dataclass
class SlotState:
    active: bool = False
    held: set[str] = field(default_factory=set)
    timed: dict[str, float] = field(default_factory=dict)
    axes: dict[str, float] = field(
        default_factory=lambda: {"LX": 0.0, "LY": 0.0, "RX": 0.0, "RY": 0.0,
                                 "LT": 0.0, "RT": 0.0}
    )

    def clear(self) -> None:
        self.held.clear()
        self.timed.clear()
        self.axes = {"LX": 0.0, "LY": 0.0, "RX": 0.0, "RY": 0.0,
                     "LT": 0.0, "RT": 0.0}

    def report(self, slot: int, now: float) -> bytes:
        for name, expires in list(self.timed.items()):
            if expires <= now:
                del self.timed[name]
        buttons = self.held | set(self.timed)

        face = 0
        for name, bit in FACE_BITS.items():
            if name in buttons:
                face |= bit

        up, right, down, left = (name in buttons for name in ("UP", "RIGHT", "DOWN", "LEFT"))
        if up and right:
            hat = 1
        elif down and right:
            hat = 3
        elif down and left:
            hat = 5
        elif up and left:
            hat = 7
        elif up:
            hat = 0
        elif right:
            hat = 2
        elif down:
            hat = 4
        elif left:
            hat = 6
        else:
            hat = 8

        misc = 0
        for name, bit in MISC_BITS.items():
            if name in buttons:
                misc |= bit
        left_trigger = max(self.axes["LT"], 1.0 if "LT" in buttons else 0.0)
        right_trigger = max(self.axes["RT"], 1.0 if "RT" in buttons else 0.0)
        if left_trigger >= 0.5:
            misc |= MISC_BITS["LT"]
        if right_trigger >= 0.5:
            misc |= MISC_BITS["RT"]
        guide = 0x01 if "GUIDE" in buttons else 0

        # The plugin consumes ten report bytes: slot, four axes, d-pad/face
        # buttons, shoulder/menu/stick buttons, Guide, and two triggers.
        return bytes((
            slot,
            encode_axis(self.axes["LX"]),
            encode_axis(-self.axes["LY"]),
            encode_axis(self.axes["RX"]),
            encode_axis(-self.axes["RY"]),
            (hat & 0x0F) | face,
            misc,
            guide,
            encode_trigger(left_trigger),
            encode_trigger(right_trigger),
        ))


class BridgeState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.slots = [SlotState() for _ in range(SLOT_COUNT)]
        self.disconnect_pending: set[int] = set()
        self.connected = False

    def apply(self, command: dict[str, object]) -> dict[str, object]:
        action = str(command.get("cmd", "")).lower()
        if action == "status":
            with self.lock:
                return {
                    "ok": True,
                    "connected": self.connected,
                    "active_slots": [i for i, slot in enumerate(self.slots) if slot.active],
                }

        try:
            slot_index = int(command.get("slot", 0))
            if slot_index < 0 or slot_index >= SLOT_COUNT:
                raise ValueError("slot must be from 0 to 3")
            with self.lock:
                slot = self.slots[slot_index]
                if action in ("press", "release", "tap"):
                    raw_buttons = command.get("buttons", [])
                    if not isinstance(raw_buttons, list) or not raw_buttons:
                        raise ValueError("provide one or more buttons")
                    buttons = [normalize_button(button) for button in raw_buttons]
                    slot.active = True
                    now = time.monotonic()
                    if action == "press":
                        for button in buttons:
                            slot.held.add(button)
                            slot.timed.pop(button, None)
                    elif action == "release":
                        for button in buttons:
                            slot.held.discard(button)
                            slot.timed.pop(button, None)
                    else:
                        duration_ms = int(command.get("ms", 120))
                        if duration_ms < 10 or duration_ms > 5000:
                            raise ValueError("tap duration must be 10..5000 ms")
                        for button in buttons:
                            slot.timed[button] = now + duration_ms / 1000.0
                elif action == "axis":
                    name = str(command.get("axis", "")).strip().upper()
                    if name not in AXIS_RANGES:
                        raise ValueError("axis must be LX, LY, RX, RY, LT, or RT")
                    try:
                        value = float(command["value"])
                    except (KeyError, TypeError, ValueError):
                        raise ValueError("axis value must be numeric")
                    low, high = AXIS_RANGES[name]
                    if value < low or value > high:
                        raise ValueError("%s value must be %s..%s" % (name, low, high))
                    slot.active = True
                    slot.axes[name] = value
                elif action == "neutral":
                    slot.active = True
                    slot.clear()
                elif action == "disconnect":
                    slot.clear()
                    slot.active = False
                    self.disconnect_pending.add(slot_index)
                else:
                    raise ValueError("unknown command")
            return {"ok": True, "slot": slot_index, "command": action}
        except (TypeError, ValueError) as error:
            return {"ok": False, "error": str(error)}

    def frames(self) -> list[str]:
        now = time.monotonic()
        with self.lock:
            result = []
            for slot_index in sorted(self.disconnect_pending):
                message = "1%d80807f7f0800000000" % slot_index
                result.extend([message, message, message])
            self.disconnect_pending.clear()
            for slot_index, slot in enumerate(self.slots):
                if slot.active:
                    result.append(slot.report(slot_index, now).hex())
            return result

    def disconnect_frames(self) -> list[str]:
        with self.lock:
            return ["1%d80807f7f0800000000" % index
                    for index, slot in enumerate(self.slots) if slot.active
                    for _ in range(3)]


def websocket_accept(key: str) -> str:
    digest = hashlib.sha1(
        (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
    ).digest()
    return base64.b64encode(digest).decode("ascii")


def websocket_text_frame(message: str) -> bytes:
    payload = message.encode("ascii")
    if len(payload) >= 126:
        raise ValueError("bridge frame is unexpectedly long")
    return bytes((0x81, len(payload))) + payload


def read_handshake(connection: socket.socket) -> bytes:
    request = bytearray()
    while b"\r\n\r\n" not in request:
        chunk = connection.recv(1024)
        if not chunk:
            raise ConnectionError("console closed during WebSocket handshake")
        request.extend(chunk)
        if len(request) > MAX_HANDSHAKE:
            raise ValueError("WebSocket request header is too large")
    return bytes(request)


def perform_handshake(connection: socket.socket) -> None:
    request = read_handshake(connection).decode("latin1")
    headers: dict[str, str] = {}
    for line in request.split("\r\n")[1:]:
        if ":" in line:
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()
    key = headers.get("sec-websocket-key")
    if not key:
        raise ValueError("console request has no Sec-WebSocket-Key")
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n" % websocket_accept(key)
    )
    connection.sendall(response.encode("ascii"))


def drain_console(connection: socket.socket, closed: threading.Event) -> None:
    """Drain plugin-to-PC WebSocket frames (currently vibration updates)."""
    try:
        while not closed.is_set():
            if not connection.recv(2048):
                break
    except OSError:
        pass
    finally:
        closed.set()


def control_loop(state: BridgeState, stop: threading.Event, ready: threading.Event) -> None:
    control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        try:
            control.bind(("127.0.0.1", CONTROL_PORT))
        except OSError as error:
            print("Could not bind local control port %d: %s" % (CONTROL_PORT, error),
                  file=sys.stderr, flush=True)
            stop.set()
            ready.set()
            return
        ready.set()
        control.settimeout(0.25)
        print("Local controls ready on 127.0.0.1:%d" % CONTROL_PORT, flush=True)
        while not stop.is_set():
            try:
                data, peer = control.recvfrom(4096)
            except socket.timeout:
                continue
            try:
                request = json.loads(data.decode("utf-8"))
                if not isinstance(request, dict):
                    raise ValueError("command must be a JSON object")
                result = state.apply(request)
            except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
                result = {"ok": False, "error": str(error)}
            try:
                control.sendto(json.dumps(result).encode("utf-8"), peer)
            except OSError:
                pass
    finally:
        control.close()


def handle_console(connection: socket.socket, peer: tuple[str, int],
                   state: BridgeState, stop: threading.Event) -> None:
    connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    connection.settimeout(5.0)
    try:
        perform_handshake(connection)
    except (OSError, ValueError, ConnectionError) as error:
        print("Rejected console connection from %s: %s" % (peer[0], error), flush=True)
        connection.close()
        return

    connection.settimeout(None)
    closed = threading.Event()
    reader = threading.Thread(target=drain_console, args=(connection, closed), daemon=True)
    reader.start()
    with state.lock:
        state.connected = True
    print("Xbox connected from %s" % peer[0], flush=True)

    # Give the plugin time to finish its handshake recv before sending frames.
    time.sleep(0.10)
    try:
        next_send = time.monotonic()
        while not stop.is_set() and not closed.is_set():
            for message in state.frames():
                connection.sendall(websocket_text_frame(message))
            next_send += SEND_INTERVAL
            delay = next_send - time.monotonic()
            if delay > 0:
                stop.wait(delay)
            else:
                next_send = time.monotonic()
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass
    finally:
        for message in state.disconnect_frames():
            try:
                connection.sendall(websocket_text_frame(message))
            except OSError:
                break
        try:
            connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        connection.close()
        with state.lock:
            state.connected = False
        closed.set()
        print("Xbox connection ended", flush=True)


def serve(bind: str, port: int, xbox_ip: str | None) -> int:
    state = BridgeState()
    stop = threading.Event()
    control_ready = threading.Event()
    control_thread = threading.Thread(target=control_loop,
                                      args=(state, stop, control_ready), daemon=True)
    control_thread.start()
    if not control_ready.wait(2.0) or stop.is_set():
        stop.set()
        control_thread.join(timeout=1.0)
        return 2

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        listener.bind((bind, port))
        listener.listen(1)
        listener.settimeout(0.5)
    except OSError as error:
        stop.set()
        listener.close()
        control_thread.join(timeout=1.0)
        print("Could not listen on %s:%d: %s" % (bind, port, error), file=sys.stderr)
        return 2

    print("Omni360 input bridge listening on %s:%d" % (bind, port), flush=True)
    print("Start the DashLaunch plugin, then send local commands from another terminal.", flush=True)
    try:
        while not stop.is_set():
            try:
                connection, peer = listener.accept()
            except socket.timeout:
                continue
            if xbox_ip and peer[0] != xbox_ip:
                print("Ignored connection from %s; expected %s" % (peer[0], xbox_ip),
                      flush=True)
                connection.close()
                continue
            handle_console(connection, peer, state, stop)
    except KeyboardInterrupt:
        print("Stopping bridge...", flush=True)
    finally:
        stop.set()
        listener.close()
        control_thread.join(timeout=1.0)
    return 0


def send_control(command: dict[str, object]) -> int:
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        client.settimeout(1.0)
        client.sendto(json.dumps(command).encode("utf-8"), ("127.0.0.1", CONTROL_PORT))
        response = json.loads(client.recvfrom(4096)[0].decode("utf-8"))
    except OSError as error:
        print("Bridge control is unavailable. Start the 'serve' command first. (%s)" % error,
              file=sys.stderr)
        return 2
    finally:
        client.close()
    print(json.dumps(response))
    return 0 if response.get("ok") else 2


def add_slot_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--slot", type=int, choices=range(SLOT_COUNT), default=0,
                        help="virtual controller slot (default: 0)")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Control an Xbox 360 through Omni360's input bridge.")
    commands = parser.add_subparsers(dest="command", required=True)

    serve_parser = commands.add_parser("serve", help="listen for the Xbox plugin")
    serve_parser.add_argument("--bind", default="0.0.0.0", help="PC interface address (default: all IPv4 interfaces)")
    serve_parser.add_argument("--port", type=int, default=DEFAULT_DATA_PORT,
                              help="TCP/WebSocket port (default: %(default)s)")
    serve_parser.add_argument("--xbox-ip", help="accept connections only from this Xbox IPv4 address")

    for name in ("press", "release", "tap"):
        sub = commands.add_parser(name, help={"press": "hold buttons", "release": "release buttons",
                                              "tap": "press buttons for a short duration"}[name])
        add_slot_argument(sub)
        sub.add_argument("buttons", nargs="+", help="A, B, X, Y, directions, LB/RB/LT/RT, BACK, START, GUIDE, L3, R3")
        if name == "tap":
            sub.add_argument("--ms", type=int, default=120, help="tap duration (default: %(default)s ms)")

    neutral_parser = commands.add_parser("neutral", help="release all buttons and center the sticks")
    add_slot_argument(neutral_parser)
    disconnect_parser = commands.add_parser("disconnect", help="remove the virtual controller")
    add_slot_argument(disconnect_parser)

    axis_parser = commands.add_parser("axis", help="set a stick axis or trigger")
    add_slot_argument(axis_parser)
    axis_parser.add_argument("name", choices=sorted(AXIS_RANGES), help="LX, LY, RX, RY, LT, or RT")
    axis_parser.add_argument("value", type=float, help="stick: -1..1; trigger: 0..1")

    commands.add_parser("status", help="show bridge and virtual slot state")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "serve":
        if args.port < 1 or args.port > 65535:
            print("port must be 1..65535", file=sys.stderr)
            return 2
        if args.xbox_ip:
            try:
                socket.inet_aton(args.xbox_ip)
            except OSError:
                print("--xbox-ip must be an IPv4 address", file=sys.stderr)
                return 2
        return serve(args.bind, args.port, args.xbox_ip)

    command: dict[str, object] = {"cmd": args.command}
    if args.command != "status":
        command["slot"] = args.slot
    if args.command in ("press", "release", "tap"):
        command["buttons"] = args.buttons
    if args.command == "tap":
        command["ms"] = args.ms
    if args.command == "axis":
        command["axis"] = args.name
        command["value"] = args.value
    return send_control(command)


if __name__ == "__main__":
    raise SystemExit(main())
