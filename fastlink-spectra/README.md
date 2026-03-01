# FastLink Spectra

Standalone Windows D3D11 visualizer for SNES9X FastLink memory activity.

## Sources

- `Memory-Mapped` (default): reads from `Local\\Snes9xFastLink.Memory.v1` and event `Local\\Snes9xFastLink.FrameReady.v1`.
- `REST API`: polls `http://127.0.0.1:9000/` as `application/octet-stream`.

## Controls

- Right-click inside the window to open the menu.
- Configure `View`, `Effects`, `Color Mode`, and `Source`.

## Build

From `fastlink-spectra/`:

```bash
./build.sh
```

Output EXE: `build/FastLinkSpectra.exe`

## Notes

This is a first-pass implementation focused on core functionality and smoothness using hardware-accelerated D3D11 presentation.
