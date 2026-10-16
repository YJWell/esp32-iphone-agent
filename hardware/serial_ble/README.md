# USB serial to Bluetooth HID (ESP32-C3 and ESP32-S3)

Opt-in computer-controlled mode, separate from the existing phone-only S3 firmware:

```text
computer --USB native CDC--> C3 or S3 --encrypted Bluetooth HID mouse/keyboard--> iPhone
```

The iPhone does not connect to the board by USB. No Wi-Fi, PSRAM, AirPlay software, iPhone app or model is required
for input-only control. The original `hardware/hid_bridge` and `ios/` workflow is unchanged.
Both boards use the **same source and host commands**; only the build environment changes.

## Build and hardware assumptions

Install PlatformIO Core 6.1.18 (`python -m pip install platformio==6.1.18`), then from the repository root:

```powershell
python -m platformio run --project-dir hardware\serial_ble -e esp32c3
python -m platformio run --project-dir hardware\serial_ble -e esp32s3
```

These commands only compile. They never flash or open a serial port.
The manifest pins Espressif32 6.12.0 (Arduino core 2.0.17) and NimBLE-Arduino 2.3.6.
No board or phone is needed for these builds.

| Environment | Generic board definition | USB / flash assumption |
|---|---|---|
| `esp32c3` | `esp32-c3-devkitm-1` | 4 MB flash; native USB Serial/JTAG on GPIO18/19 |
| `esp32s3` | `esp32-s3-devkitc-1` | conservative 4 MB flash/partition layout, no PSRAM; native USB on GPIO19/20 |

`ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` select native CDC.
Many development boards' default USB sockets instead connect to a UART bridge. Use the socket wired to native USB
(often marked **USB**, not **UART**), or confirm the exact board wiring before use. A generic C3 devkit definition
does not guarantee its socket has native USB wired. The exact ordered S3 model is unknown; check its flash capacity,
USB routing and boot procedure when it arrives. A larger-flash S3 may use this 4 MB layout, but do not guess a
smaller flash size or enable board-specific PSRAM settings.

Flashing is a **separate manual hardware step**, not part of compilation or these tests. After choosing the board
and verifying the port, use PlatformIO's upload task for that environment only when you intend to replace its firmware.
No connected COM3 board has been flashed or sent live input as part of this change.

## Pair the iPhone

1. Power the board through the computer's native-USB data connection. Unlock the iPhone.
2. Enable Settings -> Accessibility -> Touch -> AssistiveTouch.
3. In Bluetooth settings pair **iPhone-Agent-Serial**. Only one BLE connection is supported.
4. Run `status`. Input requires `initialized`, `connected`, `encrypted`, `subscribed` and `ready` all true.
   Both mouse and keyboard input-report subscriptions are required. Re-pair if the phone cached an older HID descriptor.

Pairing is bonded/encrypted "Just Works" (no display/passkey, hence no MITM authentication).
Pair in a trusted physical environment. Bond information stays on the board; remove the phone's pairing and erase
the board's bond storage through your normal firmware-maintenance procedure if intentionally changing owners.
Neither compilation nor mocked tests establish that a particular iOS version will subscribe or accept input.
Physical pairing and pointer/keyboard behavior on both boards remain manual acceptance steps.

## Windows JSON CLI (also works on Mac/Linux)

For input-only use, install `pyserial==3.5`; no server runtime is needed. From `server`:

```powershell
python -m pip install pyserial==3.5
python -m hidagent.serial_bridge ports
python -m hidagent.serial_bridge --port COM3 status
# The following are examples of LIVE INPUT: run only when you intend to control the phone.
python -m hidagent.serial_bridge --port COM3 move 100 -50
python -m hidagent.serial_bridge --port COM3 click
python -m hidagent.serial_bridge --port COM3 scroll -3 0
python -m hidagent.serial_bridge --port COM3 type "Hello  iPhone!"
python -m hidagent.serial_bridge --port COM3 key cmd+h
python -m hidagent.serial_bridge --port COM3 home
python -m hidagent.serial_bridge --port COM3 release
```

Alternatively install `pip install -e ".[serial]"` from `server` to get the `hidagent-serial` entry point.
`ports` lists devices without opening them; `status` sends no action reports (the board may send neutral releases).
If no `--port` is supplied, exactly one Espressif native CDC port (VID 303A / PID 1001) must be present.
Explicit ports are preferred if there is any ambiguity. Success is JSON on stdout; errors are JSON on stderr
with exit code 1 (help text is ordinary argparse help).

The CLI opens/closes its connection per invocation. Use the persistent Python transport/server for drags;
separate CLI `press`/`move` invocations may trigger USB-close cleanup, and a held drag always expires.
Use only one owner of a serial port: stop monitors before opening the CLI/server. Windows enforces exclusive
serial opening; POSIX uses pyserial's exclusive lock for cooperating applications.

## Commands and limits

| Command | Behavior / validated bounds |
|---|---|
| `status`, `ping` | Status/firmware handshake even without a phone; no action |
| `home` | Release, then 50 reports of (-127, -127), driving the pointer into the upper-left corner; **not** iOS Home |
| `move dx dy` | Relative integers -10000..10000, paced in <=10-unit reports every 8 ms |
| `press`, `release`, `click` | Left mouse button; release clears keyboard too; click holds 60 ms |
| `scroll v h` | Vertical wheel and horizontal AC Pan, each -127..127 |
| `type text` | 1..256 printable US ASCII characters, preserving all leading/trailing/interior spaces |
| `key combo` | `+`-separated `cmd`, `ctrl`, `alt`, `shift`, named keys or printable single characters; max six distinct key usages |

Named keys: `enter`, `esc`, `tab`, `backspace`, `space`, `up`, `down`, `left`, `right`.
Use `type "+"` for a literal plus, since `+` separates chord tokens.
Non-ASCII, DEL, CR, LF, tabs and other controls are rejected **before input**. Use `key enter` separately.
Keyboard mapping is **US layout only**; iPhone hardware-keyboard layout, shortcuts, accessibility settings and app focus
determine interpretation. There is no Unicode/IME injection, consumer-control `homescreen`, screen capture,
absolute touch or application-completion confirmation. `cmd+h` may invoke iOS Home depending on settings.
Pointer speed/acceleration differs by iOS; reuse the existing `Phone` calibration, not assumed pixel deltas.

## Protocol and safety

Each newline-terminated request is:

```text
<32 lowercase hexadecimal random request id> <BLE generation uint32> <command>
```

Maximum frame length is 599 printable ASCII bytes, excluding LF. CRLF is deliberately rejected.
`status`/`ping` may use generation 0; actions must use the exact generation from the current status.
One JSON reply echoes `id` and boolean `ok`. Successful replies include `firmware` (`esp32-iphone-serial-ble/1`),
integer `generation`, and boolean `initialized`, `connected`, `encrypted`, `subscribed`, `ready`.
Errors include an explicit `error` and current generation. A frame with no recoverable ID replies with `id=""`;
the client will not mistake it for an acknowledgement of another action.

The Python owner serializes handshake + action and uses a fresh random ID for **every exchange**.
Boot diagnostics and stale IDs cannot acknowledge new input. Wrong firmware, malformed status, lost serial,
timeout, cancellation, notification failure and BLE generation changes fail explicitly. Actions are **never retried**.
Only a later, distinct request can reconnect; reconnection probes status first. On a detected generation change,
an in-progress gesture fails rather than using stale coordinates. The existing `Phone` invalidates its position,
releases and re-homes on its next gesture.

Firmware validates the entire command/chord before non-neutral reports. Every multi-report action checks encryption,
subscriptions, USB-host presence and connection generation between reports. A disconnect/reconnect aborts the action;
pending neutral mouse/keyboard releases are retried when a safe link is available, never the original action.
Startup/reconnect clears held state. Errors release mouse/keyboard state. Typing while dragging is rejected.
`press` has a **3-second finite lease**, refreshed only by a successful move. A single move also cannot drag longer
than 3 seconds. Status/heartbeats do not renew it. Expiry releases and explicitly rejects further actions until
`release` or `home`; this prevents a vanished host from leaving a stuck button.

An acknowledgement means the BLE stack accepted the reports, **not** that iOS or an app completed an action.
A failure may mean partial input already reached the phone. Observe state before deliberately retrying.

## Reuse the existing agent

On a Mac/Linux computer with the existing UxPlay/GStreamer capture already configured, opt in with
`HIDAGENT_TRANSPORT=serial` and `HIDAGENT_SERIAL_PORT=<actual port>`, then start the existing server.
See [server README](../../server/README.md). TCP remains the default; `Phone`, gesture/calibration logic, prompts
and agent are reused rather than replaced.

**Windows is input-only here.** `Screen` uses UxPlay and POSIX `pass_fds`. There is no working Windows screen loop
in this change; an independently configured receiver/capture backend is required before a visual agent can work.

## Validation and attribution

CI compiles both environments and runs `tests/test_protocol.cpp` against the actual parser/state implementation:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic hardware/serial_ble/tests/test_protocol.cpp -o /tmp/test-serial-ble
/tmp/test-serial-ble
```

Python protocol/ownership/cancellation/`Phone` integration tests live in `server/tests/test_serial_bridge.py`.
These are simulated host tests, not physical iPhone delivery tests.

The MIT license and Henry Yuan attribution at the repository root are retained. This target adapts the local
`iphone-bridge` prior art: pinned PlatformIO/NimBLE versions, encrypted bonded HID readiness checks, bounded line input,
JSON handshake, no action replay and release-only retry. It does not copy its environments, logs, secrets or artifacts.
No GPL AirPlay implementation is used in this new target.
