#include "overlay.h"

#include "logging.h"

#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace mv
{
namespace
{

// Frames of command-list resources in flight. If none is free the overlay is
// skipped for that frame rather than waiting.
constexpr UINT kFrames = 3;

// Slots in the SRV heap, matching t0/t1/t2 in the overlay shader.
constexpr UINT kSrvSceneColor = 0;
constexpr UINT kSrvVelocity = 1;
constexpr UINT kSrvCount = 2;

// Opacity of the velocity colours when composited over the live frame.
constexpr float kOverGameplayBlend = 0.75f;

// Overlay shader source
const char* kShaderSource = R"HLSL(
Texture2D<float4> SceneColor : register(t0);
Texture2D<float4> Velocity   : register(t1);
SamplerState Samp            : register(s0);

cbuffer Params : register(b0)
{
    float2 VelocityRes;     // velocity texture dimensions, in pixels
    float  Mode;            // 1 = velocity only, 2 = over gameplay
    float  Blend;           // opacity when composited over gameplay
    float  Stale;           // 1 = the velocity texture was not refreshed for this frame
    float3 Pad;
};

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    // Fullscreen triangle - no vertex buffer needed.
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 scene = SceneColor.SampleLevel(Samp, i.uv, 0).rgb;

    // The velocity texture holds whatever was last copied into it, and the copy
    // only happens on the identified resource's RENDER_TARGET -> shader-resource
    // barrier. On a frame where that barrier does not occur there is no new
    // velocity, so draw an oncreen indicator and skip.
    if (Stale > 0.5)
    {
        bool border = i.uv.x < 0.004 || i.uv.x > 0.996 || i.uv.y < 0.007 || i.uv.y > 0.993;
        float3 base = (Mode < 1.5) ? 0.0.xxx : scene;
        return float4(border ? float3(0.55, 0.0, 0.0) : base, 1.0);
    }

    // Point-sample: the velocity buffer is at render resolution while the back
    // buffer is at output resolution, and filtering across the edge of a
    // written region would blend real values with the cleared value, inventing
    // motion that is not there.
    float2 vres = max(VelocityRes, 1.0.xx);
    float2 texel = (floor(i.uv * vres) + 0.5) / vres;
    float3 raw = Velocity.SampleLevel(Samp, texel, 0).rgb;

    // Unwritten pixels are exactly 0, which is not "no motion" - zero motion
    // encodes near 0.5 - so they are masked rather than shown.
    bool written = any(raw.rg > 0.0);

    // Channels straight to RGB, exactly as the engine's visualizetexture does.
    // Channel 2 carries no displacement: under VELOCITY_ENCODE_DEPTH it holds
    // the HIGH 16 bits of the float32 DeviceZ delta (Common.ush:2071), with
    // the low 16 in channel 3. That is why it looks bimodal (~0.214 / ~0.713)
    // - the two modes are the sign bit of that float, not two kinds of motion
    // - and it is what separates the engine's olive and lavender colour
    // families. Shown rather than dropped so this view matches the engine's,
    // but nothing here should be read as a third motion axis.
    if (Mode < 1.5)
    {
        return float4(written ? raw : 0.0.xxx, 1.0);
    }
    return float4(written ? lerp(scene, raw, Blend) : scene, 1.0);
}
)HLSL";

struct FrameResources
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    UINT64 fenceValue = 0;
};

std::mutex g_mutex;
std::atomic<int> g_mode{0};

// Joinable + stop flag, then detach()ed, never joined: this only runs from
// DLL_PROCESS_DETACH, after Windows has already killed the thread. Contrast
// capture.cpp's StopCaptureHotkeyThread, which joins a genuinely live thread
// via MvFlushCapture.
std::atomic<bool> g_hotkeyStop{false};
std::thread g_hotkeyThread;

ResourceBarrierFn g_originalResourceBarrier = nullptr;
ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pipelineState;
ComPtr<ID3D12DescriptorHeap> g_srvHeap;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
ComPtr<ID3D12Fence> g_fence;
UINT64 g_nextFenceValue = 1;
UINT g_frame = 0;
FrameResources g_frames[kFrames];

// Which presented frame g_velocityTex's contents were copied on, and whether
// anything has been copied into it at all.
uint64_t g_velocityCopyFrame = 0;
bool g_velocityCopyValid = false;

// A copy recorded during frame N is stamped N, and OverlayOnPresent for frame N
// runs before the frame counter advances, so the expected age is 0. One frame of
// slack is allowed so that an engine which presents at a different point in its
// own frame does not blank the overlay permanently
constexpr uint64_t kMaxAcceptableCopyAge = 1;

// Freshness instrumentation. mv_candidates.log can't answer "how often is
// there no new velocity?" - its batched, drop-tolerant logging loses records,
// so counted here instead, where the draw decision is actually made.
uint64_t g_freshCount = 0;
uint64_t g_staleCount = 0;
uint64_t g_staleRun = 0;
uint64_t g_staleRunMax = 0;
uint64_t g_maxCopyAge = 0;
uint64_t g_freshnessWindow = 0;
constexpr uint64_t kFreshnessLogInterval = 300;

ComPtr<ID3D12Resource> g_sceneColorTex;
ComPtr<ID3D12Resource> g_velocityTex;

// The descs those two were created from. CopyResource requires source and
// destination to be fully compatible, so a resolution change - dynamic
// resolution scaling on the velocity buffer, a window resize on the back
// buffer - turns the copy below into device removal rather than a wrong
// picture. Both are therefore compared every frame instead of sized once.
D3D12_RESOURCE_DESC g_sceneColorDesc{};
D3D12_RESOURCE_DESC g_velocityDesc{};
UINT g_velocityWidth = 0;
UINT g_velocityHeight = 0;

bool SameLayout(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
    return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count;
}

UINT g_srvDescriptorSize = 0;
UINT g_rtvDescriptorSize = 0;
bool g_initialised = false;
bool g_initFailed = false;

D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * g_srvDescriptorSize;
    return h;
}

// A null SRV keeps the shader well-defined before a source has been captured -
// sampling an unpopulated slot returns zero rather than reading garbage.
void CreateNullSrv(UINT index)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(nullptr, &desc, SrvCpu(index));
}

void CreateSrvFor(ID3D12Resource* resource, DXGI_FORMAT format, UINT index)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(resource, &desc, SrvCpu(index));
}

// Creates a texture matching a source resource's layout so CopyResource can be
// used directly. Deliberately drops the source's RENDER_TARGET/DEPTH_STENCIL
// flags: this copy is only ever read.
bool CreateMatchingTexture(const D3D12_RESOURCE_DESC& sourceDesc, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = sourceDesc;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    return SUCCEEDED(g_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr,
        IID_PPV_ARGS(&out)));
}

bool CompileShaders(ComPtr<ID3DBlob>& vs, ComPtr<ID3DBlob>& ps)
{
    ComPtr<ID3DBlob> error;
    UINT flags = 0;
    HRESULT hr = D3DCompile(
        kShaderSource, strlen(kShaderSource), "overlay", nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vs, &error);
    if (FAILED(hr))
    {
        Log(std::string("overlay: VS compile failed: ") +
            (error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown"));
        return false;
    }
    hr = D3DCompile(
        kShaderSource, strlen(kShaderSource), "overlay", nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &ps, &error);
    if (FAILED(hr))
    {
        Log(std::string("overlay: PS compile failed: ") +
            (error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown"));
        return false;
    }
    return true;
}

bool CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kSrvCount;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 8;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr))
    {
        Log(std::string("overlay: SerializeRootSignature failed: ") +
            (error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown"));
        return false;
    }
    hr = g_device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));
    return SUCCEEDED(hr);
}

bool Initialise(ID3D12Device* device, DXGI_FORMAT backBufferFormat)
{
    g_device = device;

    if (!CreateRootSignature())
    {
        return false;
    }

    ComPtr<ID3DBlob> vs, ps;
    if (!CompileShaders(vs, ps))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = backBufferFormat;
    pso.SampleDesc.Count = 1;

    if (FAILED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineState))))
    {
        Log("overlay: CreateGraphicsPipelineState failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = kSrvCount;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap))))
    {
        return false;
    }
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // One RTV per in-flight frame, not one shared slot. A descriptor must stay
    // valid until every command list referencing it has finished executing,
    // and with kFrames submissions in flight and no wait between them, writing
    // a new back buffer's RTV into a single slot every frame overwrites a
    // descriptor the GPU may still be reading. It happened to work here
    // because the three back buffers are identical in every respect except
    // address, so the stale descriptor described a compatible resource - a
    // coincidence of this swapchain's configuration, not a guarantee, and the
    // debug layer flags it.
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kFrames;
    if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap))))
    {
        return false;
    }
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < kSrvCount; ++i)
    {
        CreateNullSrv(i);
    }

    for (UINT i = 0; i < kFrames; ++i)
    {
        if (FAILED(
                g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].allocator))))
        {
            return false;
        }
        if (FAILED(g_device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                g_frames[i].allocator.Get(),
                nullptr,
                IID_PPV_ARGS(&g_frames[i].commandList))))
        {
            return false;
        }
        g_frames[i].commandList->Close();
    }

    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
    {
        return false;
    }

    Log("overlay: initialised");
    return true;
}

// Blocks until every in-flight overlay submission has retired. Only used
// on the rare recreate paths below: a texture referenced by an executing
// command list must not be released.
void WaitForOverlayIdle()
{
    if (!g_fence)
    {
        return;
    }

    UINT64 target = 0;
    for (const FrameResources& f : g_frames)
    {
        target = f.fenceValue > target ? f.fenceValue : target;
    }

    if (target == 0 || g_fence->GetCompletedValue() >= target)
    {
        return;
    }

    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
    {
        return;
    }
    if (SUCCEEDED(g_fence->SetEventOnCompletion(target, event)))
    {
        WaitForSingleObject(event, 1000);
    }
    CloseHandle(event);
}

void Transition(
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    if (before == after)
    {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    if (g_originalResourceBarrier != nullptr)
    {
        g_originalResourceBarrier(list, 1, &b);
    }
    else
    {
        list->ResourceBarrier(1, &b);
    }
}

} // namespace

bool OverlayActive()
{
    return g_mode.load(std::memory_order_relaxed) != 0;
}

void OverlaySetOriginals(ResourceBarrierFn resourceBarrier, ExecuteCommandListsFn executeCommandLists)
{
    g_originalResourceBarrier = resourceBarrier;
    g_originalExecuteCommandLists = executeCommandLists;
}

void StartOverlayHotkeyThread()
{
    if (g_hotkeyThread.joinable())
    {
        return;
    }
    g_hotkeyThread = std::thread(
        []
        {
            bool wasDown = false;
            while (!g_hotkeyStop.load(std::memory_order_relaxed))
            {
                const bool isDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
                if (isDown && !wasDown)
                {
                    const int next = (g_mode.load() + 1) % static_cast<int>(OverlayMode::Count);
                    g_mode.store(next);
                    // Must match OverlayMode in overlay.h, including its count.
                    // These previously still listed the removed flow-coloured
                    // view, so mode 1 logged itself as "flow colours" while
                    // actually drawing the raw channel mapping.
                    static_assert(static_cast<int>(OverlayMode::Count) == 3, "kNames is out of sync with OverlayMode");
                    static const char* kNames[] = {
                        "normal", "SceneVelocity (raw channels, engine view)", "SceneVelocity over gameplay"};
                    Log(std::string("overlay: mode -> ") + kNames[next]);
                }
                wasDown = isDown;
                Sleep(30);
            }
        });
    Log("overlay: hotkey thread started (F7 = cycle SceneVelocity views)");
}

void StopOverlayHotkeyThread()
{
    g_hotkeyStop.store(true, std::memory_order_relaxed);
    if (!g_hotkeyThread.joinable())
    {
        return;
    }
    // Already terminated by Windows; detach so the destructor does not call
    // std::terminate(). See the same pair in StopCaptureHotkeyThread.
    g_hotkeyThread.detach();
}

void OverlayCaptureVelocity(
    ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source, D3D12_RESOURCE_STATES stateBefore, uint64_t frame)
{
    if (g_mode.load(std::memory_order_relaxed) == 0 || cmdList == nullptr || source == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialised)
    {
        return;
    }

    ComPtr<ID3D12Resource>& dest = g_velocityTex;
    const D3D12_RESOURCE_DESC desc = source->GetDesc();

    // Recreate whenever the source layout changes, not just on the first call.
    if (dest == nullptr || !SameLayout(g_velocityDesc, desc))
    {
        if (dest != nullptr)
        {
            Log("overlay: velocity layout changed " + std::to_string(g_velocityDesc.Width) + "x" +
                std::to_string(g_velocityDesc.Height) + " -> " + std::to_string(desc.Width) + "x" +
                std::to_string(desc.Height) + "; recreating copy texture");
            WaitForOverlayIdle();
            dest.Reset();
        }
        if (!CreateMatchingTexture(desc, dest))
        {
            Log("overlay: failed to create velocity copy texture");
            return;
        }

        g_velocityDesc = desc;
        g_velocityWidth = static_cast<UINT>(desc.Width);
        g_velocityHeight = desc.Height;
        CreateSrvFor(dest.Get(), desc.Format, kSrvVelocity);
        Log("overlay: velocity copy texture created " + std::to_string(g_velocityWidth) + "x" +
            std::to_string(g_velocityHeight) + " fmt=" + std::to_string(static_cast<int>(desc.Format)));
    }

    Transition(cmdList, dest.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(cmdList, source, stateBefore, D3D12_RESOURCE_STATE_COPY_SOURCE);

    // Identical descs, so a whole-resource copy handles every plane and mip w/o
    // per-subresource book keeping
    cmdList->CopyResource(dest.Get(), source);

    Transition(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, stateBefore);
    Transition(cmdList, dest.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    g_velocityCopyFrame = frame;
    g_velocityCopyValid = true;
}

void OverlayOnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, uint64_t frame)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == 0 || !swapChain || !queue)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initFailed)
    {
        return;
    }

    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))))
    {
        return;
    }
    ComPtr<ID3D12Resource> backBuffer;
    const UINT index = swapChain3->GetCurrentBackBufferIndex();
    if (FAILED(swapChain3->GetBuffer(index, IID_PPV_ARGS(&backBuffer))) || backBuffer == nullptr)
    {
        return;
    }
    const D3D12_RESOURCE_DESC backDesc = backBuffer->GetDesc();

    // A back-buffer format change invalidates the pipeline state's RTV format
    // too so tear the overlay down and rebuild next frame.
    if (g_initialised && g_sceneColorTex != nullptr && backDesc.Format != g_sceneColorDesc.Format)
    {
        Log("overlay: back buffer format changed, reinitialising");
        WaitForOverlayIdle();
        g_sceneColorTex.Reset();
        g_velocityTex.Reset();
        g_pipelineState.Reset();
        g_rootSignature.Reset();
        g_srvHeap.Reset();
        g_rtvHeap.Reset();
        for (FrameResources& f : g_frames)
        {
            f.commandList.Reset();
            f.allocator.Reset();
            f.fenceValue = 0;
        }
        g_fence.Reset();
        g_initialised = false;
    }

    if (!g_initialised)
    {
        ComPtr<ID3D12Device> device;
        if (FAILED(backBuffer->GetDevice(IID_PPV_ARGS(&device))))
        {
            return;
        }
        if (!Initialise(device.Get(), backDesc.Format))
        {
            g_initFailed = true;
            Log("overlay: initialisation failed, overlay disabled");
            return;
        }
        g_initialised = true;
    }

    if (g_sceneColorTex == nullptr || !SameLayout(g_sceneColorDesc, backDesc))
    {
        if (g_sceneColorTex != nullptr)
        {
            Log("overlay: back buffer resized to " + std::to_string(backDesc.Width) + "x" +
                std::to_string(backDesc.Height) + "; recreating snapshot texture");
            WaitForOverlayIdle();
            g_sceneColorTex.Reset();
        }
        if (!CreateMatchingTexture(backDesc, g_sceneColorTex))
        {
            return;
        }
        g_sceneColorDesc = backDesc;
        CreateSrvFor(g_sceneColorTex.Get(), backDesc.Format, kSrvSceneColor);
    }

    // Is the velocity texture actually this frame's? Computed before the slot
    // check below so the freshness counters measure the game, not hook's own frame
    // pacing
    const uint64_t copyAge =
        (g_velocityCopyValid && frame >= g_velocityCopyFrame) ? (frame - g_velocityCopyFrame) : UINT64_MAX;
    const bool stale = !g_velocityCopyValid || copyAge > kMaxAcceptableCopyAge;

    if (stale)
    {
        ++g_staleCount;
        ++g_staleRun;
        if (g_staleRun > g_staleRunMax)
        {
            g_staleRunMax = g_staleRun;
        }
    }
    else
    {
        ++g_freshCount;
        g_staleRun = 0;
    }
    if (copyAge != UINT64_MAX && copyAge > g_maxCopyAge)
    {
        g_maxCopyAge = copyAge;
    }

    if (++g_freshnessWindow >= kFreshnessLogInterval)
    {
        Log("overlay: velocity freshness over " + std::to_string(g_freshnessWindow) + " presents - " +
            std::to_string(g_freshCount) + " drawn, " + std::to_string(g_staleCount) +
            " withheld as stale, longest stale run " + std::to_string(g_staleRunMax) + " frames, max copy age " +
            std::to_string(g_maxCopyAge) + " frames");
        g_freshnessWindow = 0;
        g_freshCount = 0;
        g_staleCount = 0;
        g_staleRunMax = 0;
        g_maxCopyAge = 0;
    }

    // Pick a frame slot whose previous submission has completed. Don't stall render thread
    const UINT slotIndex = g_frame % kFrames;
    FrameResources& fr = g_frames[slotIndex];
    if (fr.fenceValue != 0 && g_fence->GetCompletedValue() < fr.fenceValue)
    {
        return;
    }
    g_frame++;

    if (FAILED(fr.allocator->Reset()) || FAILED(fr.commandList->Reset(fr.allocator.Get(), nullptr)))
    {
        return;
    }

    ID3D12GraphicsCommandList* list = fr.commandList.Get();

    // Snapshot the finished frame before we draw over it.
    Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list, g_sceneColorTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(g_sceneColorTex.Get(), backBuffer.Get());
    Transition(list, g_sceneColorTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Composite the grid over the whole back buffer, through this slot's own
    // RTV so the descriptor outlives the command list that references it.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(slotIndex) * g_rtvDescriptorSize;
    g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(backDesc.Width), static_cast<float>(backDesc.Height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(backDesc.Width), static_cast<LONG>(backDesc.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(g_rootSignature.Get());
    list->SetPipelineState(g_pipelineState.Get());
    list->SetGraphicsRootDescriptorTable(0, g_srvHeap->GetGPUDescriptorHandleForHeapStart());

    const float constants[8] = {
        static_cast<float>(g_velocityWidth ? g_velocityWidth : 1),
        static_cast<float>(g_velocityHeight ? g_velocityHeight : 1),
        static_cast<float>(mode),
        kOverGameplayBlend,
        stale ? 1.0f : 0.0f,
        0.0f,
        0.0f,
        0.0f,
    };
    list->SetGraphicsRoot32BitConstants(1, 8, constants, 0);

    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    if (FAILED(list->Close()))
    {
        return;
    }
    ID3D12CommandList* lists[] = {list};
    if (g_originalExecuteCommandLists != nullptr)
    {
        g_originalExecuteCommandLists(queue, 1, lists);
    }
    else
    {
        queue->ExecuteCommandLists(1, lists);
    }

    fr.fenceValue = g_nextFenceValue++;
    queue->Signal(g_fence.Get(), fr.fenceValue);
}

} // namespace mv
