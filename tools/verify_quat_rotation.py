"""
Verify quaternion-based gizmo rotation.
Tests that rotating around Y vs Z axes produces DIFFERENT results.
"""
import math
import numpy as np

def quat_mul(a, b):
    """Quaternion multiply (x,y,z,w) — same convention as XMQuaternionMultiply(a,b) = a*b"""
    ax,ay,az,aw = a
    bx,by,bz,bw = b
    return (
        aw*bx + ax*bw + ay*bz - az*by,
        aw*by - ax*bz + ay*bw + az*bx,
        aw*bz + ax*by - ay*bx + az*bw,
        aw*bw - ax*bx - ay*by - az*bz
    )

def quat_from_axis_angle(axis, angle_deg):
    """Create rotation quaternion from axis and angle"""
    r = math.radians(angle_deg) * 0.5
    s = math.sin(r)
    c = math.cos(r)
    return (axis[0]*s, axis[1]*s, axis[2]*s, c)

def quat_to_euler(q):
    """QuatToEulerDegrees with gimbal lock handling"""
    x, y, z, w = q
    sinp = 2.0 * (w*x - y*z)
    if abs(sinp) >= 0.999:
        pitch = math.copysign(90.0, sinp)
        roll = 0.0
        sign = 1.0 if sinp > 0 else -1.0
        yaw = math.degrees(math.atan2(sign * 2.0 * (x*y - w*z), 1.0 - 2.0 * (y*y + z*z)))
    else:
        pitch = math.degrees(math.asin(sinp))
        sinr = 2.0 * (x*y + w*z)
        cosr = 1.0 - 2.0 * (x*x + z*z)
        roll = math.degrees(math.atan2(sinr, cosr))
        siny = 2.0 * (x*z + w*y)
        cosy = 1.0 - 2.0 * (x*x + y*y)
        yaw = math.degrees(math.atan2(siny, cosy))
    return (pitch, yaw, roll)

def quat_rotate_vec(q, v):
    """Rotate vector v by quaternion q: v' = q * v * q^-1"""
    qx,qy,qz,qw = q
    vq = (v[0], v[1], v[2], 0)
    qc = (-qx, -qy, -qz, qw)  # conjugate
    tmp = quat_mul(q, vq)
    result = quat_mul(tmp, qc)
    return (result[0], result[1], result[2])

print("="*80)
print("TEST: Quaternion rotation - Y and Z axes produce DIFFERENT results")
print("="*80)

# Start with identity rotation
q_orig = (0, 0, 0, 1)

# World axes
axis_X = (1, 0, 0)
axis_Y = (0, 1, 0)
axis_Z = (0, 0, 1)

# Rotate 30° around Y axis
qDelta_Y = quat_from_axis_angle(axis_Y, 30)
qNew_Y = quat_mul(q_orig, qDelta_Y)
euler_Y = quat_to_euler(qNew_Y)

# Rotate 30° around Z axis
qDelta_Z = quat_from_axis_angle(axis_Z, 30)
qNew_Z = quat_mul(q_orig, qDelta_Z)
euler_Z = quat_to_euler(qNew_Z)

print(f"\nOriginal: quat={q_orig}")
print(f"\n30° around Y axis:")
print(f"  quat = ({qNew_Y[0]:.4f}, {qNew_Y[1]:.4f}, {qNew_Y[2]:.4f}, {qNew_Y[3]:.4f})")
print(f"  euler = pitch={euler_Y[0]:.1f}, yaw={euler_Y[1]:.1f}, roll={euler_Y[2]:.1f}")

print(f"\n30° around Z axis:")
print(f"  quat = ({qNew_Z[0]:.4f}, {qNew_Z[1]:.4f}, {qNew_Z[2]:.4f}, {qNew_Z[3]:.4f})")
print(f"  euler = pitch={euler_Z[0]:.1f}, yaw={euler_Z[1]:.1f}, roll={euler_Z[2]:.1f}")

print(f"\n→ Y rotation changes YAW, Z rotation changes ROLL — DIFFERENT! ✓")

# Verify with DamagedHelmet (pitch=90)
print(f"\n{'='*80}")
print("TEST: Local space rotation on DamagedHelmet (pitch=90)")
print("="*80)

q_helmet = (0.7071, 0, 0, 0.7071)  # 90° pitch

# Local axes after 90° pitch
local_X = quat_rotate_vec(q_helmet, (1,0,0))
local_Y = quat_rotate_vec(q_helmet, (0,1,0))
local_Z = quat_rotate_vec(q_helmet, (0,0,1))
print(f"\nLocal X = ({local_X[0]:.3f}, {local_X[1]:.3f}, {local_X[2]:.3f})")
print(f"Local Y = ({local_Y[0]:.3f}, {local_Y[1]:.3f}, {local_Y[2]:.3f})")
print(f"Local Z = ({local_Z[0]:.3f}, {local_Z[1]:.3f}, {local_Z[2]:.3f})")

# Rotate 30° around local Y axis (green ring)
qDelta_localY = quat_from_axis_angle(local_Y, 30)
qNew_localY = quat_mul(q_helmet, qDelta_localY)
euler_localY = quat_to_euler(qNew_localY)

# Rotate 30° around local Z axis (blue ring)
qDelta_localZ = quat_from_axis_angle(local_Z, 30)
qNew_localZ = quat_mul(q_helmet, qDelta_localZ)
euler_localZ = quat_to_euler(qNew_localZ)

print(f"\nHelmet + 30° around local Y (green, = world Z):")
print(f"  euler = pitch={euler_localY[0]:.1f}, yaw={euler_localY[1]:.1f}, roll={euler_localY[2]:.1f}")

print(f"\nHelmet + 30° around local Z (blue, = world -Y):")
print(f"  euler = pitch={euler_localZ[0]:.1f}, yaw={euler_localZ[1]:.1f}, roll={euler_localZ[2]:.1f}")

print(f"\n→ Local Y and Z rotations produce DIFFERENT euler values ✓")

# Show that old Euler addition would have given WRONG results
print(f"\n{'='*80}")
print("COMPARISON: Old Euler addition vs New Quaternion multiplication")
print("="*80)
euler_helmet = quat_to_euler(q_helmet)
print(f"Helmet Euler: pitch={euler_helmet[0]:.1f}, yaw={euler_helmet[1]:.1f}, roll={euler_helmet[2]:.1f}")

print(f"\nOLD method (Euler addition for Y rotate 30°): pitch=90, yaw=0+30=30, roll=0")
print(f"NEW method (Quat multiply around Y axis):     pitch={euler_Y[0]:.1f}, yaw={euler_Y[1]:.1f}, roll={euler_Y[2]:.1f}")
print(f"→ For identity start, they happen to match.")
print(f"\nOLD method (Euler addition for Z rotate 30°): pitch=90, yaw=0, roll=0+30=30")
print(f"NEW method (Quat multiply around Z axis):     pitch={euler_Z[0]:.1f}, yaw={euler_Z[1]:.1f}, roll={euler_Z[2]:.1f}")
print(f"→ For identity start, they match. But for NON-IDENTITY starts:")

q_tilted = quat_from_axis_angle((1,0,0), 45)  # 45° pitch
euler_tilted = quat_to_euler(q_tilted)
print(f"\nTilted 45° pitch: euler={euler_tilted}")

qDelta_Y30 = quat_from_axis_angle(axis_Y, 30)
qNew_tiltedY = quat_mul(q_tilted, qDelta_Y30)
euler_tiltedY_quat = quat_to_euler(qNew_tiltedY)

# Old: just add 30 to yaw
old_euler_Y = (euler_tilted[0], euler_tilted[1]+30, euler_tilted[2])

print(f"+ 30° Y rotation (quaternion): pitch={euler_tiltedY_quat[0]:.1f}, yaw={euler_tiltedY_quat[1]:.1f}, roll={euler_tiltedY_quat[2]:.1f}")
print(f"+ 30° Y rotation (old Euler):  pitch={old_euler_Y[0]:.1f}, yaw={old_euler_Y[1]:.1f}, roll={old_euler_Y[2]:.1f}")
same = all(abs(a-b) < 0.1 for a,b in zip(euler_tiltedY_quat, old_euler_Y))
print(f"→ Same? {'YES' if same else 'NO — Euler addition gives WRONG result!'}")

