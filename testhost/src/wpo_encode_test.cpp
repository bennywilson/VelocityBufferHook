// Synthetic WPO-encode round-trip test (NEXT_PROMPT.md item 3).
//
// WHY THIS EXISTS
//
// The analytical reprojection (tools/reproject.py) validates decode
// correctness only on STATIC geometry, by construction: it selects pixels
// whose depth-motion matches pure camera motion, which is exactly how it
// excludes wind-animated foliage (world-position-offset geometry) from the
// test. That leaves two failure modes conflated on WPO pixels - "the decode
// is wrong there" vs. "the analytical reference simply cannot predict WPO
// motion, which is expected" - with nothing in the repo separating them.
//
// WHAT THIS DOES, AND WHY THAT IS ENOUGH
//
// The buffer's encode/decode format has no idea where a pixel's motion came
// from. EncodeVelocityToTexture() (Common.ush:2060) takes a clip-space delta
// and packs it; it does not care whether that delta was produced by camera
// motion, a skinned mesh, or a WPO-animated tree. So decode correctness is
// source-agnostic, and a full re-implementation of Calculate3DVelocityBase's
// camera-projection math (which reproject.py already has, and already
// validates independently on static geometry) is not needed to test it.
// What IS needed is: known clip-space velocities, run through the REAL
// encode - same macros, same GPU hardware UNORM16 quantization SceneVelocity
// itself goes through - then decoded by the unmodified, already-shipping
// mvtools.py and checked against the known input.
//
// This renders a full-screen triangle whose pixel shader computes a
// deterministic (px, py) -> (Vx, Vy, Vz) grid - decorrelated from anything a
// camera-only prediction would produce at that screen position, and swept to
// |V| = 2.0, just inside the buffer's own documented 2.008 clip-unit ceiling
// (DEBUGGING.md, "A moderate-speed capture, and the extreme-speed degradation
// resolved") - so the sweep spans the encoding's nonlinear range without
// reaching saturation, which this test does not cover -
// encodes it via EncodeVelocityToTexture()/VELOCITY_ENCODE_DEPTH transcribed
// exactly as Common.ush has them (mirroring mvtools.py's own transcription),
// and writes the result to a real R16G16B16A16_UNORM render target. The
// bytes are dumped in the same meta.txt/vel_NNNNN.bin shape a real capture
// uses, so tools/wpo_synthetic_test.py can run mvtools.decode_velocity_clip()
// and decode_velocity_depth() - unmodified - against it and compare the
// decoded values to the independently-known true grid.
//
// SCOPE NOTE: this is a round-trip test of the encode/decode format on a
// value population representative of WPO pixels, not a simulation of WPO
// vertex geometry moving through a real velocity pass. Building the latter
// (a vertex shader that displaces a mesh by a time-varying WPO function,
// differences two frames' projected clip positions, and lets THAT drive the
// encode) was judged out of scope for this phase - see NEXT_PROMPT.md and
// DEBUGGING.md for why the narrower test already answers the question that
// was actually open.

#include <d3d12.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// Grid size. Deliberately unrelated to the velocity buffer's own resolution -
// this is a test pattern, not a rendered scene, and keeping it small keeps the
// readback and the offline comparison fast. 256 columns give the X sweep a
// reasonable number of distinct encoded values to cross without making the
// dump large.
constexpr UINT kWidth = 256;
constexpr UINT kHeight = 64;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_UNORM;

// Must match tools/wpo_synthetic_test.py's VX_RANGE/VY_RANGE/VZ_RANGE exactly
// - the offline tool regenerates this same grid independently to know what
// SHOULD have been encoded, and the two sides have to agree on the formula
// for the comparison to mean anything. Nothing enforces that agreement
// automatically; changing a range here means changing it there too.
const char* kShaderSource = R"HLSL(
struct VSOut { float4 pos : SV_POSITION; };

VSOut VSMain(uint id : SV_VertexID)
{
    // Fullscreen triangle - no vertex buffer needed.
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(float4 pos : SV_POSITION) : SV_Target
{
    float2 uv = (pos.xy - 0.5) / float2(WIDTH, HEIGHT);

    // The known "true" clip-space velocity this pixel is asked to encode.
    // Swept across [-2, 2] in both axes, which covers most of the encoding's
    // usable range without reaching its edge: the buffer's ceiling is |V| =
    // 2.008 (the V that encodes to exactly 1.0), and the largest magnitude
    // here is 2.0. Encoded values therefore land in roughly [0.001, 0.997]
    // and NOTHING in this grid saturates - the clamp behaviour at the ceiling
    // is deliberately not what this test covers. What it does cover is the
    // full nonlinear span of the sqrt encoding, where one UNORM16 step means
    // very different things at the two ends.
    float2 V;
    V.x = lerp(-2.0, 2.0, uv.x);
    V.y = lerp(-2.0, 2.0, uv.y);
    // DeviceZ deltas are ~1e-6 magnitude on a real capture (see mvtools.py) -
    // swept over the same order of magnitude here.
    float Vz = lerp(-1e-5, 1e-5, uv.x * uv.y);

    // EncodeVelocityToTexture(), Common.ush:2060 - transcribed exactly.
    float2 Vg = sign(V) * sqrt(abs(V)) * (2.0 / sqrt(2.0));
    float2 EncodedXY = Vg * (0.499f * 0.5f) + 32767.0f / 65535.0f;

    // VELOCITY_ENCODE_DEPTH, Common.ush:2071. UE 5.7.1 steals channel 3's
    // bottom bit for bHasPixelAnimation (VELOCITY_Z_LOW_MASK = 0xFFFE) - the
    // same convention mvtools.py's depth_encoding() expects for (5, 7).
    uint zbits = asuint(Vz);
    uint hi = (zbits >> 16) & 0xFFFFu;
    uint animBit = (uint(pos.x) ^ uint(pos.y)) & 1u;
    uint lo = (zbits & 0xFFFEu) | animBit;

    return float4(EncodedXY, hi / 65535.0, lo / 65535.0);
}
)HLSL";

bool Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        printf("[wpo-encode-test] FAILED %s: 0x%08lX\n", what, static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool WriteFileBytes(const std::string& path, const void* data, size_t bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return f.good();
}

} // namespace

// Self-contained: its own device, no window or swapchain (an offscreen
// render target needs neither). Returns 0 on success.
int RunWpoEncodeTest(const std::string& outputDir) {
    printf("[wpo-encode-test] %ux%u synthetic velocity grid -> %s\n", kWidth, kHeight, outputDir.c_str());

    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    }

    // No DXGI factory needed: D3D12CreateDevice(nullptr, ...) below takes the
    // default adapter directly, and this test has no swapchain to create.
    ComPtr<ID3D12Device> device;
    if (!Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice")) {
        return 1;
    }
    ComPtr<ID3D12InfoQueue> infoQueue;
    device.As(&infoQueue);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (!Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue")) {
        return 1;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    if (!Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
               "CreateCommandAllocator")) {
        return 1;
    }
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    if (!Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&cmdList)),
               "CreateCommandList")) {
        return 1;
    }

    // No resources bound - the shader is fully procedural, driven only by
    // SV_VertexID and SV_Position.
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> rootBlob, rootError;
    if (!Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootError),
               "D3D12SerializeRootSignature")) {
        if (rootError) {
            printf("[wpo-encode-test] %s\n", static_cast<const char*>(rootError->GetBufferPointer()));
        }
        return 1;
    }
    ComPtr<ID3D12RootSignature> rootSignature;
    if (!Check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                            IID_PPV_ARGS(&rootSignature)),
               "CreateRootSignature")) {
        return 1;
    }

    char widthText[16], heightText[16];
    snprintf(widthText, sizeof(widthText), "%u.0", kWidth);
    snprintf(heightText, sizeof(heightText), "%u.0", kHeight);
    const D3D_SHADER_MACRO macros[] = {
        {"WIDTH", widthText},
        {"HEIGHT", heightText},
        {nullptr, nullptr},
    };

    ComPtr<ID3DBlob> vsBlob, psBlob, error;
    HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "wpo_encode_test", macros, nullptr, "VSMain",
                             "vs_5_0", 0, 0, &vsBlob, &error);
    if (FAILED(hr)) {
        printf("[wpo-encode-test] VS compile failed: %s\n",
               error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown");
        return 1;
    }
    hr = D3DCompile(kShaderSource, strlen(kShaderSource), "wpo_encode_test", macros, nullptr, "PSMain", "ps_5_0", 0,
                     0, &psBlob, &error);
    if (FAILED(hr)) {
        printf("[wpo-encode-test] PS compile failed: %s\n",
               error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown");
        return 1;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature.Get();
    pso.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    pso.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kFormat;
    pso.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipelineState;
    if (!Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipelineState)),
               "CreateGraphicsPipelineState")) {
        return 1;
    }

    // The render target itself.
    D3D12_HEAP_PROPERTIES rtHeapProps{};
    rtHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = kWidth;
    rtDesc.Height = kHeight;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = kFormat;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> renderTarget;
    if (!Check(device->CreateCommittedResource(&rtHeapProps, D3D12_HEAP_FLAG_NONE, &rtDesc,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                IID_PPV_ARGS(&renderTarget)),
               "CreateCommittedResource(render target)")) {
        return 1;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (!Check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap(RTV)")) {
        return 1;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(renderTarget.Get(), nullptr, rtv);

    // Readback buffer, sized from the real GetCopyableFootprints answer - the
    // same row-pitch-padding the offline tools already know how to strip
    // (mvtools.py's _rows()), rather than assuming a tight packing.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&rtDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

    D3D12_HEAP_PROPERTIES rbHeapProps{};
    rbHeapProps.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = totalBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (!Check(device->CreateCommittedResource(&rbHeapProps, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
               "CreateCommittedResource(readback)")) {
        return 1;
    }

    // Record: draw the full-screen triangle, barrier to COPY_SOURCE, copy out.
    D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    cmdList->SetGraphicsRootSignature(rootSignature.Get());
    cmdList->SetPipelineState(pipelineState.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER toCopySrc{};
    toCopySrc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopySrc.Transition.pResource = renderTarget.Get();
    toCopySrc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toCopySrc.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCopySrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &toCopySrc);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = renderTarget.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (!Check(cmdList->Close(), "cmdList->Close")) {
        return 1;
    }
    ID3D12CommandList* lists[] = {cmdList.Get()};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    if (!Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence")) {
        return 1;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
    }
    CloseHandle(event);

    // Debug-layer errors are exactly the thing an untested render path (no
    // vertex buffer, no bound resources at all) would trip if the root
    // signature or PSO were subtly wrong - the same discipline
    // mv_testhost's main harness applies elsewhere in this project.
    int errors = 0;
    if (infoQueue) {
        const UINT64 count = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T length = 0;
            infoQueue->GetMessage(i, nullptr, &length);
            std::vector<uint8_t> buffer(length);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
            if (SUCCEEDED(infoQueue->GetMessage(i, message, &length)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                printf("[debug-layer] %.*s\n", static_cast<int>(message->DescriptionByteLength),
                       message->pDescription);
                ++errors;
            }
        }
        infoQueue->ClearStoredMessages();
    }

    void* mapped = nullptr;
    if (!Check(readback->Map(0, nullptr, &mapped), "readback->Map")) {
        return 1;
    }
    CreateDirectoryA(outputDir.c_str(), nullptr);
    const std::string binPath = outputDir + "\\wpo_vel_00000.bin";
    const bool wroteBin = WriteFileBytes(binPath, mapped, static_cast<size_t>(totalBytes));
    readback->Unmap(0, nullptr);
    if (!wroteBin) {
        printf("[wpo-encode-test] failed to write %s\n", binPath.c_str());
        return 1;
    }

    std::string meta;
    meta += "velocity_width=" + std::to_string(kWidth) + "\n";
    meta += "velocity_height=" + std::to_string(kHeight) + "\n";
    meta += "velocity_row_pitch=" + std::to_string(footprint.Footprint.RowPitch) + "\n";
    meta += "velocity_format=" + std::to_string(static_cast<int>(kFormat)) + "\n";
    meta += "engine_version_major=5\n";
    meta += "engine_version_minor=7\n";
    if (!WriteFileBytes(outputDir + "\\meta.txt", meta.data(), meta.size())) {
        printf("[wpo-encode-test] failed to write meta.txt\n");
        return 1;
    }

    printf("[wpo-encode-test] wrote %llu bytes, rowPitch=%u, %d debug-layer error(s)\n",
           static_cast<unsigned long long>(totalBytes), footprint.Footprint.RowPitch, errors);
    printf("[wpo-encode-test] next: python tools/wpo_synthetic_test.py \"%s\"\n", outputDir.c_str());
    return errors == 0 ? 0 : 1;
}
