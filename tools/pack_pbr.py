import zipfile, io, os, struct, time
import numpy as np
from PIL import Image

ZIPS = [
    ('sand', '/home/neto/Downloads/pbr ambiente/Ground054_1K-PNG.zip', 'Ground054_1K-PNG_'),
    ('dirt', '/home/neto/Downloads/pbr ambiente/Ground048_1K-PNG.zip', 'Ground048_1K-PNG_'),
    ('grass', '/home/neto/Downloads/pbr ambiente/Grass007_1K-PNG.zip', 'Grass007_1K-PNG_'),
    ('rock', '/home/neto/Downloads/pbr ambiente/Rock058_1K-PNG.zip', 'Rock058_1K-PNG_')
]

OUT_DIR = '/home/neto/Área de trabalho/isolated/assets'
os.makedirs(OUT_DIR, exist_ok=True)

SIZE = 1024
LAYERS = len(ZIPS)

def pack_e0t(filename, width, height, layers, data_bytes):
    # Header: magic 'E0T1', uint16 width, height, layers, format (4=RGBA8), uint32 dataSize
    header = struct.pack('<4sHHHHI', b'E0T1', width, height, layers, 4, len(data_bytes))
    path = os.path.join(OUT_DIR, filename)
    with open(path, 'wb') as f:
        f.write(header)
        f.write(data_bytes)
    print(f'Wrote {path}: {os.path.getsize(path)/1e6:.2f} MB')

t0 = time.time()
print(f'Packing {LAYERS} layers at {SIZE}x{SIZE}...')

alb_list = []
nrm_list = []

for idx, (name, zpath, prefix) in enumerate(ZIPS):
    print(f'  Layer {idx}: {name} from {os.path.basename(zpath)}...')
    zf = zipfile.ZipFile(zpath)
    
    col = Image.open(io.BytesIO(zf.read(f'{prefix}Color.png'))).convert('RGB')
    if col.size != (SIZE, SIZE):
        col = col.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
        
    rgh = Image.open(io.BytesIO(zf.read(f'{prefix}Roughness.png'))).convert('L')
    if rgh.size != (SIZE, SIZE):
        rgh = rgh.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
        
    nrm = Image.open(io.BytesIO(zf.read(f'{prefix}NormalGL.png'))).convert('RGB')
    if nrm.size != (SIZE, SIZE):
        nrm = nrm.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
        
    ao = Image.open(io.BytesIO(zf.read(f'{prefix}AmbientOcclusion.png'))).convert('L')
    if ao.size != (SIZE, SIZE):
        ao = ao.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
        
    # Combine Albedo (RGB) + Roughness (A)
    alb_rgba = np.dstack([np.array(col, dtype=np.uint8), np.array(rgh, dtype=np.uint8)])
    alb_list.append(alb_rgba.tobytes())
    
    # Combine Normal (RGB) + AO (A)
    nrm_rgba = np.dstack([np.array(nrm, dtype=np.uint8), np.array(ao, dtype=np.uint8)])
    nrm_list.append(nrm_rgba.tobytes())

pack_e0t('terrain_albedo.e0t', SIZE, SIZE, LAYERS, b''.join(alb_list))
pack_e0t('terrain_normal.e0t', SIZE, SIZE, LAYERS, b''.join(nrm_list))
print(f'Done in {time.time() - t0:.2f} s')
