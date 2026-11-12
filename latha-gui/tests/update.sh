#!/usr/bin/env bash
# App updates (host/src/update.rs) end to end, headless: an app with version= and update= in its latha:app
# section, a manifest served over http on localhost, `latha-run --update` downloads the newer build, and the
# loaders' resolve picks it (checked with `latha-gui --app-info`, which runs resolve like a launch, when built).
#   LATHA_RUN=host/target/release/latha-run bash latha-gui/tests/update.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
run=${LATHA_RUN:-$here/../host/target/release/latha-run}
set_info="python3 $here/../tools/set-app-info.py"
t=$(mktemp -d)
trap 'kill $srv 2>/dev/null || true; rm -rf "$t"' EXIT
export LATHA_GUI_UPDATE_DIR=$t/store no_proxy=127.0.0.1,localhost NO_PROXY=127.0.0.1,localhost
port=$((20000 + RANDOM % 20000))
mkdir -p "$t/www"
(cd "$t/www" && exec python3 -m http.server -b 127.0.0.1 "$port" > /dev/null 2>&1) &
srv=$!
url=http://127.0.0.1:$port
for _ in $(seq 50); do curl -fs "$url/" > /dev/null && break; sleep 0.1; done

printf '\0asm\1\0\0\0' > "$t/empty.wasm"
$set_info "$t/empty.wasm" "$t/app.wasm" id=update-test name=Update version=1.2.9 update=$url/latest.txt
$set_info "$t/empty.wasm" "$t/www/app-1.10.wasm" id=update-test name=Update version=1.10 update=$url/latest.txt
sha() { sha256sum "$1" | cut -d' ' -f1; }
fail() { echo "FAIL: $*"; exit 1; }

# Up to date: the manifest names the same version.
printf 'version=1.2.9\nurl=app.wasm\nsha256=%s\n' "$(sha "$t/app.wasm")" > "$t/www/latest.txt"
"$run" --update "$t/app.wasm" | grep -q "up to date" || fail "1.2.9 should be up to date"

# A wrong SHA-256 is refused and leaves nothing behind.
printf 'version=1.10\nurl=app-1.10.wasm\nsha256=%064d\n' 0 > "$t/www/latest.txt"
if "$run" --update "$t/app.wasm" 2> "$t/err"; then fail "a wrong sha256 was taken"; fi
grep -q SHA-256 "$t/err" || fail "no SHA-256 error: $(cat "$t/err")"
[ ! -e "$t/store/update-test/current" ] || fail "current written for a bad download"

# Another app's build is refused.
$set_info "$t/empty.wasm" "$t/www/other.wasm" id=someone-else version=1.10
printf 'version=1.10\nurl=other.wasm\nsha256=%s\n' "$(sha "$t/www/other.wasm")" > "$t/www/latest.txt"
if "$run" --update "$t/app.wasm" 2> "$t/err"; then fail "another app's build was taken"; fi

# The newer build, zstd-packed: downloaded, unpacked, recorded.
zstd -q -19 "$t/www/app-1.10.wasm" -o "$t/www/app-1.10.wasm.zst"
printf 'version=1.10\nurl=app-1.10.wasm.zst\nsha256=%s\n' "$(sha "$t/www/app-1.10.wasm")" > "$t/www/latest.txt"
"$run" --update "$t/app.wasm" | tee "$t/out" | grep -q "downloaded 1.10" || fail "no download: $(cat "$t/out")"
cmp "$t/store/update-test/1.10.wasm" "$t/www/app-1.10.wasm" || fail "downloaded file differs"
[ "$(head -1 "$t/store/update-test/current")" = 1.10 ] || fail "current is not 1.10"

# Next check starts from the downloaded 1.10: up to date.
"$run" --update "$t/app.wasm" | grep -q "up to date" || fail "1.10 should be up to date"

# A newer package than the download wins: resolve keeps the given file (latha-gui --app-info shows which).
$set_info "$t/empty.wasm" "$t/pkg.wasm" id=update-test version=2.0
gui=${LATHA_GUI:-$here/../build/latha-gui}
if [ -x "$gui" ]; then
  QT_QPA_PLATFORM=offscreen "$gui" --app-info "$t/app.wasm" | grep -q "version: 1.10" || fail "latha-gui did not run the update"
  QT_QPA_PLATFORM=offscreen "$gui" --app-info "$t/pkg.wasm" | grep -q "version: 2.0" || fail "latha-gui ran an older update over a newer file"
  LATHA_GUI_UPDATE=0 QT_QPA_PLATFORM=offscreen "$gui" --app-info "$t/app.wasm" | grep -q "version: 1.2.9" || fail "LATHA_GUI_UPDATE=0 still ran the update"
else
  echo "(no $gui: resolve not checked through latha-gui)"
fi
echo "update test passed"
