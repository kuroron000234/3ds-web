#!/usr/bin/env bash
# 並列エージェント用。エージェントごとに Azahar の作業ツリー（Cloud Shell、ブランチ wip/<name>）を持ち、
# ビルドとブラウザテストはラップトップの専用ディレクトリで行う。
#
# 使い方: tools/agent.sh <name> setup [--frontend]  作業ツリーを作る（--frontend はフロントエンドも専用に持つ）
#         tools/agent.sh <name> build               変更をラップトップへ送ってビルド（終わるまで戻らない）
#         tools/agent.sh <name> test [オプション]   ラップトップのヘッドレス Chrome で ROM を起動して結果を返す
#             --graphics OpenGL|Software  --rom <file>  --seconds <n>  --option key=value  --chromebook
#         tools/agent.sh <name> backup              ブランチ wip/<name> をラップトップへ退避する
#         tools/agent.sh <name> paths               作業ツリーなどの場所を表示する
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)" # ~/games/3ds-web
name="${1:?エージェント名が必要}"
cmd="${2:?コマンドが必要}"
shift 2

HOST=laptop
R=3ds-web-build
MAIN=/tmp/3ds/azahar           # 統合ツリー（ブランチ web）
WT=/tmp/3ds/wt/$name           # このエージェントの Azahar 作業ツリー（ブランチ wip/<name>）
HOME_WT=/tmp/3ds/wt-home/$name # フロントエンドを専用に持つときの ~/games/3ds-web の作業ツリー
LOCAL=/tmp/3ds-agents/$name    # 手元に持ち帰る結果（スクリーンショットなど）
BASE_REV=$(sed -n 's/^AZAHAR_REV=//p' "$HERE/bootstrap.sh")

front_tree() { if [ -d "$HOME_WT" ]; then echo "$HOME_WT"; else echo "$HERE"; fi; }

# tar（一覧 .sync-manifest 付き）でファイルを送り、内容が違うものだけ反映する
send_files() { # <手元の基準ディレクトリ> <送り先（ラップトップのホームからの相対）> <ファイル...>
    local src="$1" dest="$2"
    shift 2
    printf '%s\n' "$@" >"$src/.sync-manifest"
    (cd "$src" && tar -cf - .sync-manifest "$@") |
        ssh "$HOST" "bash $R/repo/tools/apply-sync.sh \$HOME/$dest"
    rm -f "$src/.sync-manifest"
}

sync() {
    local files
    # shellcheck disable=SC2207
    files=($(
        git -C "$WT" diff --name-only --diff-filter=d --ignore-submodules "$BASE_REV"
        git -C "$WT" ls-files --others --exclude-standard
    ))
    send_files "$WT" "$R/agents/$name/azahar" "${files[@]}"
    local ft
    ft=$(front_tree)
    send_files "$ft/frontend" "$R/agents/$name/frontend" $(cd "$ft/frontend" && ls)
    send_files "$ft/web" "$R/agents/$name/web" index.html
    if [ -d "$HOME_WT" ]; then
        send_files "$ft/tools" "$R/agents/$name" webtest.mjs
    fi
}

case "$cmd" in
setup)
    if [ ! -d "$WT" ]; then
        mkdir -p "$(dirname "$WT")"
        if git -C "$MAIN" show-ref -q "refs/heads/wip/$name"; then
            git -C "$MAIN" worktree add -q "$WT" "wip/$name"
        else
            git -C "$MAIN" worktree add -q -b "wip/$name" "$WT" web
        fi
    fi
    if [ "${1:-}" = --frontend ] && [ ! -d "$HOME_WT" ]; then
        mkdir -p "$(dirname "$HOME_WT")"
        if git -C "$HERE" show-ref -q "refs/heads/wip/$name"; then
            git -C "$HERE" worktree add -q "$HOME_WT" "wip/$name"
        else
            git -C "$HERE" worktree add -q -b "wip/$name" "$HOME_WT" HEAD
        fi
    fi
    ssh "$HOST" "bash $R/repo/tools/laptop-agent.sh setup $name"
    "$0" "$name" paths
    ;;
build)
    sync
    ssh "$HOST" "bash $R/repo/tools/laptop-agent.sh build $name"
    ;;
test)
    mkdir -p "$LOCAL"
    ssh "$HOST" "bash $R/repo/tools/laptop-agent.sh test $name $(printf '%q ' "$@")" | tee "$LOCAL/result.json"
    if ssh "$HOST" "cat $R/agents/$name/test/shot.png" >"$LOCAL/shot.png" 2>/dev/null && [ -s "$LOCAL/shot.png" ]; then
        echo "スクリーンショット: $LOCAL/shot.png"
    fi
    ;;
backup)
    git -C "$WT" push -q -f "$HOST:$R/work/azahar" "wip/$name:refs/heads/wip/$name"
    if [ -d "$HOME_WT" ]; then
        ssh "$HOST" "[ -d $R/work/home-repo.git ] || git init -q --bare $R/work/home-repo.git"
        git -C "$HOME_WT" push -q -f "$HOST:$R/work/home-repo.git" "wip/$name:refs/heads/wip/$name"
    fi
    echo "退避: wip/$name -> $HOST:$R/work"
    ;;
paths)
    echo "Azahar 作業ツリー: $WT（ブランチ wip/$name、統合ブランチは web）"
    echo "フロントエンド:    $(front_tree)/frontend と $(front_tree)/web/index.html"
    echo "ラップトップ:      ~/$R/agents/$name（ssh $HOST）"
    echo "結果:              $LOCAL"
    ;;
*)
    sed -n '5,12p' "$0"
    exit 2
    ;;
esac
