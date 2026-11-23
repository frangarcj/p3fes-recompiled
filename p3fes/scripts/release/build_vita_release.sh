#!/usr/bin/env bash
# Builds the PS Vita VPK from the user's own SLUS_216.21 and stages it in dist/ for a GitHub prerelease.
# One script for CI (.github/workflows/release-vita.yml) and for local runs.
#
#   p3fes/scripts/release/build_vita_release.sh --elf /path/SLUS_216.21 --version v0.1.0 [options]
#
# Steps: check the ELF's SHA-1 -> build the host recompiler -> recompile into a temporary directory -> install the
# generated sources into THIS source tree (ps2xRuntime/src/runner, ps2xRuntime/include; both gitignored) -> configure
# and build the Vita VPK -> copy it to dist/ with SHA256SUMS and the rendered release notes -> whitelist check of dist/.
#
# The ELF is copied only into the temporary work directory, which is deleted on exit. Nothing derived from the game is
# written anywhere else than the gitignored generated-source paths above and the VPK itself.
#
# Options (environment variable in brackets):
#   --elf FILE            the game executable, SHA-1 must be 3929cd7c... [P3FES_ELF]
#   --version TAG         vMAJOR.MINOR[.PATCH][-suffix]; the Vita APP_VER becomes "%02d.%02d" of MAJOR.MINOR
#                         (v0.1.0 -> 00.01, v1.12.3-beta.2 -> 01.12). Default v0.0.0-dev [P3FES_VERSION]
#   --vitasdk DIR         VitaSDK root, default $VITASDK or /usr/local/vitasdk [VITASDK]
#   --vitagl-dir DIR      vitaGL source tree with libvitaGL.a already built (passed as PS2X_VITA_VITAGL_DIR).
#                         Default: the copy installed in the VitaSDK [P3FES_VITAGL_DIR]
#   --out DIR             output directory, default dist/ in the repository root [P3FES_DIST]
#   --work DIR            scratch directory (host build, Vita build, recompiler output) [P3FES_WORK]
#   --jobs N              parallel jobs, default: number of CPUs [P3FES_JOBS]
#   --commit SHA          commit shown in the release notes, default: git HEAD [P3FES_COMMIT]
#   --notes FILE          release notes template, default p3fes/release/vita-release-notes.md
#   --repo OWNER/NAME     GitHub repository that publishes the release; the notes link to its Pages site (the data
#                         tool). Default frangarcj/p3fes-recompiled [GITHUB_REPOSITORY]
#   --overwrite-generated allow replacing a non-empty ps2xRuntime/src/runner (always allowed when CI=true)
#   --clean-generated     delete the generated sources from the tree when finished
#   --configure-only      stop after the Vita CMake configure step (no build, no packaging)
#   --package-only VPK    skip everything up to packaging: stage an existing VPK (no ELF needed; dry-run of dist/)
set -Eeuo pipefail

EXPECTED_ELF_SHA1=3929cd7c02be944f25ec6b924e5f1eab9bc5e9cb
TITLE_ID=P3FES0001

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

elf=${P3FES_ELF:-}
version=${P3FES_VERSION:-v0.0.0-dev}
vitasdk=${VITASDK:-/usr/local/vitasdk}
vitagl_dir=${P3FES_VITAGL_DIR:-}
out=${P3FES_DIST:-$root/dist}
work=${P3FES_WORK:-}
jobs=${P3FES_JOBS:-}
commit=${P3FES_COMMIT:-}
notes_template=$root/p3fes/release/vita-release-notes.md
repo=${GITHUB_REPOSITORY:-frangarcj/p3fes-recompiled}
overwrite_generated=0
clean_generated=0
configure_only=0
package_only=
[ "${CI:-}" = "true" ] && overwrite_generated=1

log() { printf '[release] %s\n' "$*"; }
die() { printf '[release] ERROR: %s\n' "$*" >&2; exit 1; }
trap 'printf "[release] ERROR: failed at line %s (exit %s)\n" "$LINENO" "$?" >&2' ERR

usage() { sed -n '2,/^set -Eeuo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case $1 in
        --elf) elf=${2:?--elf needs a value}; shift 2 ;;
        --version) version=${2:?--version needs a value}; shift 2 ;;
        --vitasdk) vitasdk=${2:?--vitasdk needs a value}; shift 2 ;;
        --vitagl-dir) vitagl_dir=${2:?--vitagl-dir needs a value}; shift 2 ;;
        --out) out=${2:?--out needs a value}; shift 2 ;;
        --work) work=${2:?--work needs a value}; shift 2 ;;
        --jobs) jobs=${2:?--jobs needs a value}; shift 2 ;;
        --commit) commit=${2:?--commit needs a value}; shift 2 ;;
        --notes) notes_template=${2:?--notes needs a value}; shift 2 ;;
        --repo) repo=${2:?--repo needs a value}; shift 2 ;;
        --overwrite-generated) overwrite_generated=1; shift ;;
        --clean-generated) clean_generated=1; shift ;;
        --configure-only) configure_only=1; shift ;;
        --package-only) package_only=${2:?--package-only needs a VPK}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done

# ---------------------------------------------------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------------------------------------------------
sha1_of() {
    if command -v sha1sum >/dev/null 2>&1; then sha1sum "$1" | cut -d' ' -f1; else shasum -a 1 "$1" | cut -d' ' -f1; fi
}
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
ncpu() { nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2; }

# vMAJOR.MINOR[.PATCH][-suffix] -> "%02d.%02d" (the Vita's APP_VER is exactly XX.YY).
map_app_version() {
    local t=${1#v} maj min
    [[ $t =~ ^([0-9]+)\.([0-9]+)(\.[0-9]+)?([-+][0-9A-Za-z.+-]+)?$ ]] \
        || die "version '$1' is not vMAJOR.MINOR[.PATCH][-suffix]"
    maj=$((10#${BASH_REMATCH[1]})); min=$((10#${BASH_REMATCH[2]}))
    [ "$maj" -le 99 ] && [ "$min" -le 99 ] || die "version '$1': MAJOR and MINOR must be <= 99 for the Vita's XX.YY"
    printf '%02d.%02d' "$maj" "$min"
}

need_tool() { command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"; }

# Whitelist of what may leave this script: the VPK, SHA256SUMS and the release notes. Nothing else, no sub-directories,
# no symlinks; SHA256SUMS must list exactly the VPK with its real hash.
check_dist() {
    local dir=$1 vpk_name=$2 f name n=0
    [ -d "$dir" ] || die "whitelist: $dir does not exist"
    while IFS= read -r -d '' f; do
        name=${f#"$dir"/}
        n=$((n + 1))
        case $name in
            "$vpk_name"|SHA256SUMS|RELEASE_NOTES.md) ;;
            *) die "whitelist: unexpected entry in $dir: $name" ;;
        esac
        [ -f "$f" ] && [ ! -L "$f" ] || die "whitelist: $name is not a regular file"
    done < <(find "$dir" -mindepth 1 -print0)
    [ "$n" -eq 3 ] || die "whitelist: expected exactly 3 files in $dir, found $n"
    [ "$(cat "$dir/SHA256SUMS")" = "$(sha256_of "$dir/$vpk_name")  $vpk_name" ] \
        || die "whitelist: SHA256SUMS does not match $vpk_name"
    local size
    size=$(wc -c < "$dir/$vpk_name" | tr -d " ")
    [ "$size" -gt 1048576 ] || die "whitelist: $vpk_name is suspiciously small ($size bytes)"
    [ "$size" -lt 2000000000 ] || die "whitelist: $vpk_name is larger than GitHub's 2 GB asset limit"
    [ -s "$dir/RELEASE_NOTES.md" ] || die "whitelist: RELEASE_NOTES.md is empty"
    ! grep -q '@[A-Z_]*@' "$dir/RELEASE_NOTES.md" || die "whitelist: RELEASE_NOTES.md still has unresolved @PLACEHOLDERS@"
}

# A VPK from this project holds exactly eboot.bin and sce_sys/param.sfo (game data is never packaged).
check_vpk() {
    local vpk=$1 want_ver=$2 entries
    need_tool unzip
    entries=$(unzip -Z1 "$vpk" | grep -v '/$' | LC_ALL=C sort | tr '\n' ' ' || true)
    [ "$entries" = "eboot.bin sce_sys/param.sfo " ] \
        || die "unexpected VPK contents: $entries(expected: eboot.bin sce_sys/param.sfo)"
    # process substitution: a pipe into grep -q would trip pipefail when grep exits early
    grep -aq "$TITLE_ID" < <(unzip -p "$vpk" sce_sys/param.sfo) || die "param.sfo does not carry title id $TITLE_ID"
    grep -aq "$want_ver" < <(unzip -p "$vpk" sce_sys/param.sfo) || die "param.sfo does not carry APP_VER $want_ver"
}

render_notes() { # template tag app_ver commit vpk_name vpk_sha256 -> stdout
    local tpl=$1 tag=$2 appver=$3 cmt=$4 name=$5 sum=$6 owner tool_url
    [ -f "$tpl" ] || die "release notes template not found: $tpl"
    [[ $repo =~ ^[A-Za-z0-9-]+/[A-Za-z0-9._-]+$ ]] || die "--repo must be OWNER/NAME, got: $repo"
    owner=$(printf '%s' "${repo%%/*}" | tr '[:upper:]' '[:lower:]')
    tool_url="https://$owner.github.io/${repo#*/}/"
    sed -e "s|@VERSION@|$tag|g" -e "s|@APP_VER@|$appver|g" -e "s|@COMMIT@|$cmt|g" -e "s|@TOOL_URL@|$tool_url|g" \
        -e "s|@VPK_NAME@|$name|g" -e "s|@VPK_SHA256@|$sum|g" -e "s|@DATE@|$(date -u +%Y-%m-%d)|g" "$tpl"
}

stage_dist() { # vpk tag app_ver
    local vpk=$1 tag=$2 appver=$3 vpk_name sum
    vpk_name="P3FES-Vita-$tag.vpk"
    rm -rf "$out"
    mkdir -p "$out"
    cp "$vpk" "$out/$vpk_name"
    sum=$(sha256_of "$out/$vpk_name")
    printf '%s  %s\n' "$sum" "$vpk_name" > "$out/SHA256SUMS"
    render_notes "$notes_template" "$tag" "$appver" "$commit" "$vpk_name" "$sum" > "$out/RELEASE_NOTES.md"
    check_dist "$out" "$vpk_name"
    log "staged $out: $(find "$out" -type f | wc -l | tr -d ' ') files (whitelist ok)"
    ( cd "$out" && ls -l )
}

# ---------------------------------------------------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------------------------------------------------
[[ $version =~ ^v?[0-9A-Za-z._+-]+$ ]] || die "version contains unexpected characters"
case $version in v*) tag=$version ;; *) tag=v$version ;; esac
app_ver=$(map_app_version "$tag")
if [ -z "$commit" ]; then commit=$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null || echo unknown); fi
[[ $commit =~ ^[0-9A-Za-z]+$ ]] || die "commit contains unexpected characters"
[ -n "$jobs" ] || jobs=$(ncpu)
log "tag $tag -> Vita APP_VER $app_ver, commit $commit, $jobs jobs"

if [ -n "$package_only" ]; then
    [ -f "$package_only" ] || die "no such VPK: $package_only"
    log "package-only: staging $package_only"
    check_vpk "$package_only" "$app_ver"
    stage_dist "$package_only" "$tag" "$app_ver"
    exit 0
fi

[ -n "$elf" ] || die "no ELF: pass --elf FILE (or P3FES_ELF)"
[ -f "$elf" ] || die "ELF not found: $elf"
got_sha1=$(sha1_of "$elf")
if [ "$got_sha1" != "$EXPECTED_ELF_SHA1" ]; then
    die "ELF SHA-1 is $got_sha1, expected $EXPECTED_ELF_SHA1 (needs the USA SLUS-21621 executable); refusing to run"
fi
log "ELF SHA-1 verified"

for t in cmake ninja unzip; do need_tool "$t"; done
[ -f "$root/p3fes/p3fes.toml" ] && [ -f "$root/p3fes/p3fes_map.csv" ] || die "p3fes/p3fes.toml or p3fes_map.csv missing in $root"

[ -x "$vitasdk/bin/arm-vita-eabi-gcc" ] || die "VitaSDK not found at $vitasdk (set VITASDK or --vitasdk)"
toolchain=$vitasdk/share/vita.toolchain.cmake
[ -f "$toolchain" ] || die "missing $toolchain"
lib_dir=$vitasdk/arm-vita-eabi/lib
vitagl_archive=$lib_dir/libvitaGL.a
if [ -n "$vitagl_dir" ]; then
    vitagl_archive=$vitagl_dir/libvitaGL.a
    [ -f "$vitagl_dir/source/vitaGL.h" ] || die "$vitagl_dir is not a vitaGL source tree (source/vitaGL.h missing)"
fi
[ -f "$vitagl_archive" ] || die "libvitaGL.a not found at $vitagl_archive"
for l in libSDL2.a libvitashark.a libSceShaccCgExt.a libmathneon.a libtaihen_stub.a libSceShaccCg_stub.a; do
    [ -f "$lib_dir/$l" ] || die "VitaSDK is missing $l (install the sdl2, vitaShaRK, SceShaccCgExt, libmathneon and taihen packages)"
done
# The renderer needs the current vitaShaRK (the stock vdpm package predates these entry points).
shark_syms=$("$vitasdk/bin/arm-vita-eabi-nm" -g --defined-only "$lib_dir/libvitashark.a" 2>/dev/null || true)
grep -qw shark_set_shader_association_path <<<"$shark_syms" \
    || die "libvitashark.a lacks shark_set_shader_association_path: build vitaShaRK from upstream master (df24065), not the vdpm package"

if [ -z "$work" ]; then work=${RUNNER_TEMP:-${TMPDIR:-/tmp}}/p3fes-release-work; fi
mkdir -p "$work"
work=$(cd "$work" && pwd)
host_build=$work/host
vita_build=$work/vita
recomp_dir=$work/recomp
log "work directory: $work"

cleanup() {
    rm -rf "$recomp_dir"
    if [ "$clean_generated" = 1 ]; then
        log "removing generated sources from the tree"
        find "$root/ps2xRuntime/src/runner" -mindepth 1 -delete 2>/dev/null || true
        rm -f "$root/ps2xRuntime/include/ps2_recompiled_functions.h" "$root/ps2xRuntime/include/ps2_recompiled_stubs.h"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------------------------------------------------
# 1. Host recompiler (no VitaSDK in the environment, or the top-level CMakeLists would pick the Vita toolchain)
# ---------------------------------------------------------------------------------------------------------------------
log "building the host recompiler"
env -u VITASDK cmake -S "$root" -B "$host_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS2X_BUILD_RECOMP=ON -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF >"$work/host-configure.log" 2>&1 \
    || { tail -n 20 "$work/host-configure.log" >&2; die "host recompiler configure failed (log: $work/host-configure.log)"; }
env -u VITASDK cmake --build "$host_build" --target ps2_recomp --parallel "$jobs" >"$work/host-build.log" 2>&1 \
    || { tail -n 30 "$work/host-build.log" >&2; die "host recompiler build failed (log: $work/host-build.log)"; }
recomp_bin=$(find "$host_build" -type f -name ps2_recomp -perm -u+x | head -n 1)
[ -n "$recomp_bin" ] || die "ps2_recomp binary not found under $host_build"

# ---------------------------------------------------------------------------------------------------------------------
# 2. Recompile the ELF into a temporary directory (the config reads SLUS_216.21 and p3fes_map.csv next to it)
# ---------------------------------------------------------------------------------------------------------------------
log "recompiling the ELF"
rm -rf "$recomp_dir"
mkdir -p "$recomp_dir"
cp "$root/p3fes/p3fes.toml" "$root/p3fes/p3fes_map.csv" "$recomp_dir/"
cp "$elf" "$recomp_dir/SLUS_216.21"
( cd "$recomp_dir" && "$recomp_bin" p3fes.toml ) >"$work/recompile.log" 2>&1 \
    || { tail -n 5 "$work/recompile.log" >&2; die "recompilation failed (log: $work/recompile.log)"; }
rm -f "$recomp_dir/SLUS_216.21"
gen=$recomp_dir/output
for h in ps2_recompiled_functions.h ps2_recompiled_stubs.h register_functions.cpp; do
    [ -s "$gen/$h" ] || die "recompiler did not produce $h"
done
n_cpp=$(find "$gen" -maxdepth 1 -name '*.cpp' | wc -l | tr -d ' ')
[ "$n_cpp" -gt 10000 ] || die "recompiler produced only $n_cpp .cpp files (expected ~14600)"
log "recompiler produced $n_cpp .cpp files"

# ---------------------------------------------------------------------------------------------------------------------
# 3. Install the generated files into this tree (both locations are gitignored)
# ---------------------------------------------------------------------------------------------------------------------
runner=$root/ps2xRuntime/src/runner
incdir=$root/ps2xRuntime/include
if [ -n "$(find "$runner" -mindepth 1 -print -quit 2>/dev/null)" ] && [ "$overwrite_generated" != 1 ]; then
    die "$runner is not empty; pass --overwrite-generated to replace the generated sources"
fi
log "installing generated sources into $runner"
mkdir -p "$runner" "$incdir"
find "$runner" -mindepth 1 -delete
find "$gen" -maxdepth 1 -name '*.cpp' -exec mv {} "$runner/" \;
mv "$gen/ps2_recompiled_functions.h" "$gen/ps2_recompiled_stubs.h" "$incdir/"
rm -rf "$recomp_dir"

# ---------------------------------------------------------------------------------------------------------------------
# 4. Configure and build the Vita VPK
# ---------------------------------------------------------------------------------------------------------------------
export VITASDK=$vitasdk
cmake_args=(
    -S "$root" -B "$vita_build" -G Ninja
    -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    -DCMAKE_BUILD_TYPE=Release
    -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF
    -DPS2X_ENABLE_FFMPEG=OFF
    -DPS2X_ENABLE_LTO=OFF
    -DPS2X_ENABLE_DEBUG_UI=OFF
    -DPS2X_ENABLE_RUNTIME_LOGS=OFF
    -DPS2X_VITA_LIBPERF=OFF
    -DPS2X_VITA_SDL2_SHIM=ON
    -DPS2X_VITA_CREATE_PACKAGE=ON
    -DPS2X_VITA_TITLEID="$TITLE_ID"
    -DPS2X_VITA_VERSION="$app_ver"
    -DPS2X_ENABLE_SCCACHE=OFF
)
[ -z "$vitagl_dir" ] || cmake_args+=(-DPS2X_VITA_VITAGL_DIR="$vitagl_dir")
if command -v ccache >/dev/null 2>&1 && [ "${P3FES_NO_CCACHE:-0}" != 1 ]; then
    log "using ccache"
    cmake_args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
log "configuring the Vita build"
cmake "${cmake_args[@]}" >"$work/vita-configure.log" 2>&1 \
    || { tail -n 30 "$work/vita-configure.log" >&2; die "Vita configure failed (log: $work/vita-configure.log)"; }
if [ "$configure_only" = 1 ]; then
    log "configure-only: stopping before the build"
    exit 0
fi

# The compiler output goes to a file: warnings and errors in the generated sources print lines of recompiled code,
# which must not end up in a public CI log. Only every 100th progress line is shown.
log "building the VPK (this takes a long time; full log: $work/vita-build.log)"
if ! cmake --build "$vita_build" --target ps2xRuntime/ps2EntryRunner.vpk --parallel "$jobs" 2>&1 \
    | tee "$work/vita-build.log" \
    | awk 'match($0, /^\[[0-9]+\/[0-9]+\]/) { s = substr($0, 2, RLENGTH - 2); split(s, a, "/");
           if (a[1] % 100 == 0 || a[1] == a[2]) { print "[build] " s; fflush() } }'; then
    grep -E 'error|FAILED|undefined reference' "$work/vita-build.log" | cut -c1-200 | tail -n 15 >&2 || true
    die "Vita build failed"
fi
vpk=$vita_build/ps2xRuntime/ps2EntryRunner.vpk
[ -f "$vpk" ] || die "VPK not produced at $vpk"
check_vpk "$vpk" "$app_ver"

# ---------------------------------------------------------------------------------------------------------------------
# 5. dist/
# ---------------------------------------------------------------------------------------------------------------------
stage_dist "$vpk" "$tag" "$app_ver"
log "done"
