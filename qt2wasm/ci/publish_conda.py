"""fileapi forge job: build the qt2wasm conda package on Depot with pixi-build
and publish it to prefix.dev/nandi-testing.

Runs from the repo's .forge/jobs.toml. Forge jobs already run on Depot
(python:3.12 image, no Docker). With the Depot CLI on PATH (the forge installs
it for a job whose `run` mentions depot) it builds qt2wasm/ci/Dockerfile with
`depot build`, whose cache mounts (pixi/rattler package cache) Depot keeps
between runs; otherwise it builds in place.

1. Read the version from qt2wasm/pixi.toml. If nandi-testing already has
   qt2wasm at that version, stop: bump the version to publish again. So a
   change to the toolchain recipes never rebuilds qt2wasm, and the other way round.
2. `pixi build` the package into qt2wasm/dist.
3. Upload it with `pixi upload prefix` (pixi fetched from its GitHub release).

Secrets (user secrets work): PREFIX_TOKEN, a prefix.dev API key with write
access to nandi-testing (DEPOT_TOKEN and DEPOT_PROJECT_ID for a remote build).
"""

from __future__ import annotations

import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
PKG = HERE.parent  # qt2wasm/
CHANNEL = "nandi-testing"
NAME = "qt2wasm"
UA = {"User-Agent": "qt2wasm-ci/1"}


def say(*a):
    print(*a, flush=True)


def version() -> str:
    m = re.search(r'^\[package\][^\[]*?^version\s*=\s*"([^"]+)"', (PKG / "pixi.toml").read_text(), re.MULTILINE | re.DOTALL)
    if not m:
        sys.exit("no [package] version in qt2wasm/pixi.toml")
    return m.group(1)


def published(ver: str) -> bool:
    url = f"https://prefix.dev/{CHANNEL}/noarch/repodata.json"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
            data = json.load(r)
    except (OSError, ValueError) as e:  # a missing or unreachable channel index: just build
        say(f"could not read {url} ({e}); building anyway")
        return False
    pkgs = {**data.get("packages", {}), **data.get("packages.conda", {})}
    return any(p.get("name") == NAME and p.get("version") == ver for p in pkgs.values())


def pixi() -> str:
    bin_dir = Path(tempfile.mkdtemp())
    url = f"https://github.com/prefix-dev/pixi/releases/latest/download/pixi-{os.uname().machine}-unknown-linux-musl.tar.gz"
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=300) as r:
        data = r.read()
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        tf.extractall(bin_dir, filter="data")
    exe = bin_dir / "pixi"
    exe.chmod(0o755)
    return str(exe)


def main() -> None:
    for var in ("PREFIX_TOKEN",):
        if not os.environ.get(var):
            sys.exit(f"{var} secret is not set (a user or repo secret in fileapi)")
    ver = version()
    if published(ver):
        say(f"{NAME} {ver} is already on {CHANNEL}; nothing to do. Bump the version in "
            "qt2wasm/pixi.toml and pyproject.toml to publish again.")
        return

    dist = PKG / "dist"
    exe = pixi()
    if shutil.which("depot"):  # a runner with the Depot CLI: build remotely
        cmd = ["depot", "build", "--progress", "plain", "--platform", "linux/amd64",
               "-f", str(HERE / "Dockerfile"), "--output", f"type=local,dest={dist}", str(PKG)]
        if os.environ.get("DEPOT_PROJECT_ID"):
            cmd[2:2] = ["--project", os.environ["DEPOT_PROJECT_ID"]]
    else:  # the forge job already runs in a Depot container: build here
        cmd = [exe, "build", "--path", str(PKG / "pixi.toml"), "--output-dir", str(dist)]
    say("$ " + " ".join(cmd))
    log = PKG / "build.log"
    with open(log, "wb") as f:
        code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, check=False).returncode
    lines = log.read_text("utf-8", "replace").splitlines()
    say("\n".join(lines[-80:]))  # the job keeps only the tail of its output
    if code != 0:
        sys.exit(f"build failed (exit {code})")

    files = sorted(dist.glob("*.conda"))
    if not files:
        sys.exit(f"the build produced no .conda in {dist}")
    say("Built: " + ", ".join(f"{p.name} ({p.stat().st_size} B)" for p in files))
    env = {**os.environ, "PREFIX_API_KEY": os.environ["PREFIX_TOKEN"]}
    up = [exe, "upload", "prefix", "--channel", CHANNEL, "--skip-existing", *map(str, files)]
    subprocess.run(up, env=env, check=True)
    say(f"Done: https://prefix.dev/channels/{CHANNEL}/packages/{NAME}")


if __name__ == "__main__":
    main()
