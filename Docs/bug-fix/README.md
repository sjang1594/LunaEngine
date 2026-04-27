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

## #011 Mesh Not Visible After Phase 24

See `011_mesh_not_visible_after_phase24.md`.

---

## #012 Vulkan Tonemap Framebuffers Init

See `012_vulkan_tonemap_framebuffers_init.md`.

---

## #013 DX12 IBL Pipeline Dangling Pointers

**증상**: `CreateGraphicsPipelineState` 실패 (`0x80070057 E_INVALIDARG`). IBL deferred lighting 파이프라인 생성 시.

**원인 2가지**:

| # | 이슈 |
|---|------|
| 1 | `CreateRootSignature()` 내 로컬 배열(`ranges[]`, `params[]`)이 직렬화 전에 스코프 이탈 → dangling pointer |
| 2 | Root parameter 개수 8개인데 binding index 7/8/9 사용 → access violation |

**해결**:
- Root signature 생성 함수에서 배열을 직렬화 완료까지 유지
- Root parameter 개수/인덱스 일치시킴

---

## #014 Vulkan Shutdown Resource In Use

**증상**: 종료 시 8개 Vulkan validation error — `vkDestroyBuffer`, `vkFreeDescriptorSets`, `vkDestroyPipeline`이 VkCommandBuffer에서 사용 중인 리소스에 호출됨.

**원인**: `Application::Shutdown()`이 `ShutdownImGui()`를 `Backend::Shutdown()` **이전에** 호출. `ImGui_ImplVulkan_Shutdown()`이 내부 vertex/index buffer와 pipeline을 파괴할 때, frame command buffer가 여전히 참조 보유.

**해결**: `ShutdownImGui()`에서 `ImGui_ImplVulkan_Shutdown()` 호출 전 모든 command pool을 `vkResetCommandPool()`로 리셋:
```cpp
vkDeviceWaitIdle(dev);
for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
    if (_frames[i].cmdPool)    vkResetCommandPool(dev, _frames[i].cmdPool, 0);
    if (_computeFrames[i].cmdPool) vkResetCommandPool(dev, _computeFrames[i].cmdPool, 0);
}
if (_transferCmdPool) vkResetCommandPool(dev, _transferCmdPool, 0);
```

---

## #015 Vulkan Atmosphere Composite — Feedback Loop + Layout Mismatch + First-Frame Barrier Bug

**증상**:
1. Vulkan 검증 레이어: `vkCmdDraw(): VkImage ... layout VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL does not match previous known layout VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` — 매 프레임 10회 반복
2. `vkQueueSubmit()`: 다른 이미지가 `VK_IMAGE_LAYOUT_GENERAL`인데 `SHADER_READ_ONLY_OPTIMAL` 기대
3. `Exception 0xc0000005 at 0xffffffffffffffff` — 크래시

**원인 3가지**:

| # | 이슈 | 위치 |
|---|------|------|
| 1 | **Feedback loop**: `DrawComposite()`가 `_ppRenderPass`(`DONT_CARE` loadOp, `initialLayout=UNDEFINED`)를 사용 → HDR 씬 내용 폐기 + 렌더 패스 내부에서 `COLOR_ATTACHMENT_OPTIMAL`로 전환. `sceneTex` 디스크립터는 `SHADER_READ_ONLY_OPTIMAL` 기대 → layout mismatch + UB → 크래시 | `VulkanAtmosphere.cpp::DrawComposite()` |
| 2 | **First-frame skyView 레이아웃 오류**: `if (_precomputed)` 조건이 첫 프레임에서 `SHADER_READ_ONLY_OPTIMAL → GENERAL` barrier를 내보냄. skyView는 실제로는 `CreateLUTImages()`에서 설정된 `GENERAL` 상태였음 → 검증 레이어가 old layout 불일치 감지 | `VulkanAtmosphere::Update()` |
| 3 | **Render graph 선언 오류**: Sky Composite 패스가 `hHDR`에 대해 `.Read(SHADER_READ_ONLY)` + `.Write(SHADER_READ_ONLY, COLOR_ATTACHMENT_WRITE)` 선언 — layout과 access mask가 모순됨 | `VulkanBackend.cpp::CompositeFrame()` |

**근본 원인 상세**:

`_ppRenderPass`는 `initialLayout=UNDEFINED` + `DONT_CARE` loadOp 로 생성되어 있음 → render pass 시작 시 기존 픽셀(디퍼드 라이팅 결과)을 폐기하고 `COLOR_ATTACHMENT_OPTIMAL`로 전환. 동시에 `sceneTex` 디스크립터는 동일한 `_hdrView`를 `SHADER_READ_ONLY_OPTIMAL`로 바인딩 → 같은 이미지를 color attachment write + sampled read로 동시 접근하는 feedback loop. Vulkan은 별도 확장 없이 이를 허용하지 않음.

**해결**:

1. **`CreateAtmosphereRenderPass()` 신규 추가**: `LOAD_OP_LOAD` + `initialLayout=SHADER_READ_ONLY_OPTIMAL` + `finalLayout=SHADER_READ_ONLY_OPTIMAL` render pass 생성 (owned). 서브패스 의존성: `COLOR_ATTACHMENT_OUTPUT → COLOR_ATTACHMENT_OUTPUT`으로 디퍼드 라이팅 이후 sync 보장.

2. **`sceneTex` 제거**: `_compositeDescLayout`에서 binding 3 삭제 (4 → 3 bindings). `atmosphere_composite_vk.frag.glsl`에서 `sceneTex` 샘플러 제거, 씬 픽셀(`depth < 0.999`)은 `discard` — `LOAD_OP_LOAD`로 기존 HDR 내용이 보존되므로 별도 read 불필요.

3. **`_skyViewReady` 플래그 도입**: `Update()` 첫 호출 시에는 `SHADER_READ_ONLY → GENERAL` barrier 스킵 (skyView가 이미 `GENERAL`). 첫 Update 완료 후 `_skyViewReady = true` 설정.

4. **Render graph 수정**: Sky Composite 패스에서 `.Read(hHDR, ...)` 제거. `_atmosphereRenderPass`가 내부적으로 layout transition 처리 (`SHADER_READ_ONLY → COLOR_ATTACHMENT → SHADER_READ_ONLY`).

**결과**: 검증 레이어 오류 3종 모두 제거. 씬 픽셀은 `discard`로 보존, 하늘 픽셀만 sky-view LUT 값으로 오버라이트.

---

## TODO

| 항목 | 우선순위 |
|------|----------|
| Hi-Z double-buffer로 occlusion culling 재활성화 | HIGH |
| Async compute redesign (separate command lists) | MEDIUM |

---

## 파일 변경 요약

| 파일 | 버그 |
|------|------|
| `taa.frag.hlsl` | #001 |
| `tonemapping.frag.hlsl` | #001 |
| `VulkanBackend.cpp` | #002–009, #014, #015 |
| `VulkanBackend.h` | #006–007 |
| `deferred_lighting_ibl.frag.hlsl` | #005 |
| `gbuffer_vk.frag.glsl` | #005 |
| `Application.cpp` | #008 |
| `DX12Backend.cpp` | #009, #010, #013 |
| `DX12Backend.h` | #010 |
| `DX12Pipeline.cpp` | #013 |
| `gpu_cull.comp.hlsl` | #010 |
| `VulkanAtmosphere.cpp` | #015 |
| `VulkanAtmosphere.h` | #015 |
| `atmosphere_composite_vk.frag.glsl` | #015 |

