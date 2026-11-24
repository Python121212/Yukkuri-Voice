#!/bin/sh
# docs/audio/*.wav を現在のエンジンで作り直す。
#
# 以前はこの生成手順がリポジトリに残っておらず、音声を更新するたびに
# 「ファイルの長さから中身を推測する」ことになり、実際には入っていない
# 文（例:「ゆっくりしていってね」の 2 回繰り返し）を書き込んでしまう
# 事故があった。今後はこのスクリプトだけを使う。
#
#   sh scripts/gen-audio.sh
#
# 音声の内容を変えたいときは、下の TABLE を直してから実行する。
set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

OUT=docs/audio
CLI=${CLI:-build/ykcli}

mkdir -p "$OUT" build
if [ ! -x "$CLI" ]; then
    echo "ykcli をビルドします -> $CLI"
    ${CC:-cc} -std=c99 -O2 -Icore/include core/src/*.c tools/cli.c -lm -o "$CLI"
fi

# ファイル名 | 読み上げる内容 | プリセット
TABLE='
sample_yukkuri.wav|ゆっくりしていってね。|yukkuri
sample_reimu.wav|ゆっくりしていってね。|reimu
sample_marisa.wav|ゆっくりしていってね。|marisa
demo_yukkuri.wav|ゆっくりしていってね。ありがとう。|yukkuri
demo_sentence.wav|ゆっくりしていってね。おつかれさま。|yukkuri
vowels_consonants.wav|あ、い、う、え、お、か、き、く、け、こ、さ、し、す、せ、そ、た、ち、つ、て、と、な、に、ぬ、ね、の|yukkuri
'

echo "$TABLE" | while IFS='|' read -r name text preset; do
    [ -n "$name" ] || continue
    "$CLI" "$text" "$OUT/$name" "$preset" 2>&1 | sed "s|^|$name: |"
done

echo "完了: $OUT"
