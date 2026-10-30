# Native small-window recovery probe

This independent project exercises the Win32 host transition used by `RecoverWithNativeSmallWindows` and the real `msimeui::DeviceResources` DirectComposition renderer. It creates only its own three popup HWNDs, with WebView warmup layered/cloaked/one-pixel-clipped state, and checks recovery, physical position, actual DWM uncloaking, client hit-test queries and visible red/green/blue frames. A separate aborted transition checks restoration of old alpha, region, style and position. It never injects keyboard/mouse input, touches another application's HWND, modifies saved configuration or downloads weights.

```powershell
cmake -S server/tests/native-recovery -B build/native-recovery -A x64
cmake --build build/native-recovery --config Release --target native_recovery_probe
./build/native-recovery/Release/native_recovery_probe.exe
```

Run in an interactive Windows desktop session. The probe briefly shows its own windows without activation and writes JSON to stdout. It is not part of the installer or normal Server CMake target. Its current local validation used actual 120 DPI; it does not claim physical tests at other DPI values. Monitor colour management changes screen RGB, so the visible-colour test records actual values and accepts up to 40 per-channel difference; exact offscreen renderer parity is covered by the UI suite. Host hit-test queries are not a claim of replaying every product button interaction.

Windows rejected dynamically adding `WS_EX_NOREDIRECTIONBITMAP` to the existing warmup HWND with error 87. Recovery therefore clears `WS_EX_LAYERED`, retains a full glass background and binds a valid DirectComposition target to that existing ordinary HWND. The probe verifies actual visible composition rather than replacing HWNDs and invalidating IPC references. Normal native windows created with `WS_EX_NOREDIRECTIONBITMAP` keep their original creation path.
