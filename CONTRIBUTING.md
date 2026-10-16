# Contributing

Issues and pull requests are welcome. Keep changes small and focused, and match the surrounding code.

Before sending a change, run the checks for what you touched:

```bash
# app (Xcode 26+)
cd ios && xcodegen && xcodebuild test -scheme IPhoneAgent -destination 'platform=iOS Simulator,name=iPhone 17 Pro'
# firmware
hardware/flash.sh --build-only && make -C hardware/tests
# legacy server
cd server && pip install -e ".[dev,serial]" && ruff check . && pytest
# opt-in serial/BLE firmware (compile only, no phone or serial port needed)
python -m platformio run --project-dir hardware/serial_ble
g++ -std=c++11 -Wall -Wextra -Werror -pedantic hardware/serial_ble/tests/test_protocol.cpp -o /tmp/test-serial-ble
/tmp/test-serial-ble
```

Run each block from the repository root (or return there after `cd server`).
The serial/BLE target and protocol are documented in [hardware/serial_ble/README.md](hardware/serial_ble/README.md).
Both board environments must compile. Host tests exercise the production parser and report state machine;
they do not prove physical iPhone pairing, USB wiring or iOS input delivery.

Use uv 0.12.24 to generate `server/uv.lock` against public PyPI:
`uv lock --project server --default-index https://pypi.org/simple`.
CI checks lock freshness and publishes the generated lock as the `server-dependency-lock` artifact.
Do not commit machine-specific registries or relax TLS verification; normal pip installation remains supported.

Never commit secrets: `ios/Config/Local.xcconfig` and `hardware/hid_bridge/secrets.h` are git-ignored on purpose.
Run data exported from the app is redacted, but check it before attaching it to an issue.

Code under `hardware/hid_bridge/src/airplay/` is GPL-3.0 (ported from UxPlay); contributions there are GPL-3.0 too.
