#!/usr/bin/env bash
# ラップトップが使えないとき用。エージェントごとの作業ツリー・ビルド・ブラウザテストを
# Cloud Shell だけで行う（2 コア・メモリ少なめなので、重い処理は flock で 1 つずつ）。
#
# 使い方: tools/local.sh <name> setup [--frontend]  /tmp/3ds/agents/<name> を作る
#         tools/local.sh <name> build               ビルド（順番待ちあり）
#         tools/local.sh <name> test [オプション]   ヘッドレス Chromium（SwiftShader）で ROM を起動
#             --graphics OpenGL|Software  --seconds <n>  --option key=value  --log-lines <n>
#         tools/local.sh <name> backup              ブランチ wip/<name> を ~/3ds-wip/ に退避（再起動対策）
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
name="${1:?エージェント名が必要}"
cmd="${2:?コマンドが必要}"
shift 2

ROOT=/tmp/3ds
A=$ROOT/agents/$name
BK=$HOME/3ds-wip
PW=$HOME/.npm-packages/lib/node_modules/@playwright/mcp
BASE_REV=$(sed -n 's/^AZAHAR_REV=//p' "$HERE/bootstrap.sh")
exec 9>"$ROOT/heavy.lock"

front() { if [ -d "$A/home" ]; then echo "$A/home"; else echo "$HERE"; fi; }

case "$cmd" in
setup)
    if [ ! -d "$A/azahar" ]; then
        mkdir -p "$A/out" "$A/web" "$A/test"
        cp -a "$ROOT/azahar" "$A/azahar"
        ln -sfn "$ROOT/emsdk" "$A/emsdk"
        ln -sfn "$ROOT/deps" "$A/deps"
        if [ -f "$BK/$name.bundle" ]; then
            git -C "$A/azahar" fetch -q "$BK/$name.bundle" "wip/$name:wip/$name"
            git -C "$A/azahar" switch -q "wip/$name"
            echo "退避から復元: wip/$name"
        else
            git -C "$A/azahar" switch -q -c "wip/$name" web
        fi
    fi
    if [ "${1:-}" = --frontend ] && [ ! -d "$A/home" ]; then
        if git -C "$HERE" show-ref -q "refs/heads/wip/$name"; then
            git -C "$HERE" worktree add -q "$A/home" "wip/$name"
        else
            git -C "$HERE" worktree add -q -b "wip/$name" "$A/home" HEAD
        fi
    fi
    echo "Azahar: $A/azahar（ブランチ wip/$name）"
    echo "フロントエンド: $(front)/frontend, $(front)/web/index.html, $(front)/tools/webtest.mjs"
    ;;
build)
    # 土台の全体ビルドが走っている間は待つ（同じファイルを二重にコンパイルしないため。ccache に溜まる）
    while pgrep -x ninja >/dev/null && [ ! -f "$ROOT/build/bin/Release/azahar_web.wasm" ]; do sleep 20; done
    flock 9
    start=$(date +%s)
    status=0
    (cd "$HERE" && WORK="$A" FRONTEND_DIR="$(front)/frontend" OUT_DIR="$A/out" NINJA_JOBS=2 \
        ./bootstrap.sh build >"$A/run.log" 2>&1) || status=$?
    fails=$(grep -c '^FAILED' "$A/build.log" 2>/dev/null || true)
    echo "ビルド: $(($(date +%s) - start)) 秒、失敗 ${fails:-?} 件（終了コード $status）"
    if [ "${fails:-0}" != 0 ] || [ "$status" != 0 ]; then
        grep -E '^FAILED|error:|undefined symbol' "$A/build.log" "$A/run.log" 2>/dev/null |
            sed "s|$A/||g" | head -40
        echo "全文: $A/build.log"
        exit 1
    fi
    ;;
test)
    flock 9
    if [ ! -x "$HOME/.cache/ms-playwright/chromium-1247/chrome-linux64/chrome" ]; then
        (cd "$PW" && PLAYWRIGHT_BROWSERS_PATH=/tmp/pw node node_modules/playwright/cli.js \
            install --no-shell chromium >/dev/null 2>&1)
        ln -sfn /tmp/pw/chromium-1247 /tmp/pw/chromium-1254
    fi
    if [ ! -f "$ROOT/roms/boot.3dsx" ]; then
        d=$(mktemp -d)
        curl -sSL -o "$d/hb.zip" https://github.com/devkitPro/3ds-hbmenu/releases/download/v2.4.4/3ds-hbmenu-v2.4.4.zip
        unzip -q -o "$d/hb.zip" -d "$d/x"
        mkdir -p "$ROOT/roms" && cp "$d/x/boot.3dsx" "$ROOT/roms/"
        rm -rf "$d"
    fi
    cp "$(front)/web/index.html" "$A/web/"
    WEBTEST_ROOT=$ROOT WEBTEST_NODE="$PW" WEBTEST_NO_GPU=1 \
        WEBTEST_CHROME="$HOME/.cache/ms-playwright/chromium-1247/chrome-linux64/chrome" \
        node "$(front)/tools/webtest.mjs" --agent "$name" "$@" | tee "$A/test/result.json"
    echo "スクリーンショット: $A/test/shot.png"
    ;;
backup)
    mkdir -p "$BK"
    git -C "$A/azahar" bundle create -q "$BK/$name.bundle.tmp" "$BASE_REV..wip/$name"
    mv "$BK/$name.bundle.tmp" "$BK/$name.bundle"
    echo "退避: $BK/$name.bundle（$(du -h "$BK/$name.bundle" | cut -f1)）"
    ;;
*)
    sed -n '5,10p' "$0"
    exit 2
    ;;
esac
