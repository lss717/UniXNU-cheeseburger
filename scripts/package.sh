#!/usr/bin/env bash
# Device packaging entry point. Board-specific values live in board.toml.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
DEVICE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOT=
TOOL_BIN=
ADT_TMP=
cleanup(){
        [ -z "$TOOL_BIN" ] || rm -rf "$TOOL_BIN"
        [ -z "$ADT_TMP" ] || rm -f "$ADT_TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
PYTHON=${PYTHON:-$(command -v python3 || command -v python || true)}
[ -n "$PYTHON" ] || { echo "python3 or python is required" >&2; exit 1; }
read -r UNIXNU UPSTREAM_BASE DOWNSTREAM_REV <<< "$("$PYTHON" - "$DEVICE/board.toml" <<'PY'
import sys, tomllib
package = tomllib.load(open(sys.argv[1], 'rb')).get('package', {})
for key in ('unixnu', 'unixnu_upstream_base', 'unixnu_downstream_revision'):
    if key not in package:
        sys.exit(f'board.toml needs [package] {key}')
print(package['unixnu'], package['unixnu_upstream_base'], package['unixnu_downstream_revision'])
PY
)"
case "$UNIXNU" in /*) ROOT=$UNIXNU ;; *) ROOT=$(cd "$DEVICE/$UNIXNU" && pwd) ;; esac

UEFI=${EDK2_MSM8998:-}
CHECK=false
ARGS=()
while [ $# -gt 0 ]; do
        case "$1" in
                --uefi|--edk2)
                        [ $# -ge 2 ] || { echo "$1 needs a directory" >&2; exit 1; }
                        UEFI=$2
                        shift 2
                        ;;
                --uefi=*|--edk2=*)
                        UEFI=${1#*=}
                        shift
                        ;;
                --check)
                        CHECK=true
                        ARGS+=("$1")
                        shift
                        ;;
                *)
                        ARGS+=("$1")
                        shift
                        ;;
        esac
done

[ -z "${UNIXNU_TRUST_CACHE:-}" ] || ARGS+=(--trust-cache "$UNIXNU_TRUST_CACHE")
[ -z "${UNIXNU_DATA_RAMDISK:-}" ] || ARGS+=(--data-ramdisk "$UNIXNU_DATA_RAMDISK")
[ -z "${UNIXNU_GPU_ADDON:-}" ] || ARGS+=(--gpu-addon "$UNIXNU_GPU_ADDON")
[ "${UNIXNU_SKIP_SETUP:-0}" != 1 ] || ARGS+=(--skip-setup)

# The downstream checkout is an immutable build input. Refuse to build from a
# moving branch, an unexpected commit or a dirty worktree.
UNIXNU_REVISION=$(git -C "$ROOT" rev-parse HEAD)
[ "$UNIXNU_REVISION" = "$DOWNSTREAM_REV" ] || {
        echo "UniXNU checkout is $UNIXNU_REVISION, expected $DOWNSTREAM_REV" >&2
        exit 1
}
[ -z "$(git -C "$ROOT" status --short)" ] || {
        echo "UniXNU checkout is dirty: $ROOT" >&2
        exit 1
}
MERGE_BASE=$(git -C "$ROOT" merge-base HEAD "$UPSTREAM_BASE")
[ "$MERGE_BASE" = "$UPSTREAM_BASE" ] || {
        echo "UniXNU merge-base is $MERGE_BASE, expected $UPSTREAM_BASE" >&2
        exit 1
}

if [ -z "${CLANG_BIN:-}" ] && [ -z "${CLANG38_BIN:-}" ]; then
        if command -v brew >/dev/null 2>&1; then
                LLVM_PREFIX=$(brew --prefix llvm)
                LLD_PREFIX=$(brew --prefix lld)
                export PATH="$LLVM_PREFIX/bin:$LLD_PREFIX/bin:$PATH"
        fi
        TOOL_BIN=$(mktemp -d "${TMPDIR:-/tmp}/unixnu-tools.XXXXXX")
        for tool in clang lld-link llvm-lib llvm-rc llvm-ar llvm-objcopy ld.lld; do
                source=$(command -v "$tool") || {
                        echo "required LLVM tool not found: $tool" >&2
                        exit 1
                }
                ln -s "$source" "$TOOL_BIN/$tool"
        done
        export CLANG_BIN="$TOOL_BIN/"
        export CLANG38_BIN="$TOOL_BIN/"
        export PATH="$TOOL_BIN:$PATH"
fi

if ! "$CHECK" && [ -n "$UEFI" ]; then
        ARGS+=(--external-firmware)
fi
"$PYTHON" "$ROOT/unixnu" release -D "$DEVICE" --out "$ROOT/out" "${ARGS[@]}"
if "$CHECK" || [ -z "$UEFI" ]; then
        exit 0
fi

case "$UEFI" in /*) ;; *) UEFI=$(cd "$DEVICE/$UEFI" && pwd) ;; esac
[ -f "$UEFI/build.sh" ] || { echo "edk2-msm8998 not found: $UEFI" >&2; exit 1; }

read -r NAME OS_PROFILE KIT <<< "$("$PYTHON" - "$DEVICE/board.toml" <<'PY'
import sys, tomllib
board = tomllib.load(open(sys.argv[1], 'rb'))['board']
print(board['name'], board['os_profile'], board['kit'])
PY
)"
OUTPUT=$ROOT/out/"$NAME"/"$OS_PROFILE"/"$KIT"

UNIXNU_TREE="$OUTPUT/tree" bash "$UEFI/build.sh" \
        --device "$NAME" \
        --release "${EDK2_TARGET:-RELEASE}" \
        --skip-rootfs-gen

"$PYTHON" "$DEVICE/scripts/make-bootimg.py" \
        --edk2 "$UEFI" \
        --kernel "$UEFI/workspace/uefi-$NAME.img.gz-dtb" \
        --payload "$OUTPUT/payload/payload.img" \
        --out "$OUTPUT/boot/boot.img"

"$PYTHON" "$DEVICE/scripts/check-cheeseburger.py" \
        --fd "$UEFI/workspace/Build/$NAME/${EDK2_TARGET:-RELEASE}_CLANG38/FV/MSM8998PKG_UEFI.fd" \
        --boot "$OUTPUT/boot/boot.img" \
        --payload "$OUTPUT/payload/payload.img"

ADT_TMP=$(mktemp "${TMPDIR:-/tmp}/cheeseburger-adt.XXXXXX")
mcopy -o -i "$OUTPUT/payload/payload.img" ::/EFI/UniXNU/Payloads/kernel.macho.adt "$ADT_TMP"
"$PYTHON" "$DEVICE/scripts/check-format2-adt.py" --adt "$ADT_TMP"

"$PYTHON" "$ROOT/unixnu" manifest \
        -D "$DEVICE" \
        --out "$ROOT/out" \
        --target "${EDK2_TARGET:-RELEASE}"

"$PYTHON" "$DEVICE/scripts/update-package-manifest.py" \
        "$OUTPUT/package.manifest.json" \
        "$ROOT" \
        "$UPSTREAM_BASE" \
        "$UEFI"
