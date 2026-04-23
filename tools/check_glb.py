import struct, json
from PIL import Image
import numpy as np
import io

path = r'C:\Users\Administrator\Documents\project\personal\LunaEngine-source\LunaApp\Assets\DamagedHelmet.glb'
with open(path, 'rb') as f:
    magic, version, length = struct.unpack('<III', f.read(12))
    chunk_len, chunk_type = struct.unpack('<II', f.read(8))
    json_data = json.loads(f.read(chunk_len))
    chunk_len2, chunk_type2 = struct.unpack('<II', f.read(8))
    bin_data = f.read(chunk_len2)

mat = json_data['materials'][0]
pbr = mat['pbrMetallicRoughness']
bc_tex_idx = pbr['baseColorTexture']['index']
bc_img_idx = json_data['textures'][bc_tex_idx]['source']
bc_img = json_data['images'][bc_img_idx]
print(f"BaseColor: texture idx={bc_tex_idx}, image idx={bc_img_idx}, mime={bc_img.get('mimeType','?')}")

bv = json_data['bufferViews'][bc_img['bufferView']]
offset = bv.get('byteOffset', 0)
img_bytes = bin_data[offset:offset+bv['byteLength']]

img = Image.open(io.BytesIO(img_bytes))
arr = np.array(img)
print(f"Texture size: {img.size}, mode: {img.mode}")
h, w = arr.shape[:2]
print(f"Center pixel: R={arr[h//2,w//2,0]} G={arr[h//2,w//2,1]} B={arr[h//2,w//2,2]}")
avg = arr[:,:,:3].mean(axis=(0,1))
print(f"Average RGB: R={avg[0]:.1f} G={avg[1]:.1f} B={avg[2]:.1f}")

# Print all texture indices
print("\n--- All textures in material ---")
if 'baseColorTexture' in pbr:
    t = pbr['baseColorTexture']['index']
    s = json_data['textures'][t]['source']
    print(f"  baseColor: tex={t}, img={s}, bufView={json_data['images'][s].get('bufferView')}")
if 'metallicRoughnessTexture' in pbr:
    t = pbr['metallicRoughnessTexture']['index']
    s = json_data['textures'][t]['source']
    print(f"  metalRough: tex={t}, img={s}, bufView={json_data['images'][s].get('bufferView')}")
if 'normalTexture' in mat:
    t = mat['normalTexture']['index']
    s = json_data['textures'][t]['source']
    print(f"  normal: tex={t}, img={s}, bufView={json_data['images'][s].get('bufferView')}")
if 'emissiveTexture' in mat:
    t = mat['emissiveTexture']['index']
    s = json_data['textures'][t]['source']
    print(f"  emissive: tex={t}, img={s}, bufView={json_data['images'][s].get('bufferView')}")

# Save baseColor to disk for visual inspection
img.save(r'C:\Users\Administrator\Desktop\basecolor_extracted.png')
print("\nSaved basecolor to Desktop/basecolor_extracted.png")

