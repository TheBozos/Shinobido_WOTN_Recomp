"""review.py <first> <last> <out.png>: per cycle h<n>, its mission list (first capture) and three later captures."""
import glob
import sys

from PIL import Image

first, last, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
rows = []
for n in range(first, last + 1):
    fs = sorted(glob.glob(f'D:/Shinobido_WOTN_Recomp/build/cyc/h{n}/frame_*.png'))
    if not fs:
        continue
    pick = [fs[0], fs[len(fs) // 4], fs[len(fs) // 2], fs[-1]]
    rows.append([Image.open(f).convert('RGB').resize((320, 240)) for f in pick])
sheet = Image.new('RGB', (1280, 240 * max(1, len(rows))), 'white')
for r, row in enumerate(rows):
    for c, im in enumerate(row):
        sheet.paste(im, (320 * c, 240 * r))
sheet.save(out)
