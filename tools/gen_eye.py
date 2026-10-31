#!/usr/bin/env python3
"""Генерирует сглаженные чёрные овалы глаз барашка как картинки LVGL (ARGB8888).

LVGL рисует скруглённые прямоугольники, а у персонажа глаза — настоящие эллипсы,
поэтому глаза храним картинками. Запуск: python3 tools/gen_eye.py
"""
import os

EYES = {"ilf_eye_big": (42, 69), "ilf_eye_small": (36, 58)}  # размеры как на макете
SS = 8  # суперсэмплинг для сглаживания края

out = os.path.join(os.path.dirname(__file__), "..", "components", "ilf_ui", "ilf_eye_img.c")
lines = ["/* Сгенерировано tools/gen_eye.py — не править руками */", '#include "lvgl.h"', ""]
for name, (w, h) in EYES.items():
    data = []
    a, b = w / 2.0, h / 2.0
    for y in range(h):
        for x in range(w):
            inside = 0
            for sy in range(SS):
                for sx in range(SS):
                    px = x + (sx + 0.5) / SS - a
                    py = y + (sy + 0.5) / SS - b
                    if (px / a) ** 2 + (py / b) ** 2 <= 1.0:
                        inside += 1
            alpha = round(255 * inside / (SS * SS))
            data += [0, 0, 0, alpha]  # B, G, R, A
    lines.append(f"static const uint8_t {name}_map[] = {{")
    for i in range(0, len(data), 32):
        lines.append("    " + ", ".join(str(v) for v in data[i:i + 32]) + ",")
    lines.append("};")
    lines.append(f"const lv_image_dsc_t {name} = {{")
    lines.append("    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,")
    lines.append(f"                .w = {w}, .h = {h}, .stride = {w * 4} }},")
    lines.append(f"    .data_size = sizeof({name}_map),")
    lines.append(f"    .data = {name}_map,")
    lines.append("};")
    lines.append("")
with open(out, "w") as f:
    f.write("\n".join(lines))
print("wrote", os.path.normpath(out))
