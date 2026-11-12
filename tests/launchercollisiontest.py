#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile

launcher = Path(sys.argv[1]).resolve()
common = subprocess.check_output(["git", "-C", str(launcher.parent), "rev-parse", "--path-format=absolute", "--git-common-dir"], text=True).strip()
repository = Path(common).parent
scratch = repository / "tmp"
scratch.mkdir(exist_ok=True)
namespaces = set()
with tempfile.TemporaryDirectory(prefix="launcher-", dir=scratch) as temporary:
    root = Path(temporary)
    prefix = root / "prefix"
    (prefix / "bin").mkdir(parents=True)
    (prefix / "lib/fooyin/plugins").mkdir(parents=True)
    (prefix / "lib/fooyin/plugins/fyplugin_jellyfin.so").touch()
    executable = prefix / "bin/fooyin"
    executable.write_text('''#!/usr/bin/env python3
import json, os, socket, sys
from pathlib import Path
runtime = Path(os.environ["TMPDIR"])
endpoint = runtime / "instance.sock"
if endpoint.exists():
    client = socket.socket(socket.AF_UNIX)
    client.connect(str(endpoint))
    client.sendall(json.dumps(sys.argv[1:]).encode())
    client.close()
Path(sys.argv[0]).parents[1].joinpath("probe.json").write_text(json.dumps({"TMPDIR": str(runtime), "HOME": os.environ["HOME"], "arguments": sys.argv[1:]}))
''')
    executable.chmod(0o700)
    parent_namespace = root / "parent"
    parent_namespace.mkdir()
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(str(parent_namespace / "instance.sock"))
    listener.listen()
    environment = os.environ.copy()
    environment["TMPDIR"] = str(parent_namespace)
    arguments = ["--play", "synthetic track with spaces.flac"]
    first = root / "profile-first"
    subprocess.run([str(launcher), "--host-prefix", str(prefix), "--profile-dir", str(first), "--", *arguments], env=environment, check=True)
    probe = json.loads((prefix / "probe.json").read_text())
    assert probe["arguments"] == arguments
    assert probe["HOME"] == str(first / "home")
    runtime = Path(probe["TMPDIR"])
    namespaces.add(runtime)
    assert runtime != parent_namespace
    assert runtime.parent == scratch
    assert runtime.name.startswith("ipc-")
    assert len(str(runtime)) <= 65
    assert runtime.stat().st_mode & 0o777 == 0o700
    assert not select.select([listener], [], [], 0)[0]
    second = root / "profile-second"
    subprocess.run([str(launcher), "--host-prefix", str(prefix), "--profile-dir", str(second), "--", *arguments], env=environment, check=True)
    second_runtime = Path(json.loads((prefix / "probe.json").read_text())["TMPDIR"])
    namespaces.add(second_runtime)
    assert second_runtime != runtime
    assert not select.select([listener], [], [], 0)[0]
    listener.close()
for runtime in namespaces:
    runtime.rmdir()
print("Isolated launch arguments and profile-specific IPC namespaces verified")
