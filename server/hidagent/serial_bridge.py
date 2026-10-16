"""Opt-in USB serial/BLE HID transport and JSON CLI (no screen receiver required)."""

import argparse
import asyncio
import json
import logging
import os
import re
import sys
import threading
import time
import uuid

from .device import Device, DeviceError

FIRMWARE = "esp32-iphone-serial-ble/1"
MAX_TEXT = 256
SIMPLE = {"status", "ping", "home", "press", "release", "click"}
KEYS = {"enter", "esc", "tab", "backspace", "space", "up", "down", "left", "right"}
MODIFIERS = {"cmd", "ctrl", "alt", "shift"}
SHIFTED_KEYS = dict(zip("!@#$%^&*()_+{}|:\"~<>?", "1234567890-=[]\\;'`,./", strict=True))
log = logging.getLogger(__name__)


def validate(command: str) -> str:
    """Validate a complete command before opening a port or emitting any HID input."""
    if not command or any(not 32 <= ord(ch) <= 126 for ch in command):
        raise DeviceError("command must be printable US ASCII, without CR/LF or control characters")
    name, separator, arg = command.partition(" ")
    if name in SIMPLE and not separator:
        return name
    if name in {"move", "scroll"}:
        if re.fullmatch(r"-?[0-9]{1,5} -?[0-9]{1,5}", arg):
            bound = 10000 if name == "move" else 127
            if all(abs(int(n)) <= bound for n in arg.split(" ")):
                return name
        raise DeviceError(f"{name} requires two integers in [-{10000 if name == 'move' else 127}, "
                          f"{10000 if name == 'move' else 127}]")
    if name == "type" and separator and 1 <= len(arg) <= MAX_TEXT:
        return name
    if name == "key" and separator:
        parts = arg.split("+")
        canonical = [SHIFTED_KEYS.get(p, p.lower()) if len(p) == 1 else p for p in parts]
        if (all(p in KEYS | MODIFIERS or len(p) == 1 for p in parts)
                and len(set(canonical)) == len(parts)
                and not ("space" in parts and " " in parts)
                and len([p for p in parts if p not in MODIFIERS]) <= 6):
            return name
    raise DeviceError("invalid command, text length (1..256), or key chord")


def choose_port(port=None):
    from serial.tools import list_ports

    devices = list(list_ports.comports())
    if port:
        return port
    matches = [p.device for p in devices if p.vid == 0x303A and p.pid == 0x1001]
    if len(matches) != 1:
        raise DeviceError(f"expected one native Espressif USB port, found {len(matches)}; specify --port")
    return matches[0]


class Bridge:
    """One owner, one in-flight request; actions are never automatically retried."""

    def __init__(self, port, *, factory=None):
        self.port = port
        self._factory = factory
        self._connection = None
        self._lock = threading.Lock()
        self.links = 0
        self.ready = False
        self._generation = None

    def _close(self):
        connection, self._connection = self._connection, None
        self.ready = False
        self._generation = None
        if connection is not None:
            self.links += 1
            try:
                connection.close()
            except OSError as exc:
                raise DeviceError(f"serial close failed: {exc}") from exc

    def close(self):
        with self._lock:
            self._close()

    def _open(self):
        if self._connection is not None:
            return
        factory = self._factory
        if factory is None:
            import serial
            factory = serial.Serial
        options = {"exclusive": True} if os.name == "posix" else {}
        connection = factory(port=None, baudrate=115200, timeout=0.1, write_timeout=1, **options)
        try:
            connection.dtr = True
            connection.rts = False
            connection.port = self.port
            connection.open()
            time.sleep(0.3)
            connection.reset_input_buffer()
        except OSError:
            connection.close()
            raise
        self._connection = connection
        self.links += 1

    @staticmethod
    def _cancelled(cancel):
        if cancel is not None and cancel.is_set():
            raise DeviceError("serial request cancelled; outcome may be partial; NOT retried")

    def _exchange(self, command, generation, cancel, timeout):
        self._cancelled(cancel)
        request_id = uuid.uuid4().hex
        data = f"{request_id} {generation} {command}\n".encode("ascii")
        if self._connection.write(data) != len(data):
            raise DeviceError("short serial write; outcome unknown; NOT retried")
        deadline = time.monotonic() + timeout
        buffered = bytearray()
        while time.monotonic() < deadline:
            self._cancelled(cancel)
            chunk = self._connection.read(1)
            if not chunk:
                continue
            buffered.extend(chunk)
            if len(buffered) > 1024:
                raise DeviceError("oversized serial reply; outcome unknown; NOT retried")
            if chunk != b"\n":
                continue
            line, buffered = bytes(buffered), bytearray()
            try:
                response = json.loads(line)
            except (ValueError, UnicodeDecodeError):
                continue  # ROM startup diagnostics are not protocol acknowledgements.
            if not isinstance(response, dict) or response.get("id") != request_id:
                continue  # Never let a stale reply acknowledge a new action.
            if type(response.get("ok")) is not bool:
                raise DeviceError("malformed serial acknowledgement")
            if not response["ok"]:
                raise DeviceError(f"board rejected request: {response.get('error', 'unspecified')}; NOT retried")
            if response.get("firmware") != FIRMWARE:
                raise DeviceError("wrong serial firmware")
            epoch = response.get("generation")
            if type(epoch) is not int or not 1 <= epoch <= 0xFFFFFFFF:
                raise DeviceError("malformed BLE generation")
            for field in ("initialized", "connected", "encrypted", "subscribed", "ready"):
                if type(response.get(field)) is not bool:
                    raise DeviceError(f"malformed status field: {field}")
            if response["ready"] and not all(response[k] for k in
                                             ("initialized", "connected", "encrypted", "subscribed")):
                raise DeviceError("inconsistent BLE readiness")
            return response
        raise DeviceError("serial reply timeout; outcome unknown; NOT retried")

    def execute(self, command, cancel=None):
        name = validate(command)
        with self._lock:
            try:
                self._cancelled(cancel)
                self._open()
                status = self._exchange("status", 0, cancel, 3)
                epoch = status["generation"]
                changed = self._generation is not None and epoch != self._generation
                if epoch != self._generation or status["ready"] != self.ready:
                    self.links += 1
                self._generation, self.ready = epoch, status["ready"]
                if name in {"status", "ping"}:
                    return status
                if not self.ready:
                    raise DeviceError("iPhone not ready: pair, unlock, enable AssistiveTouch and check status")
                if changed and name not in {"release", "home"}:
                    raise DeviceError("BLE generation changed; action NOT sent; release and re-home before continuing")
                timeout = 12 if name in {"move", "type"} else 3
                result = self._exchange(command, epoch, cancel, timeout)
                if result["generation"] != epoch or not result["ready"]:
                    raise DeviceError("BLE link changed during action; outcome uncertain; NOT retried")
                return result
            except (OSError, DeviceError) as exc:
                try:
                    self._close()
                except DeviceError as close_exc:
                    raise DeviceError(f"{exc}; {close_exc}") from exc
                if isinstance(exc, DeviceError):
                    raise
                raise DeviceError(f"serial I/O failed; outcome unknown; NOT retried: {exc}") from exc


class SerialDevice(Device):
    """Use the existing Phone/agent/keep-awake behavior with an explicit serial transport."""

    def __init__(self, port, *, bridge=None):
        super().__init__()
        self.bridge = bridge if bridge is not None else Bridge(port)

    @property
    def connected(self):
        return self.bridge.ready

    @staticmethod
    async def _wait_worker(worker, cancel=None):
        try:
            return await asyncio.shield(worker)
        except asyncio.CancelledError:
            if cancel is not None:
                cancel.set()
            # Retain ownership until the worker stops, even after repeated cancellation.
            while not worker.done():
                try:
                    await asyncio.shield(worker)
                except asyncio.CancelledError:
                    continue
                except DeviceError:
                    break
            if worker.done() and not worker.cancelled():
                worker.exception()
            raise

    async def send(self, command):
        async with self._lock:
            cancel = threading.Event()
            worker = asyncio.create_task(asyncio.to_thread(self.bridge.execute, command, cancel))
            try:
                await self._wait_worker(worker, cancel)
            finally:
                self.links = self.bridge.links
            self._beat = time.time()
            if command != "ping":
                self._last = self._beat

    async def close(self):
        async with self._lock:
            worker = asyncio.create_task(asyncio.to_thread(self.bridge.close))
            try:
                await self._wait_worker(worker)
            finally:
                self.links = self.bridge.links

    async def keep_awake(self, idle=25, heartbeat=2, screen=None):
        # Unlike a TCP board, a serial board cannot initiate reconnection.
        while True:
            await asyncio.sleep(heartbeat)
            if not self.connected:
                try:
                    await self.send("ping")
                except DeviceError as exc:
                    log.info("serial board unavailable: %s", exc)
                    continue
            elif time.time() - self._beat >= heartbeat:
                await self._heartbeat(idle, screen)

    async def _heartbeat(self, idle, screen):
        try:
            if time.time() - self._last > idle and (screen is None or screen.live):
                await self.send("move 1 0")
                await self.send("move -1 0")
            else:
                await self.send("ping")
        except DeviceError as exc:
            log.info("serial heartbeat failed: %s", exc)


class JsonParser(argparse.ArgumentParser):
    def error(self, message):
        raise DeviceError(message)


def main(argv=None):
    parser = JsonParser(description=__doc__)
    parser.add_argument("--port", help="explicit USB native CDC port, e.g. COM3")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("ports")
    for name in sorted(SIMPLE):
        sub.add_parser(name)
    for name in ("move", "scroll"):
        command = sub.add_parser(name)
        command.add_argument("a", type=int)
        command.add_argument("b", type=int)
    sub.add_parser("type").add_argument("text")
    sub.add_parser("key").add_argument("combo")
    bridge = None
    try:
        args = parser.parse_args(argv)
        if args.action == "ports":
            from serial.tools import list_ports
            result = [{"port": p.device, "description": p.description, "vid": p.vid, "pid": p.pid}
                      for p in list_ports.comports()]
        else:
            command = args.action
            if command in {"move", "scroll"}:
                command += f" {args.a} {args.b}"
            elif command == "type":
                command += " " + args.text
            elif command == "key":
                command += " " + args.combo
            validate(command)
            bridge = Bridge(choose_port(args.port))
            result = bridge.execute(command)
            bridge.close()
            bridge = None
        print(json.dumps(result, ensure_ascii=True))
        return 0
    except (DeviceError, OSError, ImportError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=True), file=sys.stderr)
        return 1
    finally:
        if bridge is not None:
            bridge.close()


if __name__ == "__main__":
    sys.exit(main())
