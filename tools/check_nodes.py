import struct, json, math

for name in ['DamagedHelmet.glb', 'Box.glb']:
    path = rf'C:\Users\Administrator\Documents\project\personal\LunaEngine-source\LunaApp\Assets\{name}'
    try:
        with open(path, 'rb') as f:
            magic, ver, length = struct.unpack('<III', f.read(12))
            cl, ct = struct.unpack('<II', f.read(8))
            j = json.loads(f.read(cl))
        print(f"\n=== {name} ===")
        for i, n in enumerate(j.get('nodes', [])):
            print(f"  Node {i}: {n.get('name','?')}")
            if 'rotation' in n:
                r = n['rotation']
                print(f"    rotation(xyzw): {r}")
                # Convert to Euler matching XMMatrixRotationRollPitchYaw(pitch,yaw,roll)
                x, y, z, w = r
                sinp = 2*(w*x - y*z)
                pitch = math.copysign(90, sinp) if abs(sinp) >= 1 else math.degrees(math.asin(sinp))
                sinr = 2*(x*y + w*z)
                cosr = 1 - 2*(x*x + z*z)
                roll = math.degrees(math.atan2(sinr, cosr))
                siny = 2*(x*z + w*y)
                cosy = 1 - 2*(x*x + y*y)
                yaw = math.degrees(math.atan2(siny, cosy))
                print(f"    euler(pitch,yaw,roll) = ({pitch:.2f}, {yaw:.2f}, {roll:.2f})")
            if 'translation' in n:
                print(f"    translation: {n['translation']}")
            if 'scale' in n:
                print(f"    scale: {n['scale']}")
            if 'matrix' in n:
                print(f"    matrix: {n['matrix']}")
    except Exception as e:
        print(f"  Error: {e}")

