#!/usr/bin/env python3
"""Вырезает голову барашка из макета интерфейса и делает из неё картинку LVGL.

Источник: assets/ui/ilf_ui_mockup_v2.png (макет от Саввы, 31.10.2026), первый кадр,
спокойный барашек. Глаза закрашиваются (inpaint), потому что прошивка рисует их сама:
они моргают, смотрят по сторонам и меняют форму. Носик «Y» остаётся в картинке.

Пишет:
  components/ilf_ui/ilf_head_img.c   — ARGB8888-картинка головы
  components/ilf_ui/ilf_head_geom.h  — где на экране глаза, нос, рот

Запуск: pip install opencv-python-headless pillow numpy && python3 tools/gen_head.py
"""
import os

import cv2
import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "assets", "ui", "ilf_ui_mockup_v2.png")
OUT_C = os.path.join(ROOT, "components", "ilf_ui", "ilf_head_img.c")
OUT_H = os.path.join(ROOT, "components", "ilf_ui", "ilf_head_geom.h")
OUT_PNG = os.path.join(ROOT, "assets", "ui", "ilf_head.png")

# Первая ячейка макета (координаты в пикселях макета)
CELL = (0, 100, 313, 330)          # x0, y0, x1, y1
CROP = (14, 24, 306, 218)          # голова с запасом, внутри ячейки
EYE_L = (89, 114, 124, 172)        # x0, y0, x1, y1 включительно
EYE_R = (204, 114, 238, 171)
NOSE_BOTTOM = 180                  # низ носика «Y»

SCALE = 368 / 313                  # ширина ячейки макета = ширина экрана
SCREEN_CX = 184                    # середина между глазами — по центру экрана
EYE_CY_SCREEN = 214                # где на экране центр глаз


def main():
    im = np.array(Image.open(SRC).convert("RGB"))
    cell = im[CELL[1]:CELL[3], CELL[0]:CELL[2]].copy()
    g = cell.mean(axis=2)

    # маска глаз: тёмные пиксели внутри прямоугольников глаз, с запасом на ореол
    mask = np.zeros(g.shape, np.uint8)
    for x0, y0, x1, y1 in (EYE_L, EYE_R):
        win = (g[y0 - 3:y1 + 4, x0 - 3:x1 + 4] < 200).astype(np.uint8) * 255
        mask[y0 - 3:y1 + 4, x0 - 3:x1 + 4] = win
    mask = cv2.dilate(mask, np.ones((5, 5), np.uint8))
    bgr = cv2.cvtColor(cell, cv2.COLOR_RGB2BGR)
    filled = cv2.inpaint(bgr, mask, 9, cv2.INPAINT_TELEA)
    filled = cv2.cvtColor(filled, cv2.COLOR_BGR2RGB)
    # лёгкое размытие только по закрашенному месту, чтобы не было пятен
    blur = cv2.GaussianBlur(filled, (0, 0), 3)
    soft = cv2.GaussianBlur(mask.astype(np.float32) / 255, (0, 0), 2)[..., None]
    filled = (filled * (1 - soft) + blur * soft).astype(np.uint8)

    x0, y0, x1, y1 = CROP
    head = filled[y0:y1, x0:x1].astype(np.float32)
    w = round((x1 - x0) * SCALE)
    h = round((y1 - y0) * SCALE)
    head = np.array(Image.fromarray(head.clip(0, 255).astype(np.uint8)).resize((w, h), Image.LANCZOS)).astype(np.float32)

    # фон чёрный: прозрачность из яркости, цвет «распремножаем»
    lum = head.mean(axis=2)
    alpha = np.clip((lum - 12) / 60, 0, 1)
    alpha[lum < 12] = 0
    rgb = np.where(alpha[..., None] > 0, head / np.maximum(alpha[..., None], 1e-3), 0).clip(0, 255)

    rgba = np.dstack([rgb, alpha * 255]).round().astype(np.uint8)
    Image.fromarray(rgba, "RGBA").save(OUT_PNG)

    def to_screen(px, py):
        return (px - x0) * SCALE + img_x, (py - y0) * SCALE + img_y

    eye_mid_x = ((EYE_L[0] + EYE_L[2]) / 2 + (EYE_R[0] + EYE_R[2]) / 2) / 2
    eye_mid_y = (EYE_L[1] + EYE_L[3]) / 2
    img_x = round(SCREEN_CX - (eye_mid_x - x0) * SCALE)
    img_y = round(EYE_CY_SCREEN - (eye_mid_y - y0) * SCALE)

    lx, ly = to_screen((EYE_L[0] + EYE_L[2]) / 2, eye_mid_y)
    rx, _ = to_screen((EYE_R[0] + EYE_R[2]) / 2, eye_mid_y)
    _, nose_y = to_screen(0, NOSE_BOTTOM)
    eye_w = round((EYE_L[2] - EYE_L[0] + 1) * SCALE)
    eye_h = round((EYE_L[3] - EYE_L[1] + 1) * SCALE)

    data = rgba[..., [2, 1, 0, 3]].tobytes()  # LVGL ARGB8888 = B, G, R, A
    lines = ["/* Сгенерировано tools/gen_head.py из assets/ui/ilf_ui_mockup_v2.png — не править руками */",
             '#include "lvgl.h"', "", "static const uint8_t ilf_head_map[] = {"]
    for i in range(0, len(data), 32):
        lines.append("    " + ",".join(str(b) for b in data[i:i + 32]) + ",")
    lines += ["};", "const lv_image_dsc_t ilf_head = {",
              "    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,",
              f"                .w = {w}, .h = {h}, .stride = {w * 4} }},",
              "    .data_size = sizeof(ilf_head_map),", "    .data = ilf_head_map,", "};", ""]
    with open(OUT_C, "w") as f:
        f.write("\n".join(lines))

    with open(OUT_H, "w") as f:
        f.write(f"""/* Сгенерировано tools/gen_head.py — не править руками */
#pragma once

/* картинка головы и её место на экране */
#define HEAD_IMG_X {img_x}
#define HEAD_IMG_Y {img_y}
#define HEAD_IMG_W {w}
#define HEAD_IMG_H {h}

/* глаза (центры) и их размер, как на макете */
#define EYE_L_CX {round(lx)}
#define EYE_R_CX {round(rx)}
#define EYE_CY   {round(ly)}
#define EYE_W    {eye_w}
#define EYE_H    {eye_h}

/* низ носика «Y»: под ним рисуются рты */
#define HEAD_CX  {SCREEN_CX}
#define NOSE_Y   {round(nose_y)}
""")
    print(f"head {w}x{h} at ({img_x},{img_y}); eyes {eye_w}x{eye_h} at x={round(lx)},{round(rx)} y={round(ly)}; nose_y={round(nose_y)}")


if __name__ == "__main__":
    main()
