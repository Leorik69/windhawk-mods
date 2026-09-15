#!/usr/bin/env python3
"""Compile-check Windhawk mods on Linux.

For each ``mods/<id>.wh.cpp`` file this validates the metadata block and runs a
clang compile check for every architecture the mod declares, using the same
flags Windhawk's own compiler uses (see ``scripts/compile_mod.py`` upstream).

The check is a front-end + code-generation compile (``-fsyntax-only``), which is
what the Windhawk editor uses for live diagnostics. Linking into the final mod
DLL is done by the Windhawk engine on Windows and is intentionally not attempted
here, so the engine import library is not required.

Toolchain: an ``llvm-mingw`` install providing ``<triple>-w64-mingw32-clang++``.
It is located via ``$LLVM_MINGW_DIR/bin`` or the ``PATH``.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Windhawk targets Windows 10+, see scripts/compile_mod.py upstream.
VERSION_DEFINES = [
    "-DWINVER=0x0A00",
    "-D_WIN32_WINNT=0x0A00",
    "-D_WIN32_IE=0x0A00",
    "-DNTDDI_VERSION=0x0A000008",
]

# Mod @architecture value -> clang target triples.
ARCH_TO_TRIPLES = {
    "x86": ["i686-w64-mingw32"],
    "amd64": ["x86_64-w64-mingw32"],
    "arm64": ["aarch64-w64-mingw32"],
    "x86-64": ["x86_64-w64-mingw32", "aarch64-w64-mingw32"],
}

DEFAULT_TRIPLES = [
    "i686-w64-mingw32",
    "x86_64-w64-mingw32",
    "aarch64-w64-mingw32",
]


@dataclass
class ModInfo:
    id: str
    version: str
    compiler_options: str | None
    triples: list[str]


class MetadataError(Exception):
    pass


def find_headers_dir() -> Path:
    candidates = sorted((REPO_ROOT / ".vscode").glob("windhawk_headers_*"))
    if not candidates:
        raise SystemExit(
            "Could not find a .vscode/windhawk_headers_* directory with the SDK headers."
        )
    return candidates[-1]


DEFAULT_LLVM_MINGW_DIR = Path.home() / ".local" / "share" / "llvm-mingw"


def find_compiler(triple: str) -> str | None:
    name = f"{triple}-clang++"
    search_dirs = []
    llvm_dir = os.environ.get("LLVM_MINGW_DIR")
    if llvm_dir:
        search_dirs.append(Path(llvm_dir))
    search_dirs.append(DEFAULT_LLVM_MINGW_DIR)
    for base in search_dirs:
        candidate = base / "bin" / name
        if candidate.is_file():
            return str(candidate)
    return shutil.which(name)


def parse_metadata(mod_file: Path) -> ModInfo:
    mod_id: str | None = None
    version: str | None = None
    compiler_options: str | None = None
    architectures: set[str] = set()

    inside = False
    seen_block = False
    for raw in mod_file.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip()
        if not inside:
            if re.fullmatch(r"//[ \t]+==WindhawkMod==[ \t]*", line):
                inside = True
                seen_block = True
            continue

        if re.fullmatch(r"//[ \t]+==/WindhawkMod==[ \t]*", line):
            inside = False
            break

        if line.strip() == "":
            continue

        match = re.fullmatch(
            r"//[ \t]+@([a-zA-Z]+)(?::([a-z]{2}(?:-[A-Z]{2})?))?[ \t]+(.*)",
            line.strip(),
        )
        if not match:
            raise MetadataError(f"Invalid metadata line: {line.strip()!r}")

        key, _language, value = match.group(1), match.group(2), match.group(3)
        if key == "id":
            mod_id = value.strip()
        elif key == "version":
            version = value.strip()
        elif key == "compilerOptions":
            compiler_options = value.strip()
        elif key == "architecture":
            architectures.add(value.strip())

    if not seen_block:
        raise MetadataError("No ==WindhawkMod== metadata block found")
    if mod_id is None:
        raise MetadataError("@id is not specified")
    if version is None:
        raise MetadataError("@version is not specified")

    expected_id = mod_file.name[: -len(".wh.cpp")]
    if mod_id != expected_id:
        raise MetadataError(
            f"@id {mod_id!r} does not match file name (expected {expected_id!r})"
        )

    if architectures:
        triples: list[str] = []
        for arch in sorted(architectures):
            if arch not in ARCH_TO_TRIPLES:
                raise MetadataError(f"Invalid @architecture: {arch!r}")
            for triple in ARCH_TO_TRIPLES[arch]:
                if triple not in triples:
                    triples.append(triple)
    else:
        triples = list(DEFAULT_TRIPLES)

    return ModInfo(mod_id, version, compiler_options, triples)


def split_compiler_options(options: str | None) -> list[str]:
    if not options:
        return []
    if '"' in options:
        raise MetadataError("Compiler options cannot contain double quotes")
    return options.split()


def is_link_only_arg(arg: str) -> bool:
    # Linking is Windhawk's job; drop link-only flags for the compile check so
    # they don't produce "unused argument" noise.
    return arg.startswith("-l") or arg.startswith("-L") or arg.startswith("-Wl,")


@dataclass
class Result:
    triple: str
    ok: bool
    output: str = ""


@dataclass
class ModResult:
    mod_file: Path
    error: str | None = None
    results: list[Result] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.error is None and all(r.ok for r in self.results)


def check_mod(mod_file: Path, headers_dir: Path, verbose: bool) -> ModResult:
    try:
        info = parse_metadata(mod_file)
        extra_args = split_compiler_options(info.compiler_options)
    except MetadataError as exc:
        return ModResult(mod_file, error=str(exc))

    compile_extra = [a for a in extra_args if not is_link_only_arg(a)]

    mod_result = ModResult(mod_file)
    for triple in info.triples:
        compiler = find_compiler(triple)
        if compiler is None:
            mod_result.results.append(
                Result(triple, False, f"compiler not found: {triple}-clang++")
            )
            continue

        args = [
            compiler,
            "-std=c++23",
            "-fsyntax-only",
            "-DUNICODE",
            "-D_UNICODE",
            *VERSION_DEFINES,
            "-D__USE_MINGW_ANSI_STDIO=0",
            "-DWH_MOD",
            f'-DWH_MOD_ID=L"{info.id}"',
            f'-DWH_MOD_VERSION=L"{info.version}"',
            "-I",
            str(headers_dir),
            "-include",
            "windhawk_api.h",
            "-target",
            triple,
            "-x",
            "c++",
            str(mod_file),
            *compile_extra,
        ]
        proc = subprocess.run(args, capture_output=True, text=True)
        output = (proc.stdout + proc.stderr).strip()
        if verbose and output:
            print(output)
        mod_result.results.append(Result(triple, proc.returncode == 0, output))

    return mod_result


def collect_mod_files(paths: list[str]) -> list[Path]:
    if paths:
        files: list[Path] = []
        for p in paths:
            path = Path(p)
            if path.is_dir():
                files.extend(sorted(path.rglob("*.wh.cpp")))
            else:
                files.append(path)
        return files
    return sorted((REPO_ROOT / "mods").rglob("*.wh.cpp"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "mods",
        nargs="*",
        help="Mod files or directories (default: all mods in mods/).",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Print full compiler output."
    )
    args = parser.parse_args()

    headers_dir = find_headers_dir()
    mod_files = collect_mod_files(args.mods)
    if not mod_files:
        print("No mods found to check.")
        return 0

    print(f"Using SDK headers: {headers_dir.relative_to(REPO_ROOT)}")
    print(f"Checking {len(mod_files)} mod(s)\n")

    all_ok = True
    for mod_file in mod_files:
        rel = mod_file.relative_to(REPO_ROOT) if mod_file.is_relative_to(REPO_ROOT) else mod_file
        result = check_mod(mod_file, headers_dir, args.verbose)
        if result.error:
            all_ok = False
            print(f"FAIL {rel}: {result.error}")
            continue

        status = "OK  " if result.ok else "FAIL"
        arches = ", ".join(
            f"{r.triple.split('-')[0]}{'' if r.ok else ' (failed)'}"
            for r in result.results
        )
        print(f"{status} {rel}  [{arches}]")
        if not result.ok:
            all_ok = False
            for r in result.results:
                if not r.ok and r.output:
                    print(f"    --- {r.triple} ---")
                    for line in r.output.splitlines():
                        print(f"    {line}")

    print()
    print("All mods compiled successfully." if all_ok else "Some mods failed to compile.")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
