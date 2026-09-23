# PlatformIO pre-build script: sets FW_VERSION.
# CI passes the tag (e.g. v1.2.0) in $FW_VERSION; local builds use `git describe`.
import os
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

version = os.environ.get("FW_VERSION", "").strip()
if not version:
    try:
        version = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env["PROJECT_DIR"], stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        version = "dev"
version = version[1:] if version.startswith("v") else version

env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
print(f"Firmware version: {version}")
