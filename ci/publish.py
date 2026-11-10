"""fileapi forge job: build the conda packages in recipes/ with
rattler-build and publish them to prefix.dev/nandi-testing.

Packages, in build order (later ones may depend on earlier ones):
  wasi-sdk, wit-bindgen, wasm-tools, wasi-preview1-adapter, wasmtime  repackaged upstream releases
  jco                                                                 @bytecodealliance/jco from npm on conda-forge nodejs
  perry                                                               @perryts/perry (TypeScript -> native, GTK4 UI, wasmi) from npm
  qt6-wasi                                                            qtbase 6.8.3 for wasm32-wasip1 (~30+ min)

For each recipe: if nandi-testing already has that name + version + build
number, skip it (bump `version`, or the build number for a rebuild of the same
version, to publish again), so a push that only touches qt2wasm never rebuilds
Qt. Otherwise build it with
`rattler-build build -c https://prefix.dev/nandi-testing -c conda-forge` and
upload it right away, so a later failure doesn't lose the earlier packages.

Incremental builds: when the Depot CLI is on PATH (the forge installs it for a
job whose `run` mentions depot), each recipe is built by `depot build` of
ci/build.Dockerfile, whose BuildKit cache mounts Depot keeps between runs: the
rattler package cache, the source cache (the qtbase tarball) and ccache, which
the qt6-wasi build uses. A Qt rebuild then mostly hits ccache. Without depot it
builds in place, uncached.

  python3 ci/publish.py [recipe ...]   # default: all, in order
  python3 ci/publish.py --status       # which recipes are already published (no token needed)

Secret: PREFIX_TOKEN, a prefix.dev API key with write access to nandi-testing.
"""

from __future__ import annotations

import json
import shutil
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECIPES = HERE.parent / "recipes"
ORDER = ["wasi-sdk", "wit-bindgen", "wasm-tools", "wasi-preview1-adapter", "wasmtime", "jco", "perry", "qt6-wasi"]
CHANNEL = "nandi-testing"
CHANNEL_URL = f"https://prefix.dev/{CHANNEL}"
UA = {"User-Agent": "qt2wasm-ci/1"}
RATTLER_BUILD = "https://github.com/prefix-dev/rattler-build/releases/download/v0.80.0/rattler-build-x86_64-unknown-linux-musl"


def say(*a):
    print(*a, flush=True)


def meta(name: str) -> tuple[str, str, int, str]:
    """(package name, version, build number, subdir) from a recipe.yaml."""
    text = (RECIPES / name / "recipe.yaml").read_text()
    ctx = dict(re.findall(r'^  (\w+):\s*"?([^"\n]+?)"?\s*$', text.split("package:")[0], re.MULTILINE))
    pkg = re.search(r"^package:\n  name:\s*(\S+)\n  version:\s*(.+)$", text, re.MULTILINE)
    if not pkg:
        sys.exit(f"{name}: no package name/version in recipe.yaml")
    version = re.sub(r"\$\{\{\s*(\w+)\s*\}\}", lambda m: ctx[m.group(1)], pkg.group(2).strip().strip('"'))
    number = re.search(r"^  number:\s*(\d+)", text, re.MULTILINE)
    subdir = "noarch" if re.search(r"^  noarch:", text, re.MULTILINE) else "linux-64"
    return pkg.group(1), version, int(number.group(1)) if number else 0, subdir


def published(name: str, version: str, number: int, subdir: str) -> bool:
    url = f"{CHANNEL_URL}/{subdir}/repodata.json"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
            data = json.load(r)
    except (OSError, ValueError) as e:  # missing/unreachable index: build anyway
        say(f"could not read {url} ({e}); building")
        return False
    pkgs = {**data.get("packages", {}), **data.get("packages.conda", {})}
    return any(p.get("name") == name and p.get("version") == version and p.get("build_number", 0) == number
               for p in pkgs.values())


def rattler_build() -> str:
    exe = Path(tempfile.mkdtemp()) / "rattler-build"
    with urllib.request.urlopen(urllib.request.Request(RATTLER_BUILD, headers=UA), timeout=300) as r:
        exe.write_bytes(r.read())
    exe.chmod(0o755)
    return str(exe)


def build_cmd(exe: str, recipe: str, out: Path) -> list[str]:
    if shutil.which("depot"):  # Depot BuildKit with persistent cache mounts (ci/build.Dockerfile)
        cmd = ["depot", "build", "--progress", "plain", "--platform", "linux/amd64",
               "-f", str(HERE / "build.Dockerfile"), "--build-arg", f"RECIPE={recipe}",
               "--build-arg", f"RATTLER_BUILD_URL={RATTLER_BUILD}",
               "--output", f"type=local,dest={out / recipe}", str(HERE.parent)]
        if os.environ.get("DEPOT_PROJECT_ID"):
            cmd[2:2] = ["--project", os.environ["DEPOT_PROJECT_ID"]]
        return cmd
    return [exe, "build", "--recipe", str(RECIPES / recipe), "--output-dir", str(out),
            "-c", CHANNEL_URL, "-c", "conda-forge", "--log-style", "plain", "--color", "never"]


def build(exe: str, recipe: str, out: Path) -> list[Path]:
    before = set(out.rglob("*.conda"))
    cmd = build_cmd(exe, recipe, out)
    say("$ " + " ".join(cmd))
    log = out / f"{recipe}.log"
    t0 = time.time()
    with open(log, "wb") as f:
        p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        while p.poll() is None:  # a heartbeat so a long Qt build shows progress
            time.sleep(60)
            tail = log.read_text("utf-8", "replace").strip().splitlines()[-1:] or [""]
            say(f"  [{int(time.time() - t0) // 60} min] {tail[0][:160]}")
    lines = log.read_text("utf-8", "replace").splitlines()
    say("\n".join(lines[-60:]))  # the job keeps only the tail of its output
    stats = [ln for ln in lines if "Hits:" in ln or "Misses:" in ln or "Cacheable calls" in ln]
    if stats:
        say("ccache: " + " | ".join(ln.strip() for ln in stats[-4:]))
    if p.returncode != 0:
        sys.exit(f"{recipe}: build failed (exit {p.returncode})")
    return sorted(set(out.rglob("*.conda")) - before)


def status(wanted: list[str]) -> None:
    for recipe in wanted:
        name, version, number, subdir = meta(recipe)
        state = "published" if published(name, version, number, subdir) else "NOT published (CI will build it)"
        say(f"{name:24} {version:10} build {number}  {state}")


def main() -> None:
    args = sys.argv[1:]
    if "--status" in args:
        status([a for a in args if a != "--status"] or ORDER)
        return
    if not os.environ.get("PREFIX_TOKEN"):
        sys.exit("PREFIX_TOKEN secret is not set (a user or repo secret in fileapi)")
    wanted = args or ORDER
    todo = []
    for recipe in wanted:
        name, version, number, subdir = meta(recipe)
        if published(name, version, number, subdir):
            say(f"{name} {version} (build {number}) is already on {CHANNEL}; skipping")
        else:
            todo.append(recipe)
    if not todo:
        say("Nothing to publish.")
        return

    exe = rattler_build()
    out = HERE.parent / "out"
    out.mkdir(exist_ok=True)
    env = {**os.environ, "PREFIX_API_KEY": os.environ["PREFIX_TOKEN"]}
    for recipe in todo:
        files = build(exe, recipe, out)
        if not files:
            sys.exit(f"{recipe}: the build produced no .conda")
        say("Built: " + ", ".join(f"{p.name} ({p.stat().st_size} B)" for p in files))
        subprocess.run([exe, "upload", "prefix", "--channel", CHANNEL, "--skip-existing", *map(str, files)],
                       env=env, check=True)
        say(f"Published {recipe}: https://prefix.dev/channels/{CHANNEL}/packages/{meta(recipe)[0]}")


if __name__ == "__main__":
    main()
