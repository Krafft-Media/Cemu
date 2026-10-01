# Cemu for Dispatcher

This branch (`dispatcher`) is Cemu 2.6 with a small automation layer for
[Dispatcher](https://github.com/Krafft-Media/Dispatcher), which drives Cemu sessions for game
modding (memory, debugging, the Skylanders portal, graphic-pack patches, input). Everything
else is upstream Cemu; see [README.md](README.md) and [BUILD.md](BUILD.md).

## Additions

- `--user-data-dir <dir>`: use `<dir>` for settings, controller profiles, graphic packs, caches
  and the log, like portable mode, so several instances run side by side.
- `--control-port <port>`: a control server on `127.0.0.1:<port>` (`src/gui/DispatcherControl.cpp`).
  Newline-delimited JSON, one request per line: `{"id": 1, "cmd": "hello", "args": {}}` →
  `{"id": 1, "ok": true, "result": {...}}`. When `CEMU_DISPATCHER_TOKEN` is set, a connection
  must first send `{"cmd": "auth", "args": {"token": "..."}}`. Commands: `hello`, `quit`,
  `memory.read`, `memory.write`, `memory.find`, `portal.list`, `portal.load`, `portal.remove`,
  `portal.create`, `portal.known`, `packs.list`, `packs.rescan`, `packs.set`, `packs.reload`.
- `--log-stdout`: mirror `log.txt` to stdout.
- `--gdbstub-no-entry-stop`: with `--enable-gdbstub`, let the title run instead of waiting at its
  entry point for a debugger.

## Fixes

- The GDB stub listens on loopback only (it has no authentication) and accepts a new debugger
  after a disconnect instead of spinning on the closed socket.
- Breakpoint and watchpoint hits are reported to the debugger even when it attached to a running
  title (`--gdbstub-no-entry-stop`) and has not continued it yet.
- Single-stepping over a `blr` no longer crashes Cemu: the stub read the thread's saved LR
  without swapping its byte order, so it planted the step breakpoint at a garbage address.
  Conditional branches with absolute targets are decoded correctly, and step targets outside
  mapped memory are skipped.
- A debugger that disconnects, or a `quit` on the control socket, removes the stub's
  breakpoints and resumes the threads it paused, so the title neither stays frozen nor blocks
  Cemu from closing.
- The DSU client asks a silent server for pad data again every second, so input connects
  whichever side starts first.
- Graphic packs inside a symlinked or junctioned `graphicPacks` folder keep their
  `graphicPacks/...` path, so their enabled state and presets in settings.xml match.

## Building on Windows

`build-dispatcher.cmd` configures and builds `bin/Cemu_release.exe` with Visual Studio 2022's
CMake and Ninja. The vcpkg version pinned by Cemu 2.6 downloads an msys2 `pkgconf` package the
mirrors no longer carry; the script uses MSYS2's `pkg-config` when it is installed at
`C:\msys64`. It also writes `bin/Cemu_release.pdb`, so a crash log names Cemu's functions.

License: MPL-2.0, like Cemu.
