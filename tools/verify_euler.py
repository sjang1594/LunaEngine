w"""
Verify QuatToEulerDegrees roundtrip with XMMatrixRotationRollPitchYaw.

XMMatrixRotationRollPitchYaw(pitch, yaw, roll) = RotY(yaw) * RotX(pitch) * RotZ(roll)

We build the rotation matrix from Euler, extract quaternion, decompose back to Euler,
rebuild the matrix, and compare. All must match.
"""
import math
import numpy as np

def rot_x(deg):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    return np.array([[1,0,0],[0,c,s],[0,-s,c]])  # DirectXMath row-major convention

def rot_y(deg):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    return np.array([[c,0,-s],[0,1,0],[s,0,c]])

def rot_z(deg):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    return np.array([[c,s,0],[-s,c,0],[0,0,1]])

def roll_pitch_yaw(pitch, yaw, roll):
    """XMMatrixRotationRollPitchYaw: M = RotZ(roll) * RotX(pitch) * RotY(yaw) for row-vectors
    i.e. v' = v * Rz * Rx * Ry"""
    return rot_z(roll) @ rot_x(pitch) @ rot_y(yaw)

def mat_to_quat(R):
    """Convert 3x3 rotation matrix to quaternion (x,y,z,w)"""
    tr = R[0,0] + R[1,1] + R[2,2]
    if tr > 0:
        s = 0.5 / math.sqrt(tr + 1.0)
        w = 0.25 / s
        x = (R[1,2] - R[2,1]) * s
        y = (R[2,0] - R[0,2]) * s
        z = (R[0,1] - R[1,0]) * s
    elif R[0,0] > R[1,1] and R[0,0] > R[2,2]:
        s = 2.0 * math.sqrt(1.0 + R[0,0] - R[1,1] - R[2,2])
        w = (R[1,2] - R[2,1]) / s
        x = 0.25 * s
        y = (R[1,0] + R[0,1]) / s
        z = (R[2,0] + R[0,2]) / s
    elif R[1,1] > R[2,2]:
        s = 2.0 * math.sqrt(1.0 + R[1,1] - R[0,0] - R[2,2])
        w = (R[2,0] - R[0,2]) / s
        x = (R[1,0] + R[0,1]) / s
        y = 0.25 * s
        z = (R[1,2] + R[2,1]) / s
    else:
        s = 2.0 * math.sqrt(1.0 + R[2,2] - R[0,0] - R[1,1])
        w = (R[0,1] - R[1,0]) / s
        x = (R[2,0] + R[0,2]) / s
        y = (R[1,2] + R[2,1]) / s
        z = 0.25 * s
    return (x, y, z, w)

def quat_to_euler(q):
    """QuatToEulerDegrees matching XMMatrixRotationRollPitchYaw (YXZ intrinsic)
    With gimbal lock handling."""
    x, y, z, w = q
    sinp = 2.0 * (w*x - y*z)
    
    if abs(sinp) >= 0.999:
        # Gimbal lock
        pitch = math.copysign(90.0, sinp)
        roll = 0.0
        sign = 1.0 if sinp > 0 else -1.0
        yaw = math.degrees(math.atan2(sign * 2.0 * (x*y - w*z), 
                                       1.0 - 2.0 * (y*y + z*z)))
    else:
        pitch = math.degrees(math.asin(sinp))
        sinr = 2.0 * (x*y + w*z)
        cosr = 1.0 - 2.0 * (x*x + z*z)
        roll = math.degrees(math.atan2(sinr, cosr))
        siny = 2.0 * (x*z + w*y)
        cosy = 1.0 - 2.0 * (x*x + y*y)
        yaw = math.degrees(math.atan2(siny, cosy))
    
    return (pitch, yaw, roll)

def test_roundtrip(pitch, yaw, roll, label=""):
    # Build matrix from Euler
    M1 = roll_pitch_yaw(pitch, yaw, roll)
    # Extract quaternion
    q = mat_to_quat(M1)
    # Decompose back to Euler
    p2, y2, r2 = quat_to_euler(q)
    # Rebuild matrix from decomposed Euler
    M2 = roll_pitch_yaw(p2, y2, r2)
    
    # Compare matrices (should be identical)
    diff = np.max(np.abs(M1 - M2))
    ok = diff < 1e-6
    
    status = "OK" if ok else "FAIL"
    print(f"  [{status}] {label:30s} | input=({pitch:8.2f},{yaw:8.2f},{roll:8.2f}) "
          f"→ quat=({q[0]:+.4f},{q[1]:+.4f},{q[2]:+.4f},{q[3]:+.4f}) "
          f"→ euler=({p2:8.2f},{y2:8.2f},{r2:8.2f}) | mat_diff={diff:.2e}")
    return ok

print("=" * 120)
print("TEST 1: Single-axis rotations")
print("=" * 120)
test_roundtrip(0, 0, 0, "identity")
test_roundtrip(30, 0, 0, "pitch 30")
test_roundtrip(0, 45, 0, "yaw 45")
test_roundtrip(0, 0, 60, "roll 60")
test_roundtrip(-30, 0, 0, "pitch -30")
test_roundtrip(0, -45, 0, "yaw -45")
test_roundtrip(0, 0, -60, "roll -60")

print()
print("=" * 120)
print("TEST 2: Multi-axis rotations")
print("=" * 120)
test_roundtrip(20, 30, 40, "pitch20 yaw30 roll40")
test_roundtrip(45, 60, 30, "pitch45 yaw60 roll30")
test_roundtrip(-20, 150, -30, "pitch-20 yaw150 roll-30")
test_roundtrip(10, -170, 80, "pitch10 yaw-170 roll80")

print()
print("=" * 120)
print("TEST 3: Gimbal lock (pitch = ±90)")
print("=" * 120)
test_roundtrip(90, 0, 0, "pitch90 (gimbal lock)")
test_roundtrip(-90, 0, 0, "pitch-90 (gimbal lock)")
test_roundtrip(90, 30, 0, "pitch90 yaw30 (gimbal)")
test_roundtrip(-90, 45, 0, "pitch-90 yaw45 (gimbal)")
# These have non-zero roll which gets absorbed into yaw at gimbal lock
test_roundtrip(90, 20, 15, "pitch90 yaw20 roll15 (absorbed)")
test_roundtrip(-90, 20, 15, "pitch-90 yaw20 roll15 (absorbed)")

print()
print("=" * 120)
print("TEST 4: DamagedHelmet.glb quaternion")
print("=" * 120)
q_helmet = (0.7071068286895752, 0, 0, 0.7071068286895752)
p, y, r = quat_to_euler(q_helmet)
print(f"  Quaternion: {q_helmet}")
print(f"  Decomposed: pitch={p:.2f}, yaw={y:.2f}, roll={r:.2f}")
M = roll_pitch_yaw(p, y, r)
print(f"  Recomposed matrix row 0: [{M[0,0]:.4f}, {M[0,1]:.4f}, {M[0,2]:.4f}]")
print(f"  Recomposed matrix row 1: [{M[1,0]:.4f}, {M[1,1]:.4f}, {M[1,2]:.4f}]")
print(f"  Recomposed matrix row 2: [{M[2,0]:.4f}, {M[2,1]:.4f}, {M[2,2]:.4f}]")
# Expected: 90° rotation around X
# RotX(90) row-major = [[1,0,0],[0,0,1],[0,-1,0]]
print(f"  Expected RotX(90) row 0: [1.0000, 0.0000, 0.0000]")
print(f"  Expected RotX(90) row 1: [0.0000, 0.0000, 1.0000]")
print(f"  Expected RotX(90) row 2: [0.0000, -1.0000, 0.0000]")
diff = np.max(np.abs(M - rot_x(90)))
print(f"  Matrix difference from RotX(90): {diff:.2e} {'OK' if diff < 1e-6 else 'FAIL'}")

print()
print("=" * 120)
print("TEST 5: Verify Y and Z axes are DIFFERENT after rotation")  
print("=" * 120)
# World axes
axes_world = np.array([[1,0,0],[0,1,0],[0,0,1]], dtype=float)

# For DamagedHelmet (pitch=90), local axes = worldDir * rotMat
R_helmet = rot_x(90)  # RotX(90) in row-major
print("  DamagedHelmet local axes (v * RotX(90)):")
for i, name in enumerate(['X','Y','Z']):
    local = axes_world[i] @ R_helmet  # row-vector * matrix
    print(f"    Local {name}: ({local[0]:.3f}, {local[1]:.3f}, {local[2]:.3f})")

print()
print("  Global axes (no rotation):")
for i, name in enumerate(['X','Y','Z']):
    print(f"    World {name}: ({axes_world[i,0]:.3f}, {axes_world[i,1]:.3f}, {axes_world[i,2]:.3f})")

# Simulate camera view projection (30° elevation orbit cam)
print()
print("  Screen projection from default camera (30° elevation):")
cam_forward = np.array([0, -math.sin(math.radians(30)), math.cos(math.radians(30))])
cam_right = np.array([1, 0, 0])
cam_up = np.cross(cam_forward, cam_right)
cam_up = cam_up / np.linalg.norm(cam_up)

print(f"    Camera: right={cam_right}, up=({cam_up[0]:.3f},{cam_up[1]:.3f},{cam_up[2]:.3f}), fwd=({cam_forward[0]:.3f},{cam_forward[1]:.3f},{cam_forward[2]:.3f})")

print()
print("  Global axes projected to screen (x_screen, y_screen):")
for i, name in enumerate(['X','Y','Z']):
    ax = axes_world[i]
    sx = np.dot(ax, cam_right)
    sy = np.dot(ax, cam_up)
    sz = np.dot(ax, cam_forward)  # depth
    length = math.sqrt(sx*sx + sy*sy)
    print(f"    {name}: screen=({sx:+.3f}, {sy:+.3f}) depth={sz:+.3f} screen_len={length:.3f}")

print()
print("  Local axes (DamagedHelmet) projected to screen:")
for i, name in enumerate(['X','Y','Z']):
    local = axes_world[i] @ R_helmet
    sx = np.dot(local, cam_right)
    sy = np.dot(local, cam_up)
    sz = np.dot(local, cam_forward)
    length = math.sqrt(sx*sx + sy*sy)
    print(f"    {name}: screen=({sx:+.3f}, {sy:+.3f}) depth={sz:+.3f} screen_len={length:.3f}")

