# WinMain Control Flow

## 1. Initialization Phase
* **Environment/Config:** Set working directory, hook standard handles (stdout/stderr legacy). Initialize config modules. 
* **Command Line/Settings:** Parse CLI, load `snes9x.conf`, load controllers.
* **Window/GUI Init:** Register Window classes, instantiate the main window, hook device notifications for HID.
* **Emulation Core Start:** Initialize render filters, `S9xInit()`, setup timers.
* **Timers:** `timeBeginPeriod` for 1ms multimedia timers (affects Windows thread scheduling).
* **Game Load:** Process CLI string, start ROM loading. Map default keymaps to `XInput / DirectInput`.

## 2. Main Emulation Loop
The loop bounds are `while(TRUE)` with an inner Windows Message queue handler.
* **Message Pump:** 
  If `StopEmulation` or paused, it invokes `GetMessage` (which blocks, yielding the thread). If running, it rapidly checks `PeekMessage` non-destructively, then extracts using `GetMessage` - this is suboptimal and legacy.
* **Cheats & Watches:** Legacy loop across all memory watcher elements doing raw pointer decrements and `CopyMemory` per-frame.
* **State Management:** Frame advance decrements, Rewind buffer sampling (`push/pop`).
* **Side-car IPC (FastLink):** Checks boolean states for `MemoryServe` and `MemoryMapServe` and spins up `std::thread` dynamically if toggled mid-runtime.
* **Emulation Step:** `ProcessInput()` grabs frames, `S9xMainLoop()` steps the CPU/PPU.
* **Share Publish:** Posts exact memory references mapped by FastLink.

## 3. Teardown / Cleanup
* Triggered on `WM_QUIT`. Waits for all `std::thread` to safely `.join()`.
* **Sub-system Teardown:** Shuts down audio device callbacks, visualization filters, closes hotkey polling.
* **SRAM/Config:** Writes `.srm` to disk. Flushes `snes9x.conf`.
* **De-allocation:** Flushes Core ROM modules and graphics pointers, invokes `DeinitS9x()`.

