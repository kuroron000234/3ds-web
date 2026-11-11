#!/usr/bin/env bash
# Claude Code のステータスラインに 3DS-web のビルド進捗バーを出す。
LOG="${WORK:-/tmp/3ds}/build.log"
cat >/dev/null # ステータスライン用の JSON 入力は使わない

[ -f "$LOG" ] || { echo "3DS-web: ビルドログなし"; exit 0; }

last=$(grep -o '^\[[0-9]*/[0-9]*\]' "$LOG" | tail -1 | tr -d '[]')
[ -n "$last" ] || { echo "3DS-web: 準備中"; exit 0; }

done_n=${last%/*}
total=${last#*/}
fail=$(grep -c '^FAILED' "$LOG")
pct=$((done_n * 100 / total))
width=24
fill=$((done_n * width / total))

bar=$(printf '%*s' "$fill" '' | tr ' ' '#')$(printf '%*s' $((width - fill)) '' | tr ' ' '-')

if pgrep -x ninja >/dev/null; then state="ビルド中"; else state="停止"; fi
echo "3DS-web [$bar] ${pct}% (${done_n}/${total}) 失敗 ${fail} ・ ${state}"
