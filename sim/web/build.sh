#!/usr/bin/env bash
# Собрать тестер в браузере: clang → WebAssembly (без emscripten и без libc),
# потом вложить .wasm в одну страницу sim/web/out/ilf_tester.html
set -euo pipefail
cd "$(dirname "$0")"
ROOT=../..
OUT=out; mkdir -p $OUT/obj
CFLAGS="--target=wasm32 -O2 -ffreestanding -nostdlib -fno-builtin -Wno-everything -std=gnu11
  -Ilibc -I. -I$ROOT/components/lvgl -I$ROOT/components
  -I$ROOT/components/ilf_soul/include -I$ROOT/components/ilf_ui/include -I$ROOT/components/ilf_ui
  -I$ROOT/components/ilf_herd/include -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE"
SRCS=$(find $ROOT/components/lvgl/src -name '*.c'; \
  echo $ROOT/components/ilf_soul/ilf_soul.c $ROOT/components/ilf_soul/ilf_clock.c; \
  ls $ROOT/components/ilf_ui/*.c $ROOT/components/ilf_ui/fonts/*.c; echo web_main.c libc/libc.c)
OBJS=()
TODO=()
for s in $SRCS; do
  o=$OUT/obj/$(echo "$s" | sed 's#[/.]#_#g').o
  OBJS+=("$o")
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ]; then TODO+=("$s"); fi
done
export CFLAGS OUT
printf '%s\n' "${TODO[@]}" | grep . | xargs -r -P 8 -I{} sh -c 'clang $CFLAGS -c "$1" -o "$OUT/obj/$(echo "$1" | sed "s#[/.]#_#g").o"' _ {} || true
wasm-ld --no-entry --initial-memory=67108864 -z stack-size=1048576 --gc-sections \
  --export=__heap_base -o $OUT/ilf.wasm "${OBJS[@]}"
python3 make_page.py $OUT/ilf.wasm $OUT/ilf_tester.html
ls -la $OUT/ilf.wasm $OUT/ilf_tester.html
