#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import subprocess
import sys
import tempfile

launcher = Path(sys.argv[1]).resolve()
common = subprocess.check_output(["git", "-C", str(launcher.parent), "rev-parse", "--path-format=absolute", "--git-common-dir"], text=True).strip()
scratch = Path(common).parent / "tmp"
scratch.mkdir(exist_ok=True)


def check_missing_value(arguments, profile):
    result = subprocess.run([str(launcher), *arguments], capture_output=True, text=True)
    assert result.returncode == 2, (arguments, result.returncode, result.stderr)
    assert result.stderr.startswith(f"Missing value for {arguments[-1]}\n"), result.stderr
    assert "Usage: " in result.stderr and "--host-prefix DIR --profile-dir DIR" in result.stderr, result.stderr
    assert not profile.exists()


with tempfile.TemporaryDirectory(prefix="launcher-", dir=scratch) as temporary:
    root = Path(temporary)
    prefix = root / "prefix"
    profile = root / "profile"
    check_missing_value(["--host-prefix"], profile)
    check_missing_value(["--profile-dir", str(profile), "--host-prefix"], profile)
    check_missing_value(["--host-prefix", str(prefix), "--profile-dir"], profile)
print("Missing option values are rejected with the usage text")
