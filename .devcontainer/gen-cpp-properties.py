#!/usr/bin/env python3
"""Generate .vscode C++ IntelliSense config from the esphome build.

PlatformIO's compiledb carries every compiler flag inline, but newer esphome
no longer leaves it at the build root, and its entries point at the build-tree
copies of the sources instead of the workspace files open in the editor. So:
run pio compiledb, filter the component TUs, remap their paths back to the
workspace, and point cpptools at the result.
"""

import json
import os
import shlex
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
WORKSPACE = os.path.dirname(HERE)
BUILD = os.path.join(WORKSPACE, ".esphome", "build", "oclean-intellisense")
DB = os.path.join(BUILD, "compile_commands.json")
DEFINES_H = os.path.join(BUILD, "src", "esphome", "core", "defines.h")
BUILD_COMPONENT = os.path.join(BUILD, "src", "esphome", "components", "oclean")
WS_COMPONENT = os.path.join(WORKSPACE, "components", "oclean")
DEST_DIR = os.path.join(WORKSPACE, ".vscode")


def _resolve_compiler(raw):
    found = shutil.which(raw)
    if found or os.path.isabs(raw):
        return found or raw
    pkgs = os.path.join(os.path.expanduser("~"), ".platformio", "packages")
    if os.path.isdir(pkgs):
        for pkg in sorted(os.listdir(pkgs)):
            c = os.path.join(pkgs, pkg, "bin", raw)
            if "toolchain" in pkg and os.path.exists(c):
                return c
    return raw


def _component_entries(db):
    out = []
    for e in db:
        f = e.get("file", "")
        if not os.path.isabs(f):
            f = os.path.normpath(os.path.join(e.get("directory", ""), f))
        if f.startswith(BUILD_COMPONENT + os.sep):
            out.append({**e, "file": WS_COMPONENT + f[len(BUILD_COMPONENT) :]})
        elif f.startswith(WS_COMPONENT + os.sep):
            out.append(e)
    return out


def _unity_src(tests_dir):
    libdeps = os.path.join(tests_dir, ".pio", "libdeps")
    for attempt in range(2):
        for root, _dirs, files in os.walk(libdeps):
            if os.path.basename(root) == "src" and "unity.h" in files:
                return root
        if attempt == 0:
            # unity lands in libdeps only after a native test build
            subprocess.run(
                ["pio", "test", "-d", tests_dir, "-e", "native", "--without-testing"],
                check=False,
                timeout=600,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
    return None


def _native_test_entries():
    tests_dir = os.path.join(WORKSPACE, "tests")
    test_main = os.path.join(tests_dir, "test_protocol", "test_main.cpp")
    if not os.path.exists(test_main):
        return []
    args = [
        shutil.which("g++") or "g++",
        "-std=gnu++17",
        "-I" + os.path.join(WORKSPACE, "components", "oclean"),
    ]
    unity_src = _unity_src(tests_dir)
    if unity_src:
        args.append("-I" + unity_src)
    args += ["-c", test_main]
    return [{"directory": tests_dir, "file": test_main, "arguments": args}]


def main():
    subprocess.run(
        ["pio", "run", "-d", BUILD, "-t", "compiledb"],
        check=False,
        timeout=600,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if not os.path.exists(DB):
        print(f"gen-cpp-properties: {DB} not found (pio compiledb failed?)")
        return 1
    with open(DB) as fh:
        db = json.load(fh)
    entries = _component_entries(db)
    if not entries:
        print(f"gen-cpp-properties: no component TUs in {DB}")
        return 1

    cmd = entries[0].get("command", "")
    argv0 = shlex.split(cmd)[0] if cmd else (entries[0].get("arguments") or [""])[0]
    config = {
        "name": "ESP32",
        "compilerPath": _resolve_compiler(argv0),
        "compilerArgs": ["-mlongcalls"],
        "cStandard": "gnu17",
        "cppStandard": "gnu++20",
        "includePath": ["${workspaceFolder}/components/**"],
        "browse": {
            "path": ["${workspaceFolder}/components/**"],
            "limitSymbolsToIncludedHeaders": True,
        },
        "compileCommands": os.path.join(DEST_DIR, "compile_commands.json"),
    }
    # force-include the esphome defines per component TU, not globally, so it
    # never reaches the native test entry whose file has no esphome includes
    if os.path.exists(DEFINES_H):
        for e in entries:
            if e.get("command"):
                e["command"] += " -include " + shlex.quote(DEFINES_H)
            elif e.get("arguments"):
                e["arguments"] = [*e["arguments"], "-include", DEFINES_H]

    # pio compiledb omits test TUs; synthesize one so cpptools resolves it with
    # native flags per file instead of falling back to the ESP32 config
    entries += _native_test_entries()

    os.makedirs(DEST_DIR, exist_ok=True)
    with open(os.path.join(DEST_DIR, "compile_commands.json"), "w") as fh:
        json.dump(entries, fh, indent=2)
    with open(os.path.join(DEST_DIR, "c_cpp_properties.json"), "w") as fh:
        json.dump({"version": 4, "configurations": [config]}, fh, indent=4)
    print(f"gen-cpp-properties: {len(entries)} TUs -> {DEST_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
