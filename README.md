# Desktop Cam App

A Windows-only C++ desktop application that renders a live video capture device (webcam or capture card) in a DirectX 11 window and plays microphone audio back through the system so that **both OBS Game Capture and Discord screen share** can capture the window and its audio reliably.

Because rendering uses a real DXGI flip-model swap chain and a continuous render loop, OBS Game Capture can hook the window the same way it hooks a game. Because mic audio is played through the standard WASAPI render endpoint, Discord's "Share audio" option picks it up on screen share.

Built and tested with an **Anyoyo 4K60 capture card** at up to **3840×2160** and **144 Hz**.

---

## Features

- DirectX 11 renderer with a flip-model swap chain and a textured quad showing the live camera feed
- **Up to 4K (3840×2160) capture and 120/144/240 Hz modes**, with a mode picker that lists every native format the device exposes
- **NV12 direct capture** — frames are taken in the card's native 4:2:0 layout and converted to RGB on the GPU by our own shader, skipping Media Foundation's per-frame RGB32 conversion (BT.601 / BT.709 / BT.2020, limited or full range)
- **V-Sync or uncapped presentation** (DXGI tearing) with an optional FPS limiter, so a 144 Hz capture isn't held down to a 60 Hz desktop
- Media Foundation video capture on the GPU path (DXGI device manager, zero CPU round-trip)
- WASAPI mic passthrough with event-driven capture + render threads and a lock-free, frame-aligned ring buffer
- **Low-latency audio**: IAudioClient3 minimum engine period where the driver allows it, an adjustable passthrough delay (5–200 ms), and automatic drift trimming so latency can't creep up over a session
- Volume control from 0–200 % with smoothing and peak metering
- Dear ImGui control panel: device dropdowns, mode picker, volume slider, Start/Stop, FPS, status
- Borderless mode and borderless fullscreen toggle (`Alt+Enter`) — window styles OBS Game Capture hooks reliably
- Optional NVIDIA RTX Super Resolution upscaling (Broadcast SDK, experimental)
- **Built-in auto-updater** backed by GitHub Releases
- Automatic device hot-reload every 2 seconds (plus manual Refresh buttons)
- Multi-threaded architecture (main render / video capture / audio capture / audio render)

---

## Architecture

```
src/
  main.cpp            WinMain entry point
  Application.*       Owns all modules, main loop, selection state
  Window.*            Win32 window + borderless/fullscreen + message routing
  Renderer.*          DX11: swap chain, shaders (BGRA + NV12), present pacing
  VideoCapture.*      Media Foundation: IMFSourceReader, mode negotiation
  AudioEngine.*       WASAPI capture + render, low-latency engine periods
  FrameRing.h         SPSC ring buffer, whole frames only (see the header)
  UpscalerNV.*        NVIDIA Broadcast SDK Super Resolution (optional)
  Updater.*           GitHub Releases check / download / self-replace
  UI.*                Dear ImGui control panel
  Settings.*          %APPDATA%\DesktopCamApp\settings.ini
```

Threads:

| Thread           | Owner          | Purpose                                                 |
|------------------|----------------|---------------------------------------------------------|
| Main             | `Application`  | Pump messages, ImGui, DX11 render, Present              |
| Video capture    | `VideoCapture` | `IMFSourceReader` async callback                        |
| Audio capture    | `AudioEngine`  | Read from WASAPI capture client, apply gain, push → ring |
| Audio render     | `AudioEngine`  | Pull from ring → write WASAPI render client              |
| Updater          | `Updater`      | WinHTTP release check / download                        |

The renderer's video-texture upload is done under a mutex, so the capture thread never fights the render thread for the D3D context.

---

## High resolution / high refresh rate

The capture path is built around three things, all exposed in the UI:

**1. Mode selection.** The **Mode** dropdown lists every native media type the device advertises (resolution, framerate, pixel format), de-duplicated and sorted. Pinning a mode pins it on the *source* before the reader is created, which is the only way to stop Media Foundation quietly settling on the first type it can convert (usually 1080p@25).

Leaving it on **Auto** uses the preference next to it:

| Auto picks       | Result on a 4K60 card                    |
|------------------|-------------------------------------------|
| Max resolution   | 3840×2160 @ 60 (default)                  |
| Max framerate    | 1920×1080 @ 144                           |

Modes below 24 fps are never chosen automatically.

**2. NV12 direct capture** (Advanced, on by default). RGB32 output forces a full-frame color conversion inside Media Foundation for every frame — at 4K144 that alone is several GB/s. With NV12 the frames arrive in the card's native layout and the pixel shader converts them, using the YUV matrix and range that MF negotiated. Turn it off only if colors look wrong on an unusual device; the app falls back to RGB32 automatically if the driver rejects NV12.

**3. Presentation mode** (Advanced).

- **V-Sync** — one frame per display refresh, paced by the compositor's waitable object. Smoothest; caps the app at your monitor's Hz.
- **Uncapped** — `DXGI_PRESENT_ALLOW_TEARING`, presents as soon as a frame is drawn. Use this when the capture runs faster than the desktop refresh (e.g. 144 Hz capture on a 60 Hz screen) and you want OBS/Discord to see every frame. The optional **FPS limit** slider keeps the loop from spinning faster than frames actually arrive — set it to your capture framerate.

The status line shows what was actually negotiated, e.g. `3840x2160 @ 60  NV12  GPU • capture 60 fps • render 60 fps`, plus the current display refresh rate.

> If a mode doesn't behave, turn on **Debug console** in Advanced — it prints every native type the device offers, which one was chosen, and the negotiated output format.

---

## Latency

The Audio panel shows live measurements: `Measured: 21 ms audio • 8 ms video (capture → present)`.

**Audio** end-to-end delay is `capture period + queued audio + render period`.

- **Audio delay** slider (Audio panel) sets how much is deliberately kept queued. It applies immediately — drag it down until you hear crackle, then back off. 20–30 ms suits most setups.
- **Low-latency audio engine** (Advanced) asks the driver for its minimum engine period via `IAudioClient3`, typically ~3 ms instead of the default 10 ms. It falls back automatically on endpoints that refuse, and applies on the next Start.
- The queue is trimmed from the oldest end every period. Capture and playback devices run off independent clocks, so without trimming any hiccup permanently adds delay and the buffer eventually sits full for the rest of the session.
- If Advanced reports underruns, raise the Audio delay.

**Video** latency is measured from the moment Media Foundation hands over a frame to the moment it is presented.

- **Performance mode** (Advanced) holds the pre-render queue at one frame.
- **Uncapped** presentation removes the wait for the display's vblank.
- **NV12 direct capture** removes a full-frame color conversion from the path.

---

## Updates

The app checks GitHub Releases on startup (toggleable) and from **Updates → Check now**. When a newer version is published it downloads the release's `DesktopCamApp.exe`, verifies it is a real executable, then swaps the binary and relaunches on **Install and restart**. Nothing is sent anywhere — it is a plain HTTPS GET of the public releases API.

Downloads are cached in `%LOCALAPPDATA%\DesktopCamApp\updates`. If the app lives somewhere that needs administrator rights (e.g. `C:\Program Files`), the swap step asks for elevation.

### Publishing a release

`set(DCA_VERSION "...")` in `CMakeLists.txt` is the single source of truth for the version. Bump it, commit, then tag the same commit:

```bash
git tag v1.2.1
git push origin main --tags
```

`.github/workflows/release.yml` **fails the release if the tag and `DCA_VERSION` disagree**, then builds x64 Release and publishes `DesktopCamApp.exe` plus a zip. **The asset must keep the name `DesktopCamApp.exe`** — that is what the updater looks for.

> Don't make `DCA_VERSION` a CMake cache variable. A cached value sticks to whatever an existing build directory was first configured with, so bumping the line does nothing, the binary keeps reporting a stale version, and it offers itself the same update forever.

---

## Dependencies

Automatically fetched by CMake (`FetchContent`):

- [Dear ImGui](https://github.com/ocornut/imgui) v1.91.5 (with `imgui_impl_win32` + `imgui_impl_dx11` backends)

Windows SDK libraries (shipped with VS + Windows 10/11 SDK — no installation needed):

- `d3d11`, `dxgi`, `d3dcompiler`, `dxguid`
- `mf`, `mfplat`, `mfreadwrite`, `mfuuid`
- `ole32`, `oleaut32`, `uuid`, `propsys`
- `avrt`, `winmm`, `ksuser`
- `winhttp`, `shell32`

Optional at runtime: the [NVIDIA Broadcast SDK](https://www.nvidia.com/en-us/geforce/broadcasting/broadcast-sdk/resources/) redistributable for AI upscaling.

---

## Build instructions

### Requirements

- Windows 10 or 11
- Visual Studio 2019 (16.11+), 2022, or 2026 with the "Desktop development with C++" workload
- Windows 10 or 11 SDK (installed with VS)
- CMake 3.20+
- Git (for the ImGui fetch)

### Configure + build (Release)

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The binary lands at `build\Release\DesktopCamApp.exe`.

To point a private fork's updater at your own repo:

```bat
cmake -S . -B build -A x64 -DDCA_GITHUB_OWNER=you -DDCA_GITHUB_REPO=YourRepo
```

### Open in Visual Studio

After the first configure you can open `build\DesktopCamApp.sln`; `DesktopCamApp` is already the startup project.

---

## Running

1. Launch `DesktopCamApp.exe`.
2. Pick a camera, optionally a capture **Mode**, a microphone, and (optionally) a specific output device.
3. Click **Start**.

The app never uses `WS_EX_LAYERED` or transparency, so OBS hooking is reliable in windowed, borderless, and fullscreen modes.

---

## Using with OBS (Game Capture)

1. OBS → Sources → `+` → **Game Capture**.
2. **Mode**: `Capture specific window`.
3. **Window**: select `[DesktopCamApp.exe]: Desktop Cam App`.
4. **Hook Rate**: `Normal`.
5. Leave `Allow transparency` off.

If Game Capture doesn't hook the window:

- Make sure OBS runs on the same GPU as the app (right-click OBS → Run with Graphics Processor).
- Try `Capture foreground window with hotkey`.
- `Window Capture (BitBlt)` also works — the app renders into a normal window.

---

## Using with Discord (Screen Share)

1. Join a voice channel (or DM call).
2. **Share Your Screen** → **Application** tab → select `Desktop Cam App`.
3. Toggle **Sound** on.

Because the app plays mic audio through WASAPI shared mode, Discord's per-application audio share picks it up.

> **Avoid feedback.** Use headphones if your microphone is near your speakers — the app intentionally plays the mic through your output device so Discord's application-audio share can capture it.

---

## Controls

| Action                          | Control                               |
|---------------------------------|---------------------------------------|
| Start / Stop capture            | Button in the ImGui panel             |
| Change camera / mode / mic      | Dropdowns in the panel (pick + Start) |
| Mic volume (0 – 200 %)          | Slider                                |
| Hide / show the UI              | `F1`                                  |
| Toggle borderless fullscreen    | `Alt+Enter` or the UI button          |
| Close                           | `Alt+F4` or the window's X            |

---

## Performance notes

- Video frames stay on the GPU: Media Foundation hands us a D3D11 texture and we `CopySubresourceRegion` it into a shader-visible texture. The CPU byte path only runs when a driver can't do DXVA advanced video processing, and is not viable at 4K.
- NV12 capture removes a full-frame color conversion per frame; the shader does it for free during the draw.
- Performance mode sets the swap chain's maximum frame latency to 1; Standard uses 2–3 for smoothness.
- The audio ring buffer is ~100 ms; end-to-end passthrough latency is typically 20–60 ms.
- Audio threads register with MMCSS (`Pro Audio`) for real-time priority.

---

## Troubleshooting

**Black video with "LIVE" status**
The camera is probably held exclusively by another app. Close browser tabs using the camera, or Zoom/Teams/Discord Video, and try again.

**A 4K or 144 Hz mode isn't offered**
The list only ever shows what the device reports. USB bandwidth matters: a 4K60 card usually needs USB 3.x on a controller that isn't shared. Enable the debug console to see the raw list of native types.

**Colors look washed out or too contrasty**
Turn off **NV12 direct capture** in Advanced and press Start again — that falls back to Media Foundation's own RGB32 conversion.

**Capture is 144 fps but the app shows ~60 fps**
That is the display refresh. Switch Presentation to **Uncapped** in Advanced.

**`SetCurrentMediaType(NV12/RGB32) failed`**
The device rejected both output formats. Install the latest GPU drivers, and try pinning a specific Mode instead of Auto.

**Left and right audio channels are swapped**
Fixed in 1.2.0. The passthrough ring buffer used to count samples rather than frames, so a single truncated transfer (which happened whenever the output device stalled long enough to fill the buffer) shifted the stereo interleaving by one sample and swapped the channels for the rest of the session. It now transfers whole frames only. The **Swap L / R** checkbox remains for devices that genuinely report the channels reversed.

**Audio crackles or drops out**
Raise **Audio delay** in the Audio panel. Advanced shows the underrun/overrun counters and the negotiated device periods. If a particular endpoint misbehaves, turn off **Low-latency audio engine** in Advanced and press Start again.

**No audio in Discord / OBS**
Check that **Start** is clicked, the peak meter is animating, and the selected *Output* device is the one Discord captures.

---

## License

MIT — see [LICENSE](LICENSE).
