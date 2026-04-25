#pragma once
#include <DirectXMath.h>

namespace Luna
{
using namespace DirectX;

// Convert a unit quaternion (XMFLOAT4 {x,y,z,w}) to Euler angles in degrees.
// Convention: YXZ intrinsic (= ZXY extrinsic), matching XMMatrixRotationRollPitchYaw(pitch, yaw, roll).
//   rot.x = pitch (around X), rot.y = yaw (around Y), rot.z = roll (around Z)
inline XMFLOAT3 QuatToEulerDegrees(const XMFLOAT4& q)
{
    XMFLOAT3 rot;

    // pitch from M[1][2] = -sin(pitch)
    float sinp = 2.0f * (q.w * q.x - q.y * q.z);

    if (fabsf(sinp) >= 0.999f)
    {
        // Gimbal lock at pitch ≈ ±90°: yaw and roll share one DOF.
        // Set roll=0, solve yaw from R[0][1] and R[0][0].
        rot.x = XMConvertToDegrees(copysignf(XM_PIDIV2, sinp));
        rot.z = 0.0f;
        float sign = (sinp > 0.0f) ? 1.0f : -1.0f;
        rot.y = XMConvertToDegrees(atan2f(
            sign * 2.0f * (q.x * q.y - q.w * q.z),
            1.0f - 2.0f * (q.y * q.y + q.z * q.z)));
    }
    else
    {
        rot.x = XMConvertToDegrees(asinf(sinp));

        // roll from atan2(M[1][0], M[1][1])
        float sinr = 2.0f * (q.x * q.y + q.w * q.z);
        float cosr = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
        rot.z = XMConvertToDegrees(atan2f(sinr, cosr));

        // yaw from atan2(M[0][2], M[2][2])
        float siny = 2.0f * (q.x * q.z + q.w * q.y);
        float cosy = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
        rot.y = XMConvertToDegrees(atan2f(siny, cosy));
    }

    return rot;
}

} // namespace Luna

