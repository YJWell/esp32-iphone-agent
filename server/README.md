# server/ (legacy)

This is the computer-side server of the original version (tag `v0.1.0`, branch `legacy`): UxPlay on a Mac or Linux
machine receives the screen, this Python server runs the agent and a web UI, and the ESP32 is only a USB mouse that
connects to it over Wi-Fi.

**The current phone-only version does not use it.** The board receives AirPlay itself and the agent runs in the iPhone app
(`ios/`), with its prompts in `prompts/`. This folder is kept so the legacy setup keeps working and its tests still
run in CI. Its prompts are the older ones in `hidagent/agent.py` and are not synced
with `prompts/`.

Note: the current firmware speaks BLE, not the TCP protocol this server expects. To run the legacy setup, check out
`legacy` (or `v0.1.0`) and follow its README.

## Optional serial/BLE transport (this fork)

The default is still legacy TCP. To reuse the existing gestures, calibration and agent on a Mac/Linux host
with UxPlay already configured, build [hardware/serial_ble](../hardware/serial_ble/README.md), install
`pip install -e ".[serial]"` here, and set `HIDAGENT_TRANSPORT=serial` plus
`HIDAGENT_SERIAL_PORT=/dev/ttyACM0` (use your actual port). Start with `python -m hidagent` as before.
Serial mode does not open the TCP board listener. Reconnection only probes status; an uncertain action is never replayed.
`Phone` releases and re-homes when its transport generation changes. A drag has a three-second inactivity lease
(and at most three seconds within one move); a longer continuous drag must be split or will explicitly fail.

For **Windows input-only use**, no AirPlay receiver, FastAPI server or model is needed:
install `pyserial==3.5`, run from this directory, and use `python -m hidagent.serial_bridge ports`,
then `python -m hidagent.serial_bridge --port COM3 status`.
Other CLI examples and limitations are in the firmware README. `status` is safe without a paired phone.
Do not run a serial monitor, CLI and server on the same port concurrently.

The existing `Screen` uses UxPlay/GStreamer and POSIX `pass_fds`; it is not a Windows capture backend.
The server rejects serial-mode startup on Windows with an explicit message. An independent receiver and
capture integration is still required for a Windows visual agent; this change does not install one.
