#!/usr/bin/env bash
# Install the repository's git hooks. Idempotent; run it again after a hook changes.
#
# Hooks live here rather than in .git/hooks because .git/ is not cloned -- a hook nobody
# receives is a check nobody runs. This copies them in, and `setup.sh` calls it.
set -euo pipefail
cd "$(dirname "$0")/.."

hooks_dir="$(git rev-parse --git-path hooks)"
mkdir -p "$hooks_dir"

cat > "$hooks_dir/pre-commit" <<'HOOK'
#!/usr/bin/env bash
# Refuse a commit that made the frame slower. scripts/perfgate.py carries the argument.
set -euo pipefail
root="$(git rev-parse --show-toplevel)"

# **Only when the commit can move the frame.** A doc or script commit pays nothing, which
# is what stops this becoming the hook everyone passes --no-verify to. Staged paths only:
# an unstaged shader edit is not what is being committed.
if ! git diff --cached --name-only | grep -qE '^(engine/gfx/|engine/shaders/|engine/scene/)'; then
    exit 0
fi

if [ ! -x "$root/build/release/demo" ]; then
    echo "pre-commit: build/release/demo is not built, so the frame was not measured." >&2
    echo "            Run ./build_game.sh demo release, or commit with --no-verify." >&2
    exit 1
fi

echo "pre-commit: measuring the frame (engine/gfx, shaders or scene changed)..."
"$root/scripts/perfgate.py" --config release
HOOK

chmod +x "$hooks_dir/pre-commit"
echo "==> installed $hooks_dir/pre-commit"
