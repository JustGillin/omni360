# Omni360 Input Bridge

This DashLaunch plugin injects one to four virtual XInput gamepads into the Xbox 360. The PC helper sends button and axis reports over a local TCP/WebSocket connection. It is based on [WirelessInput360](https://github.com/UncreativeXenon/WirelessInput360); see `THIRD_PARTY_LICENSE.txt` for the MIT license.

## Compatibility

- The plugin checks for kernel builds **17559** and **17489**. The XAM hook addresses are build-specific; do not load this XEX on another kernel.
- It must be loaded by a working DashLaunch-compatible plugin loader. ABadAvatar is a temporary exploit, so follow the loader/configuration path that is already working on your console. This project does not install DashLaunch or modify the NAND.
- ABadAvatar is not persistent: after a full power-off/restart, run the exploit and loader again before expecting the plugin to run.
- Keep the disc tray closed when the plugin loads.

## Build

Open `Omni360InputBridge.vcxproj` in Visual Studio 2010 with the Xbox 360 XDK installed, then build `Release Retail | Xbox 360`. The XEX is written to the project's `Release Retail` output directory.

## Install

1. Copy `Omni360InputBridge.xex` and a configured `Omni360InputBridge.ini` into the same folder on the console. Start from `Omni360InputBridge.ini.example` and replace `192.0.2.1` with the PC's LAN IPv4 address. Leave the port at `36062` unless you also change the PC helper's `--port`.
2. Add the XEX to an unused plugin slot in the DashLaunch-compatible configuration you already use. Preserve the existing plugin entries, including XBDM if you rely on it.
3. Start the PC helper before the plugin starts:

   ```powershell
   python tools/omni360_input_bridge.py serve
   ```

   If you know the Xbox IP from Neighborhood, you can restrict incoming connections to it with `--xbox-ip 192.168.1.42`.
4. Run ABadAvatar and its configured loader, then allow the helper through Windows Firewall on your private LAN if Windows asks. The Xbox connects outbound to the PC on TCP port 36062.

## Startup and troubleshooting

The plugin does almost nothing while the console boots. A worker thread waits (15 seconds by default) and only then hooks XAM's input functions and connects to the PC. Hooking during boot left the console on a black screen under ABadAvatar.

It writes `Omni360InputBridge.log` beside the XEX, and the same lines go to XBDM's debug output. Two optional INI settings help narrow down a problem:

- `delay=15000` sets how long to wait after boot, in milliseconds.
- `stage=` stops after a given step: 2 waits only, 3 also looks up XAM, 4–7 add one hook each, and 8 (the default) runs everything, including the network.

Before hooking, the plugin checks the hardcoded XAM addresses against the expected instructions. If an address doesn't match, it skips that hook and logs why.

## Send input

The PC helper requires Python 3.10 or newer and has no third-party dependencies. Run these from another terminal while `serve` is running:

```powershell
python tools/omni360_input_bridge.py status
python tools/omni360_input_bridge.py tap A
python tools/omni360_input_bridge.py press DPAD_RIGHT
python tools/omni360_input_bridge.py release RIGHT
python tools/omni360_input_bridge.py axis LX 0.6
python tools/omni360_input_bridge.py neutral
python tools/omni360_input_bridge.py disconnect
```

`tap` defaults to 120 ms; set `--ms` to change it. Commands target slot 0 by default; use `--slot 1` through `--slot 3` to select another virtual controller. `neutral` centers sticks and releases buttons. `disconnect` removes that virtual controller. Stick values range from -1 to 1; positive Y means up, and positive X means right. Trigger values range from 0 to 1.

The helper accepts the Xbox connection on the LAN, while its command socket listens only on `127.0.0.1`. Do not forward the TCP port from your router.
