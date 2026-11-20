# Junta os quadros gerados por tools/preview_clips.py em folhas de contato (Python do sistema, usa PIL).
# Uso: python3 tools/compose_sheets.py DIR LISTA.txt [colunas]
import sys, os, glob, re
from PIL import Image, ImageDraw
d, lst = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 8
names = [l.strip() for l in open(lst, encoding='utf-8') if l.strip()]
for ci, name in enumerate(names):
    fs = sorted(glob.glob(os.path.join(d, 'c%02d_*_f*.png' % ci)))
    if not fs: continue
    rows = (len(fs) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * 300, rows * 300 + 20), 'black')
    ImageDraw.Draw(sheet).text((4, 4), name, fill=(0, 255, 0))
    for i, f in enumerate(fs):
        im = Image.open(f).convert('RGB').resize((300, 300))
        ImageDraw.Draw(im).text((4, 4), re.search(r'_f(\d+)', f).group(0)[1:], fill=(255, 255, 0))
        sheet.paste(im, ((i % cols) * 300, 20 + (i // cols) * 300))
    sheet.save(os.path.join(d, 'sheet_%02d.png' % ci))
    print('sheet', ci, name)
