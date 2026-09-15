# windhawk-mods

A workspace for creating [Windhawk](https://windhawk.net/) mods. Each mod is a
single C++ source file, `mods/<mod-id>.wh.cpp`, that Windhawk compiles and
injects into running programs on Windows.

## Repository layout

| Path | Purpose |
| --- | --- |
| `mods/` | Mod source files (`<mod-id>.wh.cpp`). |
| `.vscode/windhawk_headers_*/` | Bundled Windhawk SDK headers (`windhawk_api.h`, `windhawk_utils.h`) used for IntelliSense and the compile check. |
| `.vscode/c_cpp_properties.json` | C/C++ IntelliSense configuration for the headers above. |
| `scripts/check_mod.py` | Compile-checks mods on Linux for every declared architecture. |
| `.cursor/install.sh` | Installs the cross toolchain used by the checker. |
| `.clang-format` | Formatting style used by mods (Chromium, 4-space indent). |

## Developing a mod

A mod file starts with metadata, readme and settings blocks, followed by the
C++ code. See `mods/disable-message-beep.wh.cpp` for a complete, working
example, and the official guide
[Creating a New Mod](https://github.com/ramensoftware/windhawk/wiki/creating-a-new-mod).

## Checking a mod compiles

The checker validates the metadata and runs a clang compile check for every
architecture the mod declares (`x86` / `amd64` / `arm64`), using the same flags
Windhawk's compiler uses.

```sh
# Check every mod under mods/
python3 scripts/check_mod.py

# Check a specific mod, printing full compiler output
python3 scripts/check_mod.py mods/disable-message-beep.wh.cpp -v
```

Linking into the final mod DLL is performed by the Windhawk engine on Windows,
so the checker stops at the compile stage and does not need the engine library.

## Toolchain

The checker uses [`llvm-mingw`](https://github.com/mstorsjo/llvm-mingw), which
provides `clang++` targeting `i686`, `x86_64` and `aarch64-w64-mingw32` — the
same three architectures Windhawk builds. `.cursor/install.sh` installs it to
`~/.local/share/llvm-mingw` (idempotently) and the checker finds it there
automatically. To install or point at a custom location manually:

```sh
./.cursor/install.sh
# or, if you already have llvm-mingw elsewhere:
export LLVM_MINGW_DIR=/path/to/llvm-mingw
```
