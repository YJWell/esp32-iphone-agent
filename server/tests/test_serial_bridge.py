import asyncio
import json
import threading
import time
from collections import deque

import pytest

from hidagent.device import DeviceError
from hidagent.phone import Phone
from hidagent.serial_bridge import FIRMWARE, Bridge, SerialDevice, main, validate


class FakeSerial:
    def __init__(self, **kwargs):
        self.options = kwargs
        self.writes = []
        self.buffer = bytearray()
        self.responses = deque()
        self.closed = False
        self.epoch = 1
        self.ready = True
        self.short = False
        self.on_write = None

    def open(self):
        self.closed = False

    def close(self):
        self.closed = True

    def reset_input_buffer(self):
        self.buffer.clear()

    def write(self, data):
        self.writes.append(data)
        request_id, epoch, command = data.decode().rstrip("\n").split(" ", 2)
        if self.on_write:
            self.on_write(command)
        reply = dict(id=request_id, ok=True, firmware=FIRMWARE, generation=self.epoch,
                     initialized=True, connected=self.ready, encrypted=self.ready,
                     subscribed=self.ready, ready=self.ready)
        if self.responses:
            response = self.responses.popleft()
            if callable(response):
                response = response(reply)
            if response is not None:
                self.buffer.extend(response if isinstance(response, bytes) else json.dumps(response).encode() + b"\n")
        else:
            self.buffer.extend(json.dumps(reply).encode() + b"\n")
        return len(data) - 1 if self.short else len(data)

    def read(self, _):
        if self.buffer:
            result = bytes(self.buffer[:1])
            del self.buffer[:1]
            return result
        time.sleep(0.001)
        return b""

    @property
    def commands(self):
        return [data.decode().rstrip("\n").split(" ", 2)[2] for data in self.writes]


@pytest.fixture
def connection(monkeypatch):
    monkeypatch.setattr("hidagent.serial_bridge.time.sleep", lambda _: None)
    fake = FakeSerial()
    bridge = Bridge("FAKE", factory=lambda **kwargs: fake)
    return bridge, fake


@pytest.mark.parametrize("command", [
    "move -10000 10000", "scroll -127 127", "type   spaces  ", "type " + "x" * 256,
    "key cmd+shift+h", "key ctrl+a+b+c+d+e+f", "key enter", "key A+shift", "key shift+A",
    "status", "press", "release", "home",
])
def test_valid_commands(command):
    assert validate(command) == command.split(" ")[0]


@pytest.mark.parametrize("command", [
    "", "click left", "move 1", "move 1  2", "move 1 2 3", "move +1 0", "move 10001 0",
    "scroll 0 -128", "type ", "type " + "x" * 257, "type hi\nclick", "type \r", "type \t",
    "type \0", "type \x7f", "type \u4e2d", "key cmd+", "key +a", "key a++b", "key a+A",
    "key ctrl+ctrl+a", "key a+b+c+d+e+f+g", "key space+ ", "key unknown", "click ",
    "key 1+!", "key -+_", "key /+?",
])
def test_invalid_commands_never_open_or_write(connection, command):
    bridge, fake = connection
    with pytest.raises(DeviceError):
        bridge.execute(command)
    assert not fake.writes and bridge._connection is None


def test_handshake_preserves_spaces_and_correlates_requests(connection):
    bridge, fake = connection
    assert bridge.execute("type   hello  ")["ok"]
    assert fake.commands == ["status", "type   hello  "]
    ids = [line.split(b" ")[0] for line in fake.writes]
    assert len(set(ids)) == 2 and all(len(value) == 32 for value in ids)
    assert fake.writes[0].split(b" ")[1] == b"0"
    assert fake.writes[1].split(b" ")[1] == b"1"
    bridge.close()
    assert fake.closed


def test_status_without_phone_and_no_action_when_not_ready(connection):
    bridge, fake = connection
    fake.ready = False
    assert bridge.execute("status")["ready"] is False
    with pytest.raises(DeviceError, match="not ready"):
        bridge.execute("click")
    assert fake.commands == ["status", "status"] and fake.closed


@pytest.mark.parametrize("overrides", [
    {"firmware": "other"}, {"ready": "true"}, {"ready": None}, {"generation": True},
    {"generation": -1}, {"generation": 0x100000000}, {"initialized": False},
    {"encrypted": False}, {"subscribed": False}, {"connected": False},
])
def test_malformed_handshake_fails_before_action(connection, overrides):
    bridge, fake = connection
    fake.responses.append(lambda value: value | overrides)
    with pytest.raises(DeviceError):
        bridge.execute("click")
    assert fake.commands == ["status"] and fake.closed


def test_stale_replies_and_boot_diagnostics_are_not_acknowledgements(connection):
    bridge, fake = connection
    fake.responses.append(lambda value: b"ROM boot\n{}\n"
                          + json.dumps(value | {"id": "0" * 32}).encode() + b"\n"
                          + json.dumps(value).encode() + b"\n")
    assert bridge.execute("click")["ok"]
    assert fake.commands == ["status", "click"]


def test_generation_change_before_action_requires_rehome(connection):
    bridge, fake = connection
    bridge.execute("ping")
    before = bridge.links
    fake.epoch += 1
    with pytest.raises(DeviceError, match="generation changed"):
        bridge.execute("move 5 5")
    assert fake.commands == ["status", "status"] and bridge.links > before
    assert bridge.execute("home")["ok"]


def test_disconnect_during_action_is_not_replayed(connection):
    bridge, fake = connection
    fake.responses.extend([lambda value: value, lambda value: value | {"generation": 2}])
    with pytest.raises(DeviceError, match="NOT retried"):
        bridge.execute("click")
    assert fake.commands == ["status", "click"] and fake.closed
    assert bridge.execute("status")["ok"]
    assert fake.commands == ["status", "click", "status"]


@pytest.mark.parametrize("response", [
    lambda value: value | {"ok": False, "error": "notify_failed"},
    lambda value: value | {"ok": "yes"},
    lambda value: b"x" * 1025,
])
def test_bad_action_reply_closes_without_retry(connection, response):
    bridge, fake = connection
    fake.responses.extend([lambda value: value, response])
    with pytest.raises(DeviceError):
        bridge.execute("click")
    assert fake.commands == ["status", "click"] and fake.closed


def test_timeout_and_short_write_are_uncertain_without_retries(connection, monkeypatch):
    bridge, fake = connection
    fake.responses.extend([lambda value: value, None])
    clock = [0]
    original_read = fake.read

    def read(size):
        if fake.commands == ["status", "click"] and not fake.buffer:
            clock[0] = 20
        return original_read(size)

    fake.read = read
    monkeypatch.setattr("hidagent.serial_bridge.time.monotonic", lambda: clock[0])
    with pytest.raises(DeviceError, match="timeout.*NOT retried"):
        bridge.execute("click")
    assert fake.commands == ["status", "click"]
    fake.short = True
    with pytest.raises(DeviceError, match="short serial write"):
        bridge.execute("click")
    assert fake.commands == ["status", "click", "status"]


def test_serial_exception_is_visible_and_closes(connection):
    bridge, fake = connection

    def failed(_):
        raise OSError("port removed")

    fake.on_write = failed
    with pytest.raises(DeviceError, match="port removed"):
        bridge.execute("click")
    assert fake.closed and fake.commands == ["status"]


def test_concurrent_calls_keep_handshake_and_action_together(connection):
    bridge, fake = connection
    threads = [threading.Thread(target=bridge.execute, args=(command,))
               for command in ("move 1 2", "key enter", "click")]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=2)
        assert not thread.is_alive()
    assert fake.commands[::2] == ["status"] * 3
    assert set(fake.commands[1::2]) == {"move 1 2", "key enter", "click"}


def test_async_cancellation_waits_for_worker_and_never_sends_action(connection):
    bridge, fake = connection
    started, finish = threading.Event(), threading.Event()

    def wait(command):
        if command == "status":
            started.set()
            assert finish.wait(2)

    fake.on_write = wait
    device = SerialDevice("FAKE", bridge=bridge)

    async def scenario():
        task = asyncio.create_task(device.send("click"))
        assert await asyncio.to_thread(started.wait, 2)
        task.cancel()
        await asyncio.sleep(0)
        task.cancel()
        assert device._lock.locked()
        finish.set()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert not device._lock.locked() and fake.closed

    asyncio.run(scenario())
    assert fake.commands == ["status"]


def test_async_close_retains_ownership_until_worker_finishes(connection):
    bridge, fake = connection
    bridge.execute("status")
    started, finish = threading.Event(), threading.Event()
    original_close = fake.close

    def wait_close():
        started.set()
        assert finish.wait(2)
        original_close()

    fake.close = wait_close
    device = SerialDevice("FAKE", bridge=bridge)

    async def scenario():
        task = asyncio.create_task(device.close())
        assert await asyncio.to_thread(started.wait, 2)
        task.cancel()
        await asyncio.sleep(0)
        task.cancel()
        assert device._lock.locked()
        finish.set()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert not device._lock.locked() and fake.closed
        assert device.links == bridge.links

    asyncio.run(scenario())


def test_phone_reuses_transport_for_drag_and_rehomes_after_reconnect(connection, tmp_path):
    bridge, fake = connection
    device = SerialDevice("FAKE", bridge=bridge)
    phone = Phone(device, tmp_path / "calibration.json")
    phone.span = {"w": 100, "h": 200}

    async def scenario():
        await phone.drag([(0.1, 0.1), (0.2, 0.2)], hold=False)
        assert "press" in fake.commands and "release" in fake.commands
        fake.epoch += 1
        with pytest.raises(DeviceError, match="generation changed"):
            await phone.point(0.3, 0.3)
        start = len(fake.commands)
        await phone.point(0.3, 0.3)
        assert [c for c in fake.commands[start:] if c != "status"] == ["release", "home", "move 30 60"]
        await device.close()

    asyncio.run(scenario())


def test_cli_invalid_input_is_json_without_port_access(capsys):
    assert main(["move", "10001", "0"]) == 1
    result = json.loads(capsys.readouterr().err)
    assert result["ok"] is False and "integers" in result["error"]
    assert main(["move", "oops", "0"]) == 1
    assert json.loads(capsys.readouterr().err)["ok"] is False


def test_close_failure_is_explicit_and_keeps_primary_error(connection):
    bridge, fake = connection

    def fail_close():
        raise OSError("close failed")

    fake.close = fail_close
    fake.responses.append(lambda value: value | {"firmware": "wrong"})
    with pytest.raises(DeviceError, match="wrong serial firmware; serial close failed"):
        bridge.execute("click")
    assert bridge._connection is None


@pytest.mark.parametrize("transport", ["tcp", "serial"])
@pytest.mark.parametrize("failure", [None, "startup", "shutdown", "heartbeat"])
def test_server_lifecycle_preserves_tcp_and_closes_serial(monkeypatch, tmp_path, transport, failure):
    from types import SimpleNamespace

    monkeypatch.setenv("HIDAGENT_DATA", str(tmp_path))
    monkeypatch.setenv("HIDAGENT_TRANSPORT", "tcp")
    from hidagent import app

    events = []

    class Listener:
        def close(self):
            events.append("listener-close")

        async def wait_closed(self):
            events.append("listener-drained")

    class Device:
        async def serve(self, host, port):
            events.append("listen")
            return Listener()

        async def send(self, command):
            events.append(command)

        async def close(self):
            events.append("serial-close")

        async def keep_awake(self, **kwargs):
            try:
                await asyncio.Event().wait()
            finally:
                events.append("heartbeat-drained")
                if failure == "heartbeat":
                    raise RuntimeError("heartbeat failed")

    class Screen:
        def start(self):
            events.append("screen-start")
            if failure == "startup":
                raise RuntimeError("receiver failed")

        def stop(self):
            events.append("screen-stop")
            if failure == "shutdown":
                raise RuntimeError("receiver shutdown failed")

    monkeypatch.setattr(app, "transport", transport)
    monkeypatch.setattr(app, "device", Device())
    monkeypatch.setattr(app, "screen", Screen())
    monkeypatch.setattr(app, "os", SimpleNamespace(name="posix", environ={}))

    async def scenario():
        async with app.lifespan(None):
            await asyncio.sleep(0)

    if failure:
        with pytest.raises(RuntimeError, match="failed"):
            asyncio.run(scenario())
    else:
        asyncio.run(scenario())
        assert "heartbeat-drained" in events
    assert events[0] == ("listen" if transport == "tcp" else "ping")
    assert "screen-stop" in events
    if transport == "tcp":
        assert events[-2:] == ["listener-close", "listener-drained"]
        assert "serial-close" not in events
    else:
        assert events[-1] == "serial-close" and "listen" not in events


def test_windows_server_rejects_before_opening_any_hardware(monkeypatch, tmp_path):
    from types import SimpleNamespace

    monkeypatch.setenv("HIDAGENT_DATA", str(tmp_path))
    from hidagent import app

    monkeypatch.setattr(app, "transport", "serial")
    monkeypatch.setattr(app, "os", SimpleNamespace(name="nt"))

    async def scenario():
        async with app.lifespan(None):
            pytest.fail("Windows screen backend must not start")

    with pytest.raises(RuntimeError, match="serial JSON CLI only"):
        asyncio.run(scenario())
