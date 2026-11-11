#!/usr/bin/env bash
# remote.sh sync から呼ばれる（ラップトップ側で実行する）。
# 標準入力の tar（Cloud Shell で変更した Azahar のファイルと、その一覧 .sync-manifest）を
# 引数のディレクトリ（ラップトップの work/azahar）に反映する。
# 内容が同じファイルは触らないので、mtime が変わらず無駄な再ビルドが起きない。
set -euo pipefail

DEST="$1"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

tar -C "$STAGE" -xf -
cd "$STAGE"

while IFS= read -r f; do
    [ -n "$f" ] && [ -f "$f" ] || continue
    mkdir -p "$DEST/$(dirname "$f")"
    if ! cmp -s "$f" "$DEST/$f"; then
        cp "$f" "$DEST/$f"
        echo "更新: $f"
    fi
done <.sync-manifest

# Cloud Shell 側で元に戻したファイルは、ラップトップ側でも元に戻す
git -C "$DEST" diff --name-only --ignore-submodules | while IFS= read -r f; do
    if ! grep -qxF "$f" .sync-manifest; then
        git -C "$DEST" checkout -q -- "$f"
        echo "復元: $f"
    fi
done
