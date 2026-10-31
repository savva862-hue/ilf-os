#!/usr/bin/env bash
# Шрифты экрана: Nunito (кириллица) + значки FontAwesome из LVGL (кнопки, Bluetooth, звонок).
# Запуск: ./tools/gen_fonts.sh   (нужен node: npx скачает lv_font_conv)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=components/ilf_ui/fonts
TEXT="-r 0x20-0x7E -r 0x401,0x410-0x44F,0x451 -r 0x2014,0xAB,0xBB,0xB7,0x2026,0x2022,0xD7"
FA=components/lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff
SYMS="0xF00C,0xF048,0xF04B,0xF04C,0xF051,0xF053,0xF077,0xF0E7,0xF0F3,0xF293,0xF55A"
conv() { npx -y lv_font_conv --no-compress --bpp 4 --format lvgl --lv-include lvgl.h "$@"; }
for size in 24 32; do
  conv --size $size --font assets/fonts/Nunito-Bold.ttf $TEXT --font $FA -r $SYMS \
       -o $OUT/ilf_font_$size.c --lv-font-name ilf_font_$size
done
conv --size 48 --font assets/fonts/Nunito-ExtraBold.ttf $TEXT -o $OUT/ilf_font_48.c --lv-font-name ilf_font_48
conv --size 120 --font assets/fonts/Nunito-ExtraBold.ttf -r 0x30-0x3A -o $OUT/ilf_font_digits_120.c --lv-font-name ilf_font_digits_120
echo "шрифты готовы"
