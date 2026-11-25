#!/usr/bin/env bash
# Build, test and package Recif for Linux: linux-x64 (native) and linux-arm64 (cross-compiled).
#
#   tools/build_linux.sh [x64|arm64|all] [--no-test] [--no-package] [--keep] [--build-root DIR] [--sysroot DIR]
#   tools/build_linux.sh sysroot [--sysroot DIR]       (as root, before an arm64 build: the arm64 root for the cross build)
#   tools/build_linux.sh clean                          (removes everything the script made under /tmp/recif-build)
#
# Run it inside WSL Debian 12 from the repository, which may be on a Windows drive (/mnt/c/...). The sources are
# copied to /tmp/recif-build/repo, where the build trees and the test output go too; only the archives go to the
# repository's dist/releases/. Once the targets are built, the copy, the build trees and the sysroot are removed
# (--keep leaves them). WSL clears /tmp when it idles: make the sysroot and build in one WSL session. The 2.11BSD
# pack, work/disks/unix211-rp06.dsk, must exist (tools/unix211.py, or tools/build_dist.ps1 on Windows).
#
# What it does for each target:
#   x64    configure and build (Release, SDL2 built from source and linked statically), ctest, a start of the panel
#          on SDL's offscreen video driver, and dist/releases/Recif-<version>-linux-x64.tar.gz
#   arm64  the same build with aarch64-linux-gnu-gcc against an arm64 root (a Debian sysroot with the dev packages
#          SDL needs, made by "sysroot"); when qemu-aarch64 is installed ctest and the start go through it;
#          dist/releases/Recif-<version>-linux-arm64.tar.gz
#
# Packages needed (Debian, root): build-essential cmake ninja-build pkg-config rsync libasound2-dev libpulse-dev
# libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxkbcommon-dev libdrm-dev
# libgbm-dev libgl-dev libgles-dev libegl-dev libdbus-1-dev libudev-dev libwayland-dev libdecor-0-dev
# wayland-protocols libsndio-dev (SDL2's build dependencies; at run time SDL loads X11, Wayland, ALSA, PulseAudio and
# the GL libraries when present, so the programs link only the C library); for arm64 also gcc-aarch64-linux-gnu,
# debootstrap and, to run the tests, qemu-user-static.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$here")
version=$(head -n 1 "$root/src/VERSION" | tr -d '[:space:]')
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "src/VERSION must hold MAJOR.MINOR.PATCH, not '$version'" >&2; exit 1; }
pack="$root/work/disks/unix211-rp06.dsk"

targets=()
test=1 package=1 keep=0
build_root="${RECIF_BUILD_ROOT:-/tmp/recif-build}"
sysroot="${RECIF_SYSROOT:-/tmp/recif-build/sysroot-arm64}"
while [[ $# -gt 0 ]]; do
    case $1 in
        x64|arm64|sysroot|clean) targets+=("$1") ;;
        all) targets+=(x64 arm64) ;;
        --no-test) test=0 ;;
        --no-package) package=0 ;;
        --keep) keep=1 ;;
        --build-root) build_root=$2; shift ;;
        --sysroot) sysroot=$2; shift ;;
        -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $1 (tools/build_linux.sh --help)" >&2; exit 2 ;;
    esac
    shift
done
[[ ${#targets[@]} -gt 0 ]] || targets=(x64 arm64)

tree="$build_root/repo"
sync_sources() {
    mkdir -p "$tree"
    rsync -a --delete --exclude /.git --exclude /work --exclude /dist --exclude /build --exclude /docs "$root/" "$tree/"
    mkdir -p "$tree/work"
}

# The arm64 root: Debian 12's own packages, unpacked (not configured: nothing of arm64 runs), so the cross
# compiler finds the C library and the headers SDL's backends need.
sysroot_packages=(libc6-dev linux-libc-dev libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev
    libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl-dev libgles-dev
    libegl-dev libdbus-1-dev libudev-dev libwayland-dev libdecor-0-dev wayland-protocols libsndio-dev)

make_sysroot() {
    [[ $EUID -eq 0 ]] || { echo "making the sysroot needs root: sudo tools/build_linux.sh sysroot --sysroot $sysroot" >&2; exit 1; }
    command -v debootstrap >/dev/null || { echo "debootstrap is not installed (apt-get install debootstrap)" >&2; exit 1; }
    local existing=$sysroot
    while [[ ! -d $existing ]]; do existing=$(dirname "$existing"); done
    local owner=${SUDO_UID:+$SUDO_UID:$SUDO_GID}
    [[ -n $owner ]] || owner=$(stat -c %u:%g "$existing")
    rm -rf "$sysroot"
    mkdir -p "$sysroot"
    local made=$sysroot
    while [[ $made != "$existing" ]]; do chown "$owner" "$made"; made=$(dirname "$made"); done
    debootstrap --foreign --variant=minbase --arch=arm64 --include="$(IFS=,; echo "${sysroot_packages[*]}")" \
        bookworm "$sysroot" http://deb.debian.org/debian
    local deb
    for deb in "$sysroot"/var/cache/apt/archives/*.deb; do dpkg-deb -x "$deb" "$sysroot"; done
    chown -R "$owner" "$sysroot"
    chmod -R u+rX "$sysroot"
    echo "sysroot: $sysroot"
}

# ctest, the version and one start of the panel on the offscreen driver; arm64 through qemu-user.
run_tests() {
    local arch=$1 dir=$2
    ctest --test-dir "$dir" --output-on-failure
    local scratch="$build_root/smoke-$arch"
    rm -rf "$scratch"; mkdir -p "$scratch"
    local env=(env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy)
    local recif=("${env[@]}" "$dir/recif") daf=("${env[@]}" "$dir/daf2020")
    if [[ $arch == arm64 ]]; then
        recif=("${env[@]}" qemu-aarch64 -L "$sysroot" "$dir/recif")
        daf=("${env[@]}" qemu-aarch64 -L "$sysroot" "$dir/daf2020")
    fi
    (cd "$scratch" &&
        [[ $("${recif[@]}" --version) == "$version" ]] &&
        "${recif[@]}" --hidden --frames 3 --screenshot "$scratch/panel.png" > "$scratch/out.txt" 2>&1 &&
        "${daf[@]}" --hidden --demo --frames 3 --screenshot "$scratch/daf.png" >> "$scratch/out.txt" 2>&1 &&
        [[ -s $scratch/panel.png && -s $scratch/daf.png ]]) ||
        { echo "the start of the $arch programs failed" >&2; cat "$scratch/out.txt" >&2 || true; exit 1; }
    echo "start of the $arch programs: ok"
}

# The archive: the programs, the documents and the 2.11BSD pack under Recif-<version>-<platform>/.
package_target() {
    local arch=$1 dir=$2 name="Recif-$version-linux-$arch"
    [[ -f $pack ]] || { echo "the 2.11BSD pack $pack is missing (python3 tools/unix211.py)" >&2; exit 1; }
    local stage="$build_root/stage-$arch/$name"
    rm -rf "$build_root/stage-$arch"
    mkdir -p "$stage/docs" "$stage/disks"
    install -m 755 "$dir/recif" "$dir/daf2020" "$stage/"
    install -m 644 "$root/README.md" "$root/README.ro.md" "$stage/"
    # Without the lines that show the pictures of assets/, which the archive does not hold.
    sed -i -e '/^!\[[^]]*\](assets\/[^)]*)\r\?$/{N;d;}' -e '/^<p align="center">.*assets\/.*<\/p>\r\?$/{N;d;}' \
        "$stage/README.md" "$stage/README.ro.md"
    local doc
    install -m 644 "$root/LICENSE" "$stage/"
    for doc in THIRD_PARTY.md CHANGELOG.md NOTES.md NOTES.ro.md; do install -m 644 "$root/docs/$doc" "$stage/docs/"; done
    install -m 644 "$pack" "$stage/disks/"
    mkdir -p "$root/dist/releases"
    local archive="$root/dist/releases/$name.tar.gz"
    tar -C "$build_root/stage-$arch" --owner=0 --group=0 --sort=name -czf "$archive.tmp" "$name"
    mv -f "$archive.tmp" "$archive"
    find "$root/dist/releases" -maxdepth 1 -name "Recif-*-linux-$arch.tar.gz" ! -name "$name.tar.gz" -delete
    rm -rf "$build_root/stage-$arch"
    echo "archive: dist/releases/$name.tar.gz"
}

build_target() {
    local arch=$1 dir="$build_root/$1"
    mkdir -p "$dir"
    local args=(-S "$tree/src" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release)
    if [[ $arch == arm64 ]]; then
        command -v aarch64-linux-gnu-gcc >/dev/null || { echo "aarch64-linux-gnu-gcc is not installed (apt-get install gcc-aarch64-linux-gnu)" >&2; exit 1; }
        [[ -f $sysroot/usr/include/X11/Xlib.h ]] || { echo "no arm64 sysroot at $sysroot: sudo tools/build_linux.sh sysroot --sysroot $sysroot" >&2; exit 1; }
        cat > "$dir/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_SYSROOT "$sysroot")
set(CMAKE_FIND_ROOT_PATH "$sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
        if [[ $test -eq 1 ]] && command -v qemu-aarch64 >/dev/null; then
            echo "set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64 -L \"$sysroot\")" >> "$dir/toolchain.cmake"
        fi
        args+=(-DCMAKE_TOOLCHAIN_FILE="$dir/toolchain.cmake")
        export PKG_CONFIG_SYSROOT_DIR="$sysroot"
        export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig"
        unset PKG_CONFIG_PATH
    fi
    echo "== linux-$arch: $dir"
    cmake "${args[@]}"
    cmake --build "$dir"
    if [[ $test -eq 1 ]]; then
        if [[ $arch == x64 || -n $(command -v qemu-aarch64 || true) ]]; then
            run_tests "$arch" "$dir"
        else
            echo "qemu-aarch64 is not installed: the arm64 programs are built but not run"
        fi
    fi
    [[ $package -eq 0 ]] || package_target "$arch" "$dir"
}

for t in "${targets[@]}"; do
    case $t in
        sysroot) make_sysroot ;;
        clean) rm -rf "$build_root" "$sysroot"; echo "removed $build_root" ;;
        *) sync_sources; build_target "$t" ;;
    esac
done
# Everything a build made under /tmp goes once it is done (--keep leaves it; a sysroot made alone stays for the
# build after it). As root, "sysroot all" makes the root and builds both targets in one session.
built=0
for t in "${targets[@]}"; do [[ $t != x64 && $t != arm64 ]] || built=1; done
if [[ $keep -eq 0 && $built -eq 1 ]]; then
    rm -rf "$build_root/repo" "$build_root/x64" "$build_root/arm64" "$build_root"/smoke-* "$sysroot"
    rmdir "$build_root" 2>/dev/null || true
fi
