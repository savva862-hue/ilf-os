#!/usr/bin/env bash
# Собрать симулятор, прогнать сценки и сложить PNG в sim/out/
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S sim -B build/sim -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/sim
mkdir -p sim/out
./build/sim/ilf_sim sim/out
python3 - <<'EOF'
import glob, os
from PIL import Image, ImageDraw
files = sorted(glob.glob("sim/out/*.ppm"))
imgs = []
for f in files:
    im = Image.open(f).convert("RGB")
    im.save(f[:-4] + ".png")
    os.remove(f)
    imgs.append((os.path.basename(f)[:-4], im))
# общая картинка: 4 в ряд
cols, pad = 4, 24
w, h = imgs[0][1].size
rows = (len(imgs) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + pad + 30) + pad), (40, 36, 46))
d = ImageDraw.Draw(sheet)
for i, (name, im) in enumerate(imgs):
    x = pad + (i % cols) * (w + pad)
    y = pad + (i // cols) * (h + pad + 30)
    sheet.paste(im, (x, y))
    d.text((x, y + h + 6), name, fill=(220, 210, 200))
sheet.save("sim/out/all.png")
print("sheet: sim/out/all.png")
EOF
