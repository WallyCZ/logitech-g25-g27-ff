# Logitech G25 / G27 / Driving Force GT force feedback driver for Windows 10 and 11

Force feedback, 900 degree rotation and separate pedals for the Logitech G25, G27 and Driving Force GT racing wheels
on 64-bit Windows 10 and Windows 11, for both 64-bit and 32-bit games, **with Memory Integrity (Core Isolation) on**.

| Wheel | Status |
|---|---|
| G25 | tested (Windows 11, Memory Integrity on, Richard Burns Rally) |
| G27 | untested: should work, the protocol is the same; please [report](https://github.com/WallyCZ/logitech-g25-g27-ff/issues) how it goes |
| Driving Force GT | untested: should work, the protocol is the same; please [report](https://github.com/WallyCZ/logitech-g25-g27-ff/issues) how it goes |

The G27 and Driving Force GT support follows the Linux `hid-lg4ff` driver: the same force feedback commands as the
G25, with their own command for the switch to native mode. The G27's shift lights are not supported.

The original Logitech driver (Logitech Gaming Software 5.x / Logitech Profiler) no longer works on Windows 11 with
Memory Integrity: its kernel filters (`WmFilter.sys`, `WmBEnum.sys`) are blocked, which leaves the wheel unusable.
Newer Logitech software does not support these wheels (G HUB), or is retired (Logitech Gaming Software 9).

This driver needs **no kernel driver** and no Logitech software: the wheel keeps the standard Windows HID driver.

## Installation
Download `G25FF-<version>.msi` from [Releases](https://github.com/WallyCZ/logitech-g25-g27-ff/releases) and run it. If the original Logitech wheel driver is installed, the
installer offers to remove it. Leave it checked: the wheel does not work while it is installed.

Test it in the Game Controllers panel (`joy.cpl`): select the wheel, click Properties and use the test
buttons on the Force Feedback tab. Uninstall it from Settings > Apps.

Alternatively, without the MSI: download `G25FF-<version>.zip`, extract it, right click `Install.ps1` and choose
"Run with PowerShell" (uninstall with `Install.ps1 -Uninstall`).

## Settings
Game Controllers panel (`joy.cpl`) > the wheel > Properties > Force Feedback:
- rotation range, 40 to 900 degrees,
- effect strength for all effects, springs and dampers,
- friction with the wheel's own friction effect, or played as damper,
- reverse the direction of forces (for games that push the wrong way),
- the wheel's centering spring when no game uses force feedback (games with force feedback center it themselves),
- test effects: push left, push right, rumble, spring.

Changes take effect when you click Apply, also in a game that is running.

## Supported effects
Constant force, ramp, square, sine, triangle, sawtooth up/down, spring, damper, friction and inertia (played as
damper). Envelopes, durations, loops, start delay and gain are supported. Trigger buttons and custom forces are not.
Only the steering axis has force feedback.

## How it works
The wheels start in a compatibility mode (they act as a Driving Force EX: 200 degrees, combined pedals, no shifter,
USB product id C294) and switch to their native mode (G25 C299, G27 C29B, Driving Force GT C29A) when they receive
a command. In compatibility mode the USB device revision tells the models apart (`src/devices.h`). The original
driver sent that command from its kernel filter; here the **agent** does it: `rundll32 G25FF.dll,Agent`, started
at logon, switches the wheel whenever it is plugged in and applies the rotation range and centering settings.

The wheel speaks Logitech's "classic" force feedback protocol: four force slots, each holding a constant force, a
spring, a damper or a friction effect, but no effect timing. The **DirectInput effect driver** (`G25FF.dll`,
registered as the wheel's `OEMForceFeedback` driver) plays the effects itself: an engine thread runs every 2 ms
while effects play, works out durations, loops, start delays and envelopes, adds all constant, ramp and periodic
effects up into the constant force slot and combines the conditions of each type into the spring, damper and
friction slots. A slot is only written when its content changes.

The wheel's HID descriptor has no force feedback usages, so the registration also marks the steering axis as a
force feedback actuator (`Axes\0` in the DirectInput OEM key); without it DirectInput rejects every effect.

- `src/devices.h` - the supported models: product ids, identification and native mode commands
- `src/wheel.*` - the wheels' protocol commands and HID access
- `src/ffdevice.*` - the effect engine: effect timing and the mapping of DirectInput effects to the four slots
- `src/effectdriver.*` - the `IDirectInputEffectDriver` COM object DirectInput loads for the wheel
- `src/agent.*` - the agent: native mode switch and wheel settings when the wheel is plugged in
- `src/proppage.*`, `src/settings.*` - the Force Feedback tab in the Game Controllers panel and its settings
- `src/registration.*`, `src/dllmain.cpp`, `src/msi.cpp` - COM, DirectInput and agent registration (used by
  `regsvr32` and by the MSI custom actions) and removal of the original Logitech driver
- `installer/G25FF.wxs` - the MSI installer

## Building
Run `build.cmd` (needs Visual Studio with the C++ workload). It produces `bin\x64\G25FF.dll` and
`bin\x86\G25FF.dll`; `Install.ps1` in the repository root finds them there. `package.ps1 -Version x.y.z` creates the
MSI (WiX 5, installed by `dotnet tool restore`) and the zip in `dist\`.

GitHub Actions builds both on every push. Pushing a tag `vX.Y.Z` publishes a release with them; running the Build
workflow manually creates a draft release for testing.

Tools for testing (build each with the `build.cmd` next to it):
- `tools\fftest` - DirectInput force feedback tests: `fftest` lists the devices and effects, `fftest directions`
  checks which way forces turn the wheel, `fftest suite interactive` plays every effect type with a description
  and asks how it felt
- `tools\hidraw` - raw HID reports: `hidraw switch` sends the native mode command, `hidraw write 00 ...` any
  command, `hidraw read` prints input reports
- `tools\analyze-capture.py` - finds the meaning of input report bytes in a `hidraw read` capture

## Troubleshooting
If the wheel turns only about 200 degrees and the pedals are combined, it is still in compatibility mode: check
that the agent runs (`rundll32.exe` with `G25FF.dll,Agent` in Task Manager's details), or switch it once with
`rundll32 "C:\Program Files\G25FF\x64\G25FF.dll",SwitchMode`.

Set `HKCU\Software\G25FF` `Debug` (DWORD) to `1` and the driver logs to `%TEMP%\G25FF.log` and to the debugger
output (DebugView). Please attach the log when reporting a problem.
