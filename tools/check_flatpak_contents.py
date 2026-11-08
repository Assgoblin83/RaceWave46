#!/usr/bin/env python3
"""Check that a RaceWave46 Flatpak contains no ROM-derived code.

The distributable package ships the host, the recompilers, the Clang toolchain
and the module sources, but never code generated from a ROM. This script
inspects an exploded flatpak-builder tree (a build directory or its `files/`
directory) or a `.flatpak` bundle and fails when it finds generated sources,
a ROM image or recompiled `func_8...` symbols in the host binary.

Usage: tools/check_flatpak_contents.py PATH [--expect-runtime]
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

# Generated code and ROM images must never be in the package.
FORBIDDEN_NAMES = [
    (re.compile(r"^funcs(_[0-9]+)?\.(c|h)$", re.IGNORECASE),
     "recompiled game source"),
    (re.compile(r"^aspmain\.(c|cc|cpp|cxx|h|hpp)$", re.IGNORECASE),
     "recompiled RSP microcode source"),
    (re.compile(r"\.z64$|\.n64$|\.v64$", re.IGNORECASE),
     "ROM image"),
    (re.compile(r"^recomp_overlays\.inl$", re.IGNORECASE),
     "recompiled overlay table"),
    (re.compile(r"^lookup\.cpp$", re.IGNORECASE),
     "recompiled function lookup table"),
]

# Recompiled N64 functions keep their address as the symbol name.
FUNC_SYMBOL = re.compile(rb"^func_8[0-9A-Fa-f]{4,}$")
ASP_SYMBOL = re.compile(rb"^aspMain$")


def walk_files(root):
    for directory, _, names in os.walk(root):
        for name in names:
            yield os.path.join(directory, name)


def check_names(root, errors):
    for path in walk_files(root):
        if not os.path.isfile(path):
            continue
        name = os.path.basename(path)
        for pattern, description in FORBIDDEN_NAMES:
            if pattern.search(name):
                errors.append(
                    f"{os.path.relpath(path, root)}: {description} must not be packaged")
                break


def elf_symbols(path):
    """Yield the names in an ELF64 little-endian symbol table."""
    with open(path, "rb") as stream:
        data = stream.read()
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return
    try:
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
        if shoff == 0 or shentsize < 64 or shnum == 0 or shoff + shnum * shentsize > len(data):
            return
        sections = []
        for index in range(shnum):
            base = shoff + index * shentsize
            name, kind, _, _, offset, size, link, _, _, entsize = struct.unpack_from(
                "<IIQQQQIIQQ", data, base)
            sections.append((name, kind, offset, size, link, entsize))
        string_tables = {}
        for name, kind, offset, size, link, entsize in sections:
            if kind == 3 and offset + size <= len(data):  # SHT_STRTAB
                string_tables[offset] = data[offset:offset + size]
        for name, kind, offset, size, link, entsize in sections:
            if kind not in (2, 11):  # SHT_SYMTAB, SHT_DYNSYM
                continue
            if entsize < 24 or offset + size > len(data):
                continue
            strings = string_tables.get(sections[link][2]) if link < len(sections) else None
            if not strings:
                continue
            count = size // entsize
            for index in range(count):
                base = offset + index * entsize
                st_name, = struct.unpack_from("<I", data, base)
                end = strings.find(b"\0", st_name)
                if st_name == 0 or end < 0:
                    continue
                yield strings[st_name:end]
    except (struct.error, IndexError):
        return


def check_symbols(root, errors):
    hosted_roots = [os.path.join(root, "lib", "racewave46")]
    seen = set()
    for directory in hosted_roots:
        if not os.path.isdir(directory):
            continue
        for path in walk_files(directory):
            relative = os.path.relpath(path, root)
            if "/tools/" in "/" + relative.replace(os.sep, "/"):
                continue
            if not os.path.isfile(path):
                continue
            real = os.path.realpath(path)
            if real in seen:
                continue
            seen.add(real)
            try:
                with open(path, "rb") as stream:
                    if stream.read(4) != b"\x7fELF":
                        continue
            except OSError:
                continue
            for symbol in elf_symbols(path):
                if FUNC_SYMBOL.match(symbol) or ASP_SYMBOL.match(symbol):
                    errors.append(
                        f"{relative}: host imports game-derived symbol "
                        f"{symbol.decode('ascii', 'replace')}")
                    break


REQUIRED_RUNTIME = [
    "lib/racewave46/WaveRace64Recompiled",
    "lib/racewave46/librashader.so",
    "lib/racewave46/tools/N64Recomp",
    "lib/racewave46/tools/RSPRecomp",
    "lib/racewave46/tools/clang",
    "lib/racewave46/tools/lld",
    "lib/racewave46/tools/libLLVM.so.22.1",
    "lib/racewave46/tools/libclang-cpp.so.22.1",
    "lib/racewave46/lib/clang/22/include/stddef.h",
    "lib/racewave46/assets/recomp.rcss",
    "share/racewave46/config/generated-code.json",
    "share/racewave46/config/us.rev1.toml",
    "share/racewave46/config/aspMain.us.rev1.toml",
    "share/racewave46/symbols/waverace64.us.rev1.toml",
    "share/racewave46/data/waverace64-widescreen.ips",
    "share/racewave46/include/wr64_game_module.h",
    "share/racewave46/include/recomp.h",
    "share/racewave46/include/librecomp/rsp.hpp",
    "share/racewave46/include/librecomp/sections.h",
    "share/racewave46/include/ultramodern/ultra64.h",
    "share/racewave46/src/game_module/wr64_game_module_abi.cpp",
    "share/racewave46/sysroot/c++/16.2.0/vector",
    "share/racewave46/sysroot/include/stdio.h",
    "share/racewave46/sysroot/include/x86_64-linux-gnu/bits/libc-header-start.h",
]


def check_runtime(root, errors):
    for relative in REQUIRED_RUNTIME:
        if not os.path.exists(os.path.join(root, relative)):
            errors.append(f"{relative}: required for the in-app game module build is missing")


def application_id(metadata):
    try:
        with open(metadata, "r", encoding="utf-8") as stream:
            for line in stream:
                if line.startswith("name="):
                    return line.strip().split("=", 1)[1]
    except OSError:
        pass
    return "io.github.Assgoblin83.RaceWave46"


def check_desktop(root, app_id, errors):
    for relative in (
        f"share/applications/{app_id}.desktop",
        f"share/metainfo/{app_id}.metainfo.xml",
        f"share/icons/hicolor/256x256/apps/{app_id}.png",
    ):
        if not os.path.exists(os.path.join(root, relative)):
            errors.append(f"{relative}: desktop integration file is missing")


def extract_bundle(bundle, work):
    """Check out a .flatpak bundle into work using the local flatpak tools."""
    repo = os.path.join(work, "repo")
    tree = os.path.join(work, "tree")
    subprocess.run(["ostree", "init", "--repo", repo, "--mode=archive"], check=True)
    subprocess.run(["flatpak", "build-import-bundle", repo, bundle], check=True)
    refs = subprocess.run(
        ["ostree", "refs", f"--repo={repo}"], check=True, capture_output=True,
        text=True).stdout
    refs = [line.strip() for line in refs.splitlines() if line.strip().startswith("app/")]
    if not refs:
        raise SystemExit(f"{bundle}: no application ref in the bundle")
    subprocess.run(["ostree", "checkout", f"--repo={repo}", "-U", refs[0], tree],
                   check=True)
    metadata = os.path.join(work, "metadata")
    subprocess.run(["ostree", "cat", f"--repo={repo}", refs[0], "/metadata"],
                   check=True, stdout=open(metadata, "wb"))
    return tree, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", help="flatpak-builder directory, files/ directory or .flatpak")
    parser.add_argument("--expect-runtime", action="store_true",
                        help="also require the compiler, headers and module sources")
    arguments = parser.parse_args()

    errors = []
    temporary = None
    path = os.path.abspath(arguments.path)
    if path.endswith(".flatpak"):
        temporary = tempfile.TemporaryDirectory(prefix="racewave46-flatpak-")
        try:
            root, metadata = extract_bundle(path, temporary.name)
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"{arguments.path}: cannot read bundle: {error}", file=sys.stderr)
            return 2
        if os.path.isdir(os.path.join(root, "files")):
            root = os.path.join(root, "files")
        app_id = application_id(metadata)
    else:
        root = path
        if os.path.isdir(os.path.join(path, "files")):
            build_directory = path
            root = os.path.join(path, "files")
        else:
            build_directory = os.path.dirname(path)
        app_id = application_id(os.path.join(build_directory, "metadata"))

    check_names(root, errors)
    check_symbols(root, errors)
    check_desktop(root, app_id, errors)
    if arguments.expect_runtime:
        check_runtime(root, errors)

    if temporary is not None:
        temporary.cleanup()

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        print(f"FAILED: {len(errors)} problem(s) in {arguments.path}", file=sys.stderr)
        return 1
    print(f"OK: no ROM-derived files or symbols in {arguments.path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
