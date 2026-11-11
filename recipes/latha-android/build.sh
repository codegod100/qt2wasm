#!/usr/bin/env bash
# rattler-build script for recipes/latha-android: build the bundled app, get
# Rust + the Android NDK/SDK, then `cargo apk build --release`.
set -euxo pipefail
cd "$SRC_DIR"
dl=https://dl.google.com/android/repository
work=$SRC_DIR/.android-build
mkdir -p "$work"

# 1. The app: qt2wasm's sketch example, a latha:gui component.
export CONDA_PREFIX=$BUILD_PREFIX
font=$(ls "$BUILD_PREFIX"/fonts/DejaVuSans*.ttf | head -n 1)
mkdir -p latha-android/assets
# (into its own dir: qt2wasm writes docs and the WIT beside the .wasm)
qt2wasm build qt2wasm/examples/sketch --gui --font "$font" --js none -o "$work/app/sketch.wasm"
cp "$work/app/sketch.wasm" latha-android/assets/app.wasm
printf 'sketch\n' > latha-android/assets/app.args

# 2. Rust with the Android targets, and cargo-apk. Conda's compiler flags are
# for linux-64 and must not reach the NDK's clang (cargo-apk sets CC_<target>).
unset CC CXX CFLAGS CXXFLAGS CPPFLAGS LDFLAGS AR RANLIB || true
export RUSTUP_HOME=$work/rustup CARGO_HOME=$work/cargo
curl -fsSL https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal \
    --default-toolchain "$LATHA_RUST" --target aarch64-linux-android,x86_64-linux-android
export PATH=$CARGO_HOME/bin:$PATH
cargo install cargo-apk --version 0.10.0 --locked

# 3. The SDK parts cargo-apk uses (a platform's android.jar, aapt/zipalign/
# apksigner from build-tools) and the NDK, straight from Google's repository.
export ANDROID_HOME=$work/sdk
mkdir -p "$ANDROID_HOME/platforms" "$ANDROID_HOME/build-tools"
fetch() { curl -fsSL "$dl/$1" -o "$work/$1" && unzip -q "$work/$1" -d "$work/x" && rm "$work/$1"; }
rm -rf "$work/x"; fetch platform-35_r02.zip; mv "$work"/x/android-* "$ANDROID_HOME/platforms/android-35"
rm -rf "$work/x"; fetch build-tools_r35_linux.zip; mv "$work"/x/android-* "$ANDROID_HOME/build-tools/35.0.0"
rm -rf "$work/x"; fetch "android-ndk-$LATHA_NDK-linux.zip"; mv "$work"/x/android-ndk-* "$work/ndk"
export ANDROID_NDK_ROOT=$work/ndk ANDROID_NDK_HOME=$work/ndk

# 4. Sign with a fresh key (an APK must be signed to install). A new key per
# build: uninstall the old latha before installing a newer one.
keytool -genkeypair -keystore "$work/latha.keystore" -storepass android -keypass android \
    -alias latha -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=latha" -noprompt
export CARGO_APK_RELEASE_KEYSTORE=$work/latha.keystore CARGO_APK_RELEASE_KEYSTORE_PASSWORD=android

cd latha-android
cargo apk build --release
apk=target/release/apk/latha.apk
ls -la "$apk"
install -Dm644 "$apk" "$PREFIX/share/latha-android/latha.apk"
install -Dm644 assets/app.wasm "$PREFIX/share/latha-android/app.wasm"
install -Dm644 README.md "$PREFIX/share/latha-android/README.md"
