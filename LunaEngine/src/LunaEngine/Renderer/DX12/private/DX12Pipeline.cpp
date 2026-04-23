#include "LunaPCH.h"
#include "Renderer/DX12/Public/DX12Backend.h"
#include "Renderer/DX12/Public/DX12Pipeline.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Renderer/HAL/Public/IRenderContext.h"
#include "Logger/Logger.h"

#include <dxcapi.h>
#include <fstream>
#include <vector>

#ifdef LUNA_ENABLE_SLANG
// ---------------------------------------------------------------------------
// SlangBlobAdapter — wraps a Slang DXIL ISlangBlob as an IDxcBlob so the
// downstream CreatePipelineState path needs no changes.
// ---------------------------------------------------------------------------
class SlangBlobAdapter : public IDxcBlob
{
    Slang::ComPtr<ISlangBlob> _blob;
    ULONG _refCount = 1;
public:
    explicit SlangBlobAdapter(Slang::ComPtr<ISlangBlob> blob) : _blob(std::move(blob)) {}

    LPVOID STDMETHODCALLTYPE GetBufferPointer() override
    { return const_cast<void*>(_blob->getBufferPointer()); }

    SIZE_T STDMETHODCALLTYPE GetBufferSize() override
    { return static_cast<SIZE_T>(_blob->getBufferSize()); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override
    { *ppv = nullptr; return E_NOINTERFACE; }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++_refCount; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --_refCount;
        if (r == 0) delete this;
        return r;
    }
};
#endif // LUNA_ENABLE_SLANG

namespace Luna
{

// Static DXC instances — created once on first use, shared across all pipelines
ComPtr<IDxcUtils>     DX12Pipeline::s_DxcUtils;
ComPtr<IDxcCompiler3> DX12Pipeline::s_DxcCompiler;

#ifdef LUNA_ENABLE_SLANG
Slang::ComPtr<slang::IGlobalSession> DX12Pipeline::s_SlangGlobalSession;

void DX12Pipeline::EnsureSlangInitialized()
{
    if (s_SlangGlobalSession) return;

    SlangResult r = slang::createGlobalSession(s_SlangGlobalSession.writeRef());
    if (SLANG_FAILED(r))
        LUNA_LOG_ERROR("slang::createGlobalSession failed: 0x%08lX", (unsigned long)r);
    else
        LUNA_LOG_INFO("Slang global session created");
}
#endif // LUNA_ENABLE_SLANG

void DX12Pipeline::EnsureDXCInitialized()
{
    if (s_DxcUtils && s_DxcCompiler) return;

    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&s_DxcUtils));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("DxcCreateInstance (IDxcUtils) failed: 0x%08lX", (unsigned long)hr);
        return;
    }

    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&s_DxcCompiler));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("DxcCreateInstance (IDxcCompiler3) failed: 0x%08lX", (unsigned long)hr);
        return;
    }
}

bool DX12Pipeline::Initialize(const ComPtr<ID3D12Device> &device, const std::wstring &vsPath,
                              const std::wstring &psPath, const PipelineStateDesc &desc)
{
    _desc = desc;

    EnsureDXCInitialized();
#ifdef LUNA_ENABLE_SLANG
    EnsureSlangInitialized();
#endif

    // Helper: returns true when a path has a .slang extension
    auto isSlang = [](const std::wstring& p) -> bool {
        return p.size() > 6 && p.substr(p.size() - 6) == L".slang";
    };

    // Helper: load a shader, routing to Slang or DXC based on file extension
    auto loadShader = [&](const std::wstring& fullPath, const std::wstring& target,
                          ComPtr<IDxcBlob>& outBlob) -> bool {
#ifdef LUNA_ENABLE_SLANG
        if (isSlang(fullPath))
            return LoadShaderSlang(fullPath, target, outBlob);
#endif
        return LoadShaderDXC(fullPath, target, outBlob);
    };

    // Phase 12: compute pipeline — only CS, no VS/PS
    if (desc.computeShader)
    {
        ComPtr<IDxcBlob> csBlob;
        std::wstring csFullPath = GetShaderFullPath(vsPath); // reuse vsPath for CS path
        if (!loadShader(csFullPath, L"cs_6_0", csBlob)) return false;
        if (!CreateRootSignature(device)) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC csoDesc = {};
        csoDesc.pRootSignature = _rootSignature.Get();
        csoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
        HRESULT hr = device->CreateComputePipelineState(&csoDesc, IID_PPV_ARGS(&_pipelineState));
        if (FAILED(hr))
        {
            LUNA_LOG_ERROR("CreateComputePipelineState failed: 0x%08lX", (unsigned long)hr);
            return false;
        }
        return true;
    }

    std::wstring vsFullPath = GetShaderFullPath(vsPath);
    if (!loadShader(vsFullPath, L"vs_6_0", _vsBlob)) return false;

    // Phase 8: depth-only passes have no pixel shader
    if (!desc.depthOnlyPass)
    {
        std::wstring psFullPath = GetShaderFullPath(psPath);
        if (!loadShader(psFullPath, L"ps_6_0", _psBlob)) return false;
    }

    if (!CreateRootSignature(device))                    return false;
    if (!CreatePipelineState(device, _vsBlob, _psBlob))  return false;

    return true;
}

// ---------------------------------------------------------------------------
// Phase 2B: DXC compilation — DXIL output (SM 6.0+)
// ---------------------------------------------------------------------------
bool DX12Pipeline::LoadShaderDXC(const std::wstring &path, const std::wstring &target,
                                  ComPtr<IDxcBlob> &outBlob)
{
    if (!s_DxcUtils || !s_DxcCompiler)
    {
        LUNA_LOG_ERROR("DXC not initialized");
        return false;
    }

    // Load source file
    ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT hr = s_DxcUtils->LoadFile(path.c_str(), nullptr, &sourceBlob);
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("DXC: failed to load %ls: 0x%08lX", path.c_str(), (unsigned long)hr);
        return false;
    }

    DxcBuffer srcBuf = {};
    srcBuf.Ptr      = sourceBlob->GetBufferPointer();
    srcBuf.Size     = sourceBlob->GetBufferSize();
    srcBuf.Encoding = DXC_CP_ACP;

    LPCWSTR args[] = {
        path.c_str(),      // filename hint for error messages
        L"-E", L"main",    // entry point
        L"-T", target.c_str(), // target profile (vs_6_0 / ps_6_0)
        L"-HV", L"2021",   // HLSL 2021 semantics
#ifdef _DEBUG
        L"-Zs",            // embed debug info in debug builds
        L"-Od",            // disable optimizer in debug builds
#endif
    };

    ComPtr<IDxcResult> result;
    hr = s_DxcCompiler->Compile(
        &srcBuf,
        args, _countof(args),
        nullptr,            // no include handler (shaders are self-contained)
        IID_PPV_ARGS(&result));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("DXC: Compile() call failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    // Check for compile errors
    HRESULT compileStatus = S_OK;
    result->GetStatus(&compileStatus);

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    if (FAILED(compileStatus))
    {
        if (errors && errors->GetStringLength() > 0)
            LUNA_LOG_ERROR("Shader compile error in %ls:\n%s", path.c_str(), errors->GetStringPointer());
        else
            LUNA_LOG_ERROR("Shader compile failed (no error message): %ls", path.c_str());
        return false;
    }

    if (errors && errors->GetStringLength() > 0)
        LUNA_LOG_WARN("Shader warnings in %ls:\n%s", path.c_str(), errors->GetStringPointer());

    hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&outBlob), nullptr);
    if (FAILED(hr) || !outBlob)
    {
        LUNA_LOG_ERROR("DXC: failed to retrieve shader object for %ls", path.c_str());
        return false;
    }

    return true;
}

#ifdef LUNA_ENABLE_SLANG
// ---------------------------------------------------------------------------
// LoadShaderSlang — compile a .slang file to DXIL using the Slang C API.
// Returns an IDxcBlob-compatible adapter so callers need no changes.
// ---------------------------------------------------------------------------
bool DX12Pipeline::LoadShaderSlang(const std::wstring &path, const std::wstring &target,
                                   ComPtr<IDxcBlob> &outBlob)
{
    if (!s_SlangGlobalSession)
    {
        LUNA_LOG_ERROR("Slang not initialized");
        return false;
    }

    // Read .slang source from disk
    std::ifstream file(path);
    if (!file.is_open())
    {
        LUNA_LOG_ERROR("Slang: cannot open %ls", path.c_str());
        return false;
    }
    std::string source((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    // Narrow path string for Slang APIs
    std::string narrowPath(path.begin(), path.end());

    // Narrow target profile (e.g. L"ps_6_0" -> "ps_6_0")
    std::string narrowTarget(target.begin(), target.end());

    // Build a per-compilation session targeting DXIL at the requested SM profile
    slang::TargetDesc targetDesc  = {};
    targetDesc.format             = SLANG_DXIL;
    targetDesc.profile            = s_SlangGlobalSession->findProfile(narrowTarget.c_str());
    if (targetDesc.profile == SLANG_PROFILE_UNKNOWN)
    {
        LUNA_LOG_ERROR("Slang: unknown profile '%s'", narrowTarget.c_str());
        return false;
    }

    slang::SessionDesc sessionDesc = {};
    sessionDesc.targets            = &targetDesc;
    sessionDesc.targetCount        = 1;

    Slang::ComPtr<slang::ISession> session;
    SlangResult r = s_SlangGlobalSession->createSession(sessionDesc, session.writeRef());
    if (SLANG_FAILED(r))
    {
        LUNA_LOG_ERROR("Slang: createSession failed: 0x%08lX", (unsigned long)r);
        return false;
    }

    // Load module from source string — moduleName is used as a logical identifier
    Slang::ComPtr<ISlangBlob> diagnostics;
    slang::IModule* slangModule = session->loadModuleFromSourceString(
        "deferred_lighting",        // module name
        narrowPath.c_str(),         // path hint for error messages
        source.c_str(),
        diagnostics.writeRef());

    if (diagnostics && diagnostics->getBufferSize() > 0)
        LUNA_LOG_WARN("Slang diagnostics for %ls:\n%s",
                      path.c_str(),
                      static_cast<const char*>(diagnostics->getBufferPointer()));

    if (!slangModule)
    {
        LUNA_LOG_ERROR("Slang: loadModuleFromSourceString failed for %ls", path.c_str());
        return false;
    }

    // Find the "main" entry point
    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    r = slangModule->findEntryPointByName("main", entryPoint.writeRef());
    if (SLANG_FAILED(r) || !entryPoint)
    {
        LUNA_LOG_ERROR("Slang: entry point 'main' not found in %ls", path.c_str());
        return false;
    }

    // Compose module + entry point into a single component
    // entryPoint.get() yields slang::IEntryPoint* which inherits IComponentType*
    slang::IComponentType* components[] = { slangModule, entryPoint.get() };
    Slang::ComPtr<slang::IComponentType> composed;
    diagnostics = nullptr;
    r = session->createCompositeComponentType(components, 2, composed.writeRef(),
                                              diagnostics.writeRef());
    if (diagnostics && diagnostics->getBufferSize() > 0)
        LUNA_LOG_WARN("Slang compose diagnostics:\n%s",
                      static_cast<const char*>(diagnostics->getBufferPointer()));
    if (SLANG_FAILED(r))
    {
        LUNA_LOG_ERROR("Slang: createCompositeComponentType failed: 0x%08lX", (unsigned long)r);
        return false;
    }

    // Link (resolves cross-module references, emits final DXIL)
    Slang::ComPtr<slang::IComponentType> linked;
    diagnostics = nullptr;
    r = composed->link(linked.writeRef(), diagnostics.writeRef());
    if (diagnostics && diagnostics->getBufferSize() > 0)
        LUNA_LOG_WARN("Slang link diagnostics:\n%s",
                      static_cast<const char*>(diagnostics->getBufferPointer()));
    if (SLANG_FAILED(r))
    {
        LUNA_LOG_ERROR("Slang: link failed: 0x%08lX", (unsigned long)r);
        return false;
    }

    // Extract DXIL blob for target index 0
    Slang::ComPtr<ISlangBlob> dxilBlob;
    diagnostics = nullptr;
    r = linked->getTargetCode(0, dxilBlob.writeRef(), diagnostics.writeRef());
    if (diagnostics && diagnostics->getBufferSize() > 0)
        LUNA_LOG_WARN("Slang getTargetCode diagnostics:\n%s",
                      static_cast<const char*>(diagnostics->getBufferPointer()));
    if (SLANG_FAILED(r) || !dxilBlob)
    {
        LUNA_LOG_ERROR("Slang: getTargetCode failed for %ls: 0x%08lX",
                       path.c_str(), (unsigned long)r);
        return false;
    }

    // Wrap Slang DXIL in an IDxcBlob-compatible adapter
    outBlob = new SlangBlobAdapter(dxilBlob);
    return true;
}
#endif // LUNA_ENABLE_SLANG

bool DX12Pipeline::CreateRootSignature(const ComPtr<ID3D12Device> &device)
{
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
    ComPtr<ID3DBlob>            serializedBlob;
    ComPtr<ID3DBlob>            errorBlob;
    HRESULT                     hr;

    if (_desc.rootLayout == RootSignatureLayout::PBR)
    {
        // Phase 11: Bindless PBR root signature
        //   params[0] b0 — MVP transform CBV (vertex + pixel visible)
        //   params[1] b1 — MaterialConstants CBV (pixel visible)
        //   params[2] b2 — 1 DWORD root constant: materialIndex = base SRV slot in heap
        //   params[3]    — Unbounded descriptor table: t0+, space1
        //                  (covers the entire _imGuiSrvHeap; index with materialIndex+0/1/2)
        //   static sampler s0 — anisotropic wrap (pixel-visible)
        //
        // Shader uses: Texture2D<float4> gAllTextures[] : register(t0, space1);
        //              albedo    = gAllTextures[gMaterialIndex + 0].Sample(s0, uv)
        //              normalMap = gAllTextures[gMaterialIndex + 1].Sample(s0, uv)
        //              metalRough= gAllTextures[gMaterialIndex + 2].Sample(s0, uv)
        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);  // b0: MVP
        params[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);// b1: MaterialConstants
        params[2].InitAsConstants(1, 2, 0, D3D12_SHADER_VISIBILITY_PIXEL);      // b2: materialIndex

        // Unbounded SRV array — covers the whole descriptor heap (space1 avoids clash with any
        // explicit t0-t4 bindings in space0 that the lighting pass uses on the same heap)
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1); // t0…, space1
        params[3].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter           = D3D12_FILTER_ANISOTROPIC;
        samplerDesc.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.MaxAnisotropy    = 8;
        samplerDesc.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        samplerDesc.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplerDesc.MinLOD           = 0.0f;
        samplerDesc.MaxLOD           = D3D12_FLOAT32_MAX;
        samplerDesc.ShaderRegister   = 0;   // s0
        samplerDesc.RegisterSpace    = 0;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootSigDesc.Init(4, params, 1, &samplerDesc,
                         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    }
    else if (_desc.rootLayout == RootSignatureLayout::DeferredLighting)
    {
        // Phase 7+9: Deferred Lighting root signature
        //   params[0] b0 — SceneConstants CBV (vertex+pixel visible)
        //   params[1]    — Descriptor table: t0-t4 (GB0/1/2, depth, shadow) — pixel-visible
        //   params[2]    — Descriptor table: t5 (SSAO blur R8_UNORM) — pixel-visible  [Phase 9]
        //   static sampler s0 — point-clamp  (G-buffer reads)
        //   static sampler s1 — bilinear-clamp (SSAO upscale)                         [Phase 9]
        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);  // b0

        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0); // t0-t4
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_DESCRIPTOR_RANGE ssaoRange;
        ssaoRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5); // t5 — SSAO blur
        params[2].InitAsDescriptorTable(1, &ssaoRange, D3D12_SHADER_VISIBILITY_PIXEL);

        // s0 = point-clamp (G-buffer / depth / shadow reads — no interpolation artefacts)
        // s1 = bilinear-clamp (SSAO half-res -> full-res upscale)
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW
                             = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].MaxAnisotropy    = 1;
        samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        samplers[0].MinLOD           = 0.0f;
        samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister   = 0;   // s0
        samplers[0].RegisterSpace    = 0;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        samplers[1]                = samplers[0];
        samplers[1].Filter         = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samplers[1].ShaderRegister = 1;   // s1

        // No input layout — fullscreen triangle uses SV_VertexID
        rootSigDesc.Init(3, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::CSMDepth)
    {
        // Phase 8: CSM depth-only root signature
        //   params[0] — 16 inline root constants at b0 (light-space MVP, 4×4 = 16 DWORDs)
        //   Vertex-only visibility; pixel shader denied for GPU optimization
        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

        D3D12_ROOT_SIGNATURE_FLAGS flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS       |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS     |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS   |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

        rootSigDesc.Init(1, params, 0, nullptr, flags);
    }
    else if (_desc.rootLayout == RootSignatureLayout::SSAO)
    {
        // Phase 9: SSAO root signature
        //   params[0] b0 — SSAOConstants CBV (kernel + matrices + noise scale + radius + bias)
        //   params[1]    — SRV table t0: depth (R32_FLOAT)
        //   params[2]    — SRV table t1: G-buffer1 normal (R16G16B16A16_FLOAT)
        //   params[3]    — SRV table t2: 4×4 noise texture (R8G8_UNORM)
        //   s0           — point-clamp (depth / normal sampling)
        //   s1           — point-wrap  (noise tiling)
        CD3DX12_DESCRIPTOR_RANGE srvRanges[3];
        srvRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0 depth
        srvRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // t1 normal
        srvRanges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2); // t2 noise

        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &srvRanges[0], D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsDescriptorTable(1, &srvRanges[1], D3D12_SHADER_VISIBILITY_PIXEL);
        params[3].InitAsDescriptorTable(1, &srvRanges[2], D3D12_SHADER_VISIBILITY_PIXEL);

        // s0=point-clamp, s1=point-wrap
        CD3DX12_STATIC_SAMPLER_DESC samplers[2];
        samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                         D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                         D3D12_TEXTURE_ADDRESS_MODE_WRAP);

        rootSigDesc.Init(4, params, 2, samplers,
                         D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::SSAOBlur)
    {
        // Phase 9: SSAO blur root signature
        //   params[0] — SRV table t0: raw SSAO (R8_UNORM)
        //   s0        — point-clamp
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0 raw SSAO

        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler;
        sampler.Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        rootSigDesc.Init(1, params, 1, &sampler,
                         D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::TAA)
    {
        // Phase 10: TAA resolve
        //   params[0] b0 — TAAConstants CBV (pixel-visible)
        //   params[1]    — SRV t0: current HDR
        //   params[2]    — SRV t1: history buffer
        //   params[3]    — SRV t2: hardware depth
        //   s0=bilinear-clamp, s1=point-clamp
        CD3DX12_DESCRIPTOR_RANGE ranges[3];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // t1
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2); // t2

        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL);
        params[3].InitAsDescriptorTable(1, &ranges[2], D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC samplers[2];
        samplers[0].Init(0, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
                         D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_POINT,
                         D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        rootSigDesc.Init(4, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::BloomBright)
    {
        // Phase 10: bloom bright-pass
        //   params[0] b0 — 4 inline root constants (threshold, knee, 0, 0)
        //   params[1]    — SRV t0: HDR input
        //   s0=point-clamp
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler;
        sampler.Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        rootSigDesc.Init(2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::BloomBlur)
    {
        // Phase 10: separable Gaussian blur (H and V passes share this root sig)
        //   params[0] b0 — 4 inline root constants (texelStep.xy, 0, 0)
        //   params[1]    — SRV t0: bloom input
        //   s0=bilinear-clamp (avoids seams at edges)
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler;
        sampler.Init(0, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        rootSigDesc.Init(2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::ToneMap)
    {
        // Phase 10: ACES tone mapping + bloom composite
        //   params[0] b0 — 4 inline root constants (bloomStrength, exposure, 0, 0)
        //   params[1]    — SRV t0: TAA-resolved HDR
        //   params[2]    — SRV t1: blurred bloom
        //   s0=point-clamp
        CD3DX12_DESCRIPTOR_RANGE ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // t1

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler;
        sampler.Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        rootSigDesc.Init(3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::GPUCull)
    {
        // Phase 12: GPU frustum cull compute root signature
        //   params[0] b0 — 28 inline root constants (6 frustum planes + objectCount + pad = 28 DWORDs)
        //   params[1]    — SRV t0: StructuredBuffer<GPUObjectData>
        //   params[2]    — SRV t1: StructuredBuffer<MeshDrawInfo>
        //   params[3]    — UAV u0: RWStructuredBuffer<IndirectDrawCommand>
        //   params[4]    — UAV u1: RWByteAddressBuffer (draw count)
        CD3DX12_ROOT_PARAMETER params[5];
        params[0].InitAsConstants(28, 0, 0, D3D12_SHADER_VISIBILITY_ALL);  // b0: CullConstants
        params[1].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL);  // t0: objects
        params[2].InitAsShaderResourceView(1, 0, D3D12_SHADER_VISIBILITY_ALL);  // t1: meshInfo
        params[3].InitAsUnorderedAccessView(0, 0, D3D12_SHADER_VISIBILITY_ALL); // u0: drawArgs
        params[4].InitAsUnorderedAccessView(1, 0, D3D12_SHADER_VISIBILITY_ALL); // u1: drawCount

        rootSigDesc.Init(5, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::PBRIndirect)
    {
        // Phase 12: Indirect PBR G-buffer fill root signature
        //   params[0] b0 — ViewProj CBV (vertex+pixel visible)
        //   params[1] b1 — MaterialConstants CBV (pixel visible)
        //   params[2] b2 — 1 DWORD root constant: materialIndex (pixel visible)
        //   params[3] b3 — 1 DWORD root constant: objectIndex (vertex visible)
        //   params[4]    — SRV t0 space0: StructuredBuffer<GPUObjectData> (vertex visible)
        //   params[5]    — Unbounded SRV table: t0+ space1 (pixel visible, bindless textures)
        //   s0 = static anisotropic sampler
        CD3DX12_ROOT_PARAMETER params[6];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);   // b0: ViewProj
        params[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL); // b1: MaterialConstants
        params[2].InitAsConstants(1, 2, 0, D3D12_SHADER_VISIBILITY_PIXEL);       // b2: materialIndex
        params[3].InitAsConstants(1, 3, 0, D3D12_SHADER_VISIBILITY_VERTEX);      // b3: objectIndex
        params[4].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);// t0 space0: objects SSBO

        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1); // t0…, space1
        params[5].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter           = D3D12_FILTER_ANISOTROPIC;
        samplerDesc.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.MaxAnisotropy    = 8;
        samplerDesc.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        samplerDesc.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplerDesc.MinLOD           = 0.0f;
        samplerDesc.MaxLOD           = D3D12_FLOAT32_MAX;
        samplerDesc.ShaderRegister   = 0;   // s0
        samplerDesc.RegisterSpace    = 0;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootSigDesc.Init(6, params, 1, &samplerDesc,
                         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    }
    else if (_desc.rootLayout == RootSignatureLayout::EquirectToCube)
    {
        // Phase 14: equirectangular -> cubemap compute
        //   params[0] b0 — 4 root constants (faceSize, pad×3)
        //   params[1]    — SRV  t0: equirect Texture2D
        //   params[2]    — UAV  u0: RWTexture2DArray output
        //   s0 = bilinear-clamp
        CD3DX12_DESCRIPTOR_RANGE srvRange, uavRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
        params[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_STATIC_SAMPLER_DESC sampler;
        sampler.Init(0, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootSigDesc.Init(3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::IrradianceConv)
    {
        // Phase 14: irradiance convolution compute
        //   params[0] b0 — 4 root constants (faceSize, pad×3)
        //   params[1]    — SRV  t0: envCube TextureCube
        //   params[2]    — UAV  u0: RWTexture2DArray irradiance output
        //   s0 = trilinear-clamp
        CD3DX12_DESCRIPTOR_RANGE srvRange, uavRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
        params[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_STATIC_SAMPLER_DESC sampler;
        sampler.Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootSigDesc.Init(3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::PrefilterEnv)
    {
        // Phase 14: prefiltered environment map compute
        //   params[0] b0 — 4 root constants (faceSize, mipLevel, roughness as uint bits, sampleCount)
        //   params[1]    — SRV  t0: envCube TextureCube
        //   params[2]    — UAV  u0: RWTexture2DArray prefilter output (specific mip)
        //   s0 = trilinear-clamp
        CD3DX12_DESCRIPTOR_RANGE srvRange, uavRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
        params[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_STATIC_SAMPLER_DESC sampler;
        sampler.Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootSigDesc.Init(3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::BrdfLut)
    {
        // Phase 14: BRDF integration LUT compute
        //   params[0]  — UAV  u0: RWTexture2D<float2> BRDF LUT
        //   no SRV, no CB, no sampler
        CD3DX12_DESCRIPTOR_RANGE uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

        rootSigDesc.Init(1, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::Skybox)
    {
        // Phase 14: Skybox graphics pipeline
        //   params[0] b0 — 16 root constants (row-major invViewProj float4x4), VS + PS visible
        //   params[1]    — SRV t0: TextureCube env cubemap, pixel-visible
        //   s0 = trilinear-clamp, pixel-visible
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler;
        sampler.Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootSigDesc.Init(2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::DeferredLightingIBL)
    {
        // Phase 14: Deferred Lighting with IBL
        //   params[0] b0 — SceneConstants CBV
        //   params[1]    — SRV table t0-t4 (GB0/1/2/depth/shadow)
        //   params[2]    — SRV t5 (SSAO blur)
        //   params[3]    — SRV t6 (irradiance cubemap)
        //   params[4]    — SRV t7 (prefiltered env cubemap)
        //   params[5]    — SRV t8 (BRDF LUT)
        //   s0 = point-clamp, s1 = bilinear-clamp, s2 = trilinear-clamp
        CD3DX12_DESCRIPTOR_RANGE ranges[5];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0); // t0-t4
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5); // t5 SSAO
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6); // t6 irradiance
        ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7); // t7 prefilter
        ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8); // t8 BRDF LUT

        CD3DX12_ROOT_PARAMETER params[6];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
        params[1].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL);
        params[3].InitAsDescriptorTable(1, &ranges[2], D3D12_SHADER_VISIBILITY_PIXEL);
        params[4].InitAsDescriptorTable(1, &ranges[3], D3D12_SHADER_VISIBILITY_PIXEL);
        params[5].InitAsDescriptorTable(1, &ranges[4], D3D12_SHADER_VISIBILITY_PIXEL);

        // s0=point-clamp, s1=bilinear-clamp, s2=trilinear-clamp (IBL)
        D3D12_STATIC_SAMPLER_DESC samplers[3] = {};
        samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW
                             = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].MaxAnisotropy    = 1;
        samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        samplers[0].MinLOD           = 0.0f;
        samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister   = 0;
        samplers[0].RegisterSpace    = 0;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        samplers[1]                = samplers[0];
        samplers[1].Filter         = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samplers[1].ShaderRegister = 1;

        samplers[2]                = samplers[0];
        samplers[2].Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[2].ShaderRegister = 2;

        rootSigDesc.Init(6, params, 3, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::SSRCompute)
    {
        // Phase 16B: SSR compute
        //   params[0] b0 — SSRConstants CBV (256B)
        //   params[1]    — SRV t0: depthTex
        //   params[2]    — SRV t1: normalTex
        //   params[3]    — SRV t2: metalRoughTex
        //   params[4]    — SRV t3: sceneHDR
        //   params[5]    — UAV u0: ssrOut
        //   s0 = point-clamp, s1 = linear-clamp (ALL visible)
        CD3DX12_DESCRIPTOR_RANGE ranges[5];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
        ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
        ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[6];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
        params[1].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);
        params[2].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);
        params[3].InitAsDescriptorTable(1, &ranges[2], D3D12_SHADER_VISIBILITY_ALL);
        params[4].InitAsDescriptorTable(1, &ranges[3], D3D12_SHADER_VISIBILITY_ALL);
        params[5].InitAsDescriptorTable(1, &ranges[4], D3D12_SHADER_VISIBILITY_ALL);

        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW
                             = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].MaxAnisotropy    = 1;
        samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister   = 0;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        samplers[1]                = samplers[0];
        samplers[1].Filter         = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samplers[1].ShaderRegister = 1;

        rootSigDesc.Init(6, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else if (_desc.rootLayout == RootSignatureLayout::SSRBlend)
    {
        // Phase 16B: SSR additive blend pass
        //   params[0] — SRV t0: ssrTex
        //   s0 = point-clamp (PIXEL)
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler;
        sampler.Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootSigDesc.Init(1, params, 1, &sampler,
                         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    }
    else if (_desc.rootLayout == RootSignatureLayout::MotionBlur)
    {
        // Phase 18B: screen-space motion blur
        //   params[0] b0 — MotionBlurCB CBV (pixel-visible)
        //   params[1]    — SRV t0: HDR color
        //   params[2]    — SRV t1: depth
        //   s0 = point-clamp (PIXEL)
        CD3DX12_DESCRIPTOR_RANGE ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // t1

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler;
        sampler.Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootSigDesc.Init(3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    }
    else
    {
        // Default MVP-only root signature
        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsConstantBufferView(0); // b0 — MVP transform
        rootSigDesc.Init(1, params, 0, nullptr,
                         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    }

    hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &serializedBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob)
            LUNA_LOG_ERROR("Root signature serialize error:\n%s",
                           static_cast<const char *>(errorBlob->GetBufferPointer()));
        else
            LUNA_LOG_ERROR("Root signature serialize failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    hr = device->CreateRootSignature(0, serializedBlob->GetBufferPointer(),
                                     serializedBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&_rootSignature));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("CreateRootSignature failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    return true;
}

bool DX12Pipeline::CreatePipelineState(const ComPtr<ID3D12Device>& device,
                                       ComPtr<IDxcBlob> vs, ComPtr<IDxcBlob> ps)
{
    // TriangleVertex: { Vec3 position (12 B), Vec4 color (16 B) } — stride 28 B
    D3D12_INPUT_ELEMENT_DESC triangleLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    // PBRVertex: { pos(12) + normal(12) + uv(8) + tangent(16) } — stride 48 B
    D3D12_INPUT_ELEMENT_DESC pbrLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature     = _rootSignature.Get();
    // IDxcBlob satisfies GetBufferPointer/GetBufferSize — compatible with D3D12_SHADER_BYTECODE
    psoDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    // Phase 8: PS blob is null for depth-only passes
    psoDesc.PS = ps ? D3D12_SHADER_BYTECODE{ps->GetBufferPointer(), ps->GetBufferSize()}
                    : D3D12_SHADER_BYTECODE{nullptr, 0};
    psoDesc.RasterizerState    = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState         = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState  = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask         = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.SampleDesc.Count   = 1;
    psoDesc.NodeMask           = 0;
    psoDesc.Flags              = D3D12_PIPELINE_STATE_FLAG_NONE;

    // Input layout — noInputLayout=true for fullscreen passes that use SV_VertexID
    if (_desc.noInputLayout)
    {
        psoDesc.InputLayout = {nullptr, 0};
        // Fullscreen triangle uses CCW winding — disable culling to ensure it draws
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    }
    else if (_desc.vertexLayout == VertexLayout::PBR)
    {
        psoDesc.InputLayout = {pbrLayout, _countof(pbrLayout)};
    }
    else
    {
        psoDesc.InputLayout = {triangleLayout, _countof(triangleLayout)};
    }

    if (_desc.depthOnlyPass)
    {
        // Phase 8: depth-only PSO (CSM shadow pass) — no RTVs, no PS, depth bias set
        psoDesc.NumRenderTargets = 0;
        psoDesc.PS               = {nullptr, 0};
        psoDesc.DSVFormat        = (_desc.dsvFormat != DXGI_FORMAT_UNKNOWN)
                                   ? _desc.dsvFormat : DXGI_FORMAT_D32_FLOAT;

        // Depth bias reduces shadow acne (self-shadowing artifacts)
        psoDesc.RasterizerState.DepthBias            = 100;
        psoDesc.RasterizerState.DepthBiasClamp       = 0.0f;
        psoDesc.RasterizerState.SlopeScaledDepthBias = 2.0f;
        // Front-face culling: caster back-faces project into shadow map, reducing acne
        psoDesc.RasterizerState.CullMode             = D3D12_CULL_MODE_FRONT;

        psoDesc.DepthStencilState.DepthEnable    = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    }
    else
    {
        // MRT support — use desc fields instead of hardcoded 1 RT
        psoDesc.NumRenderTargets = _desc.numRenderTargets;
        for (UINT i = 0; i < _desc.numRenderTargets; ++i)
            psoDesc.RTVFormats[i] = _desc.rtvFormats[i];

        if (_desc.enableWireFrame)
            psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

        if (_desc.rootLayout == RootSignatureLayout::Skybox)
        {
            // Phase 14: Skybox draws at depth=1.0 (far plane) using LESS_EQUAL so it renders
            // only where no geometry wrote depth. Depth write must be OFF so it doesn't clobber
            // the depth buffer for downstream passes.
            psoDesc.DepthStencilState.DepthEnable    = TRUE;
            psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        }
        else if (_desc.rootLayout == RootSignatureLayout::SSRBlend)
        {
            // Phase 16B: Additive blend SSR onto _hdrRT; no depth test/write
            psoDesc.DepthStencilState.DepthEnable = FALSE;
            auto& rt = psoDesc.BlendState.RenderTarget[0];
            rt.BlendEnable           = TRUE;
            rt.SrcBlend              = D3D12_BLEND_ONE;
            rt.DestBlend             = D3D12_BLEND_ONE;
            rt.BlendOp               = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
            rt.DestBlendAlpha        = D3D12_BLEND_ZERO;
            rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            psoDesc.DSVFormat        = DXGI_FORMAT_UNKNOWN;
        }
        else
        {
            psoDesc.DepthStencilState.DepthEnable = _desc.enableDepthTest ? TRUE : FALSE;
            // Fullscreen passes have no depth buffer — DSVFormat must be UNKNOWN
            psoDesc.DSVFormat = (_desc.enableDepthTest && !_desc.noInputLayout)
                                ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        }
    }

    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_pipelineState));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("CreateGraphicsPipelineState failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    return true;
}

} // namespace Luna
