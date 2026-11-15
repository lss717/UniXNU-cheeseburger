#!/usr/bin/env bash
# Device packaging entry point. Board-specific values live in board.toml.
set -euo pipefail
DEVICE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TOOL_BIN=
cleanup(){ [ -z "$TOOL_BIN" ] || rm -rf "$TOOL_BIN"; }
trap cleanup EXIT
PYTHON=${PYTHON:-$(command -v python3 || command -v python || true)}
[ -n "$PYTHON" ] || { echo "python3 or python is required" >&2; exit 1; }
UNIXNU=$("$PYTHON" - "$DEVICE/board.toml" <<'PY'
import sys, tomllib
package = tomllib.load(open(sys.argv[1], 'rb')).get('package', {})
if 'unixnu' not in package:
    sys.exit('board.toml needs [package] unixnu = "<UniXNU checkout>"')
print(package['unixnu'])
PY
)
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

# Apply reviewed compatibility patches to the pinned UniXNU checkout. A reverse
# check skips a patch that is already present, so --check and a full package
# run are idempotent.
for patch in "$DEVICE"/patches/unixnu/*.patch; do
        [ -e "$patch" ] || continue
        if git -C "$ROOT" apply --reverse --check "$patch" >/dev/null 2>&1; then
                continue
        fi
        git -C "$ROOT" apply --check "$patch"
        git -C "$ROOT" apply "$patch"
done

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
"$PYTHON" "$ROOT/unixnu" release -D "$DEVICE" "${ARGS[@]}"
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

"$PYTHON" "$ROOT/unixnu" manifest \
        -D "$DEVICE" \
        --target "${EDK2_TARGET:-RELEASE}"
