#!/usr/bin/env bash
# Device packaging entry point. Board-specific values live in board.toml.
set -euo pipefail
DEVICE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
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

exec "$PYTHON" "$ROOT/unixnu" release -D "$DEVICE" "$@"
