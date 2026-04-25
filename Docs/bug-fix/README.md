# LunaEngine Bug Fix Log

정리된 버그 수정 기록. 각 항목은 증상 → 원인 → 해결 순서로 기술.

---

## #001 TAA YCoCg 채널 스왑

**증상**: DX12에서 금/노란색 → 초록, 초록 → 금색으로 출력. 시간이 지날수록 보라/마젠타 틴트 누적.

**원인**: `taa.frag.hlsl`의 `YCoCgToRGB()` 역변환에서 Co(Chroma Orange)와 Cg(Chroma Green) 채널이 뒤바뀜. TAA가 90% history를 blend하며 매 프레임 색상 scramble → 10프레임 후 포화.

**해결**:
```hlsl
// c.x=Y, c.y=Cg+0.5, c.z=Co+0.5
float Co = c.z - 0.5;
float Cg = c.y - 0.5;
return saturate(float3(c.x + Co - Cg, c.x + Cg, c.x - Co - Cg));
```

**추가**: Luminance 기반 hue-preserving ACES 톤매핑으로 교체 (채도 높은 색상 hue shift 방지).

---

## #002–004 Vulkan Diagonal Stripe Artifacts

**증상**: Vulkan 백엔드에서 DamagedHelmet 메쉬에 대각선 metallic 줄무늬.

**원인**: 
1. **TAA Jitter 미적용** — `_vkCurJitter`가 항상 `{0,0}`. Halton 시퀀스 미할당.
2. **Anisotropic 필터링 불일치** — `_linearSampler` 생성 지점 3곳 중 일부만 anisotropic 8x 적용.

DX12는 TAA jitter + anisotropic 8x가 정상 작동하여 UV seam 아티팩트가 smooth됨.

**해결**:
- `UpdateMVP()`에 Halton(2,3) jitter 추가
- 모든 `_linearSampler` 생성 지점에 `anisotropyEnable=VK_TRUE, maxAnisotropy=8.0` 통일

---

## #005 DX12/Vulkan 렌더링 패리티

**증상**: DX12와 Vulkan 간 완전히 다른 렌더링 결과. DX12는 어두운 ambient, Vulkan은 붉은 emissive 번짐.

**원인 3가지**:

| # | 이슈 | 위치 |
|---|------|------|
| 1 | DX12 IBL 셰이더가 IBL 텍스처 샘플링 안 함 | `deferred_lighting_ibl.frag.hlsl` |
| 2 | Legacy Vulkan G-buffer `outAlbedo.a=1.0` → emissive로 해석 | `gbuffer_vk.frag.glsl` |
| 3 | `_linearSampler` 설정 불일치 | `VulkanBackend.cpp` 3곳 |

**해결**:
- DX12 IBL: Split-sum IBL 샘플링 추가 (irradiance + prefiltered + BRDF LUT)
- Vulkan G-buffer: `outAlbedo.a = 0.0`으로 변경
- 모든 sampler 생성 지점 통일

---

## #006–007 Vulkan Device Lost (Resize + IBL)

**증상**: 윈도우 리사이즈 또는 IBL 로딩 시 `VK_ERROR_DEVICE_LOST`.

**원인**:

| # | 이슈 |
|---|------|
| 1 | IBL precompute 전체가 단일 커맨드 버퍼 → GPU TDR (2초 타임아웃) |
| 2 | `BeginSingleTimeCommands()`가 `_frames[0].cmdPool` 공유 → race condition |
| 3 | RT descriptor가 리사이즈 후 destroy된 view 참조 |
| 4 | Render graph에 RT pass 의존성 미선언 |
| 5 | `Resize()` 콜백이 스왑체인 생성 후 발생 → 즉시 재생성 필요 |

**해결**:
- IBL precompute를 스테이지별 8개 separate submission으로 분할
- `_transferCmdPool` 전용 커맨드 풀 생성
- `RecreateSwapchain()` 후 RT descriptor 업데이트
- `_pendingResize` 패턴으로 deferred resize
- 리사이즈 후 2프레임간 RT 비활성화 (`_framesSinceResize`)

---

## #008 Custom Title Bar 초기화 순서

**증상**: `customTitleBar=true`일 때 첫 프레임에서 device lost.

**원인**: `CustomTitleBar::Init()`이 `WS_CAPTION` 제거 시 `WM_SIZE` 발생. 이때 GLFW 콜백이 이미 등록되어 있어 `_pendingResize=true` 설정 → 첫 프레임에 스왑체인 재생성 → stale descriptor 참조.

**해결**: 초기화 순서 변경
```
1. glfwCreateWindow()
2. CustomTitleBar::Init()     ← 콜백 등록 전
3. glfwGetFramebufferSize()   ← 실제 크기 조회
4. GLFW 콜백 등록
5. IRenderContext::Initialize(실제 크기)
```

---

## #009 Hi-Z Depth Layout Mismatch

**증상**: Hi-Z 활성화 후 첫 프레임에서 device lost.

**원인**: G-buffer pass 종료 후 depth는 `READ_ONLY_OPTIMAL`. `BuildHiZPyramid()`가 `ATTACHMENT_OPTIMAL`로 전환 후 복원 안 함. Render graph는 `READ_ONLY_OPTIMAL` 가정 → layout mismatch.

```
G-buffer ends → READ_ONLY_OPTIMAL
BuildHiZPyramid → TRANSFER_SRC → ATTACHMENT_OPTIMAL ← 여기서 멈춤
Render graph expects → READ_ONLY_OPTIMAL ← MISMATCH
```

**해결**: `CompositeFrame()`에서 `BuildHiZPyramid()` 호출 후 depth를 `READ_ONLY_OPTIMAL`로 복원하는 barrier 추가.

**빌드 에러 수정**:
- `hiz_generate.comp.hlsl`: `<ClCompile>` → `<None>`
- `Init()` → `Initialize()`
- `InsertEndTimestamp(cmd, "...")`에서 두 번째 인자 제거

---

## #010 DX12 GPU-Driven Flickering

**증상**: DX12에서 오브젝트가 랜덤하게 나타났다 사라짐 (flickering).

**원인 2가지**:

| # | 이슈 | 상태 |
|---|------|------|
| 1 | Frustum plane 정규화 시 near-zero 길이 → NaN/inf | ✅ Fixed |
| 2 | `_hizTexture` 단일 인스턴스 → cross-frame race | ❌ Hi-Z 비활성화 |

**해결 (원인 1)**:
```cpp
float len = XMVectorGetX(XMVector3Length(p));
if (len > 0.0001f) {           // safety check
    p = XMVectorScale(p, 1.0f / len);
    XMStoreFloat4(&cullCB.planes[i], p);
}
```

**해결 (원인 2)**: `cullCB.enableHiZ = 0` — Hi-Z double-buffering 구현 전까지 비활성화.

**추가 수정**:
- `_objectDataBuffer` → per-frame array
- Indirect buffer: `COMMON` → `UNORDERED_ACCESS` state
- `ExecuteIndirect` 전 명시적 `OMSetRenderTargets` 호출

---

## TODO

| 항목 | 우선순위 |
|------|----------|
| Hi-Z double-buffer로 occlusion culling 재활성화 | HIGH |
| Async compute redesign (separate command lists) | MEDIUM |
| Vulkan shutdown validation 에러 정리 | LOW |

---

## 파일 변경 요약

| 파일 | 버그 |
|------|------|
| `taa.frag.hlsl` | #001 |
| `tonemapping.frag.hlsl` | #001 |
| `VulkanBackend.cpp` | #002–009 |
| `VulkanBackend.h` | #006–007 |
| `deferred_lighting_ibl.frag.hlsl` | #005 |
| `gbuffer_vk.frag.glsl` | #005 |
| `Application.cpp` | #008 |
| `DX12Backend.cpp` | #009, #010 |
| `DX12Backend.h` | #010 |
| `gpu_cull.comp.hlsl` | #010 |

