#include "view_cb.h"

#include "logging.h"

#include <windows.h>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace mv
{
namespace
{

// ---------------------------------------------------------------------------
// Counting which constant buffers get bound, cheaply.
//
// Called for every uniform buffer of every draw, from several render threads
// at once, so this is a fixed-size open-addressed table of atomics: no lock,
// no allocation. Collisions and lost increments are acceptable - the only
// question is which address dominates, and the View uniform buffer is bound
// by nearly every draw, so it doesn't need to be exact.
//
// One exact caveat worth flagging: under the D3D12 debug layer every count is
// DOUBLE, since the validation layer forwards each call through this same
// patched code.  Counts in viewcb.csv are therefore relative, not draw counts.
// ---------------------------------------------------------------------------

constexpr int kTableSize = 128; // power of two, indexed by a hash of the address
constexpr int kProbeLimit = 4;

struct Entry
{
    std::atomic<uint64_t> address{0};
    std::atomic<uint32_t> count{0};
};

Entry g_table[kTableSize];
std::atomic<bool> g_tracking{false};

// Constant buffer addresses are 256-byte aligned (D3D12 requires it), so the
// low 8 bits carry no information and hashing on them would put everything in
// one bucket.
inline int HashSlot(uint64_t address)
{
    const uint64_t h = (address >> 8) * 0x9E3779B97F4A7C15ull;
    return static_cast<int>((h >> 40) & (kTableSize - 1));
}

// ---------------------------------------------------------------------------
// The copy itself.
// ---------------------------------------------------------------------------

std::mutex g_shaderMutex;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pso;
bool g_shaderFailed = false;

// A raw-buffer copy, and nothing else. Src is bound as a ROOT SRV, which is the
// entire point of this file: root descriptors take a GPU virtual address
// directly, with no ID3D12Resource and no descriptor heap, so this can read a
// constant buffer we only ever saw as an address.
const char kCopyShader[] = R"(
ByteAddressBuffer   Src : register(t0);
RWByteAddressBuffer Dst : register(u0);

cbuffer Params : register(b0)
{
    uint DstOffset;
    uint ByteCount;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint offset = id.x * 16;
    if (offset < ByteCount)
    {
        Dst.Store4(DstOffset + offset, Src.Load4(offset));
    }
}
)";

bool EnsureShader(ID3D12Device* device)
{
    std::lock_guard<std::mutex> lock(g_shaderMutex);
    if (g_pso != nullptr)
    {
        return true;
    }
    if (g_shaderFailed)
    {
        return false;
    }

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 2;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialised;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialised, &error);
    if (FAILED(hr))
    {
        Log(std::string("viewcb: D3D12SerializeRootSignature failed: ") +
            (error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "(no message)"));
        g_shaderFailed = true;
        return false;
    }
    hr = device->CreateRootSignature(
        0, serialised->GetBufferPointer(), serialised->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));
    if (FAILED(hr))
    {
        Log("viewcb: CreateRootSignature failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        g_shaderFailed = true;
        return false;
    }

    ComPtr<ID3DBlob> bytecode;
    hr = D3DCompile(
        kCopyShader,
        sizeof(kCopyShader) - 1,
        "mv_viewcb_copy",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        0,
        0,
        &bytecode,
        &error);
    if (FAILED(hr))
    {
        Log(std::string("viewcb: D3DCompile failed: ") +
            (error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "(no message)"));
        g_shaderFailed = true;
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.CS.pShaderBytecode = bytecode->GetBufferPointer();
    psoDesc.CS.BytecodeLength = bytecode->GetBufferSize();
    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_pso));
    if (FAILED(hr))
    {
        Log("viewcb: CreateComputePipelineState failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        g_shaderFailed = true;
        return false;
    }
    Log("viewcb: raw-address copy shader and root signature created");
    return true;
}

} // namespace

void ViewCbSetTracking(bool enabled)
{
    g_tracking.store(enabled, std::memory_order_relaxed);
}

void ViewCbNoteRootCbv(uint64_t gpuVirtualAddress)
{
    if (gpuVirtualAddress == 0 || !g_tracking.load(std::memory_order_relaxed))
    {
        return;
    }
    int slot = HashSlot(gpuVirtualAddress);
    for (int probe = 0; probe < kProbeLimit; ++probe)
    {
        Entry& entry = g_table[(slot + probe) & (kTableSize - 1)];
        uint64_t existing = entry.address.load(std::memory_order_relaxed);
        if (existing == gpuVirtualAddress)
        {
            entry.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (existing == 0)
        {
            // Claim it. If someone else claimed it first with a different
            // address we simply fall through to the next probe.
            if (entry.address.compare_exchange_strong(existing, gpuVirtualAddress, std::memory_order_relaxed))
            {
                entry.count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (existing == gpuVirtualAddress)
            {
                entry.count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    // Table full along this probe chain. Dropped deliberately: the buffer this
    // is looking for is bound thousands of times a frame and will already be in
    // the table by the time it fills up.
}

int ViewCbTakeCandidates(ViewCbCandidate* out, int maxOut)
{
    std::vector<ViewCbCandidate> all;
    all.reserve(kTableSize);
    for (Entry& entry : g_table)
    {
        const uint64_t address = entry.address.exchange(0, std::memory_order_relaxed);
        const uint32_t count = entry.count.exchange(0, std::memory_order_relaxed);
        if (address != 0 && count > 0)
        {
            all.push_back(ViewCbCandidate{address, count, 0});
        }
    }
    std::sort(
        all.begin(),
        all.end(),
        [](const ViewCbCandidate& a, const ViewCbCandidate& b) { return a.bindCount > b.bindCount; });

    int written = 0;
    for (const ViewCbCandidate& candidate : all)
    {
        if (written >= maxOut)
        {
            break;
        }
        // How much of kViewCbBytes can be read without leaving the 64KB block
        // the address sits in. A root descriptor isn't bounds-checked, and we
        // don't know the resource's real size, but D3D12 buffers are 64KB-aligned,
        // so staying inside that block bounds the read to "same allocation" even
        // without a spec guarantee.
        const uint64_t intoBlock = candidate.address % 65536ull;
        const uint64_t available = 65536ull - intoBlock;
        ViewCbCandidate entry = candidate;
        entry.bytes = static_cast<uint32_t>(std::min<uint64_t>(kViewCbBytes, available));
        entry.bytes &= ~15u; // the shader copies 16 bytes at a time
        if (entry.bytes < 2304)
        {
            // Too close to the block's end to hold the fields this is for.
            continue;
        }
        out[written++] = entry;
    }
    return written;
}

bool ViewCbRecordCopies(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* uav,
    ID3D12Resource* readback,
    const ViewCbCandidate* candidates,
    int count,
    void(STDMETHODCALLTYPE* barrierFn)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*))
{
    if (device == nullptr || cmdList == nullptr || uav == nullptr || readback == nullptr || count <= 0)
    {
        return false;
    }
    if (!EnsureShader(device))
    {
        return false;
    }

    cmdList->SetPipelineState(g_pso.Get());
    cmdList->SetComputeRootSignature(g_rootSignature.Get());
    const D3D12_GPU_VIRTUAL_ADDRESS uavAddress = uav->GetGPUVirtualAddress();
    for (int i = 0; i < count; ++i)
    {
        const uint32_t dstOffset = static_cast<uint32_t>(i) * kViewCbBytes;
        const uint32_t bytes = candidates[i].bytes;
        cmdList->SetComputeRoot32BitConstant(0, dstOffset, 0);
        cmdList->SetComputeRoot32BitConstant(0, bytes, 1);
        cmdList->SetComputeRootShaderResourceView(1, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(candidates[i].address));
        cmdList->SetComputeRootUnorderedAccessView(2, uavAddress);
        cmdList->Dispatch((bytes / 16 + 63) / 64, 1, 1);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = uav;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrierFn(cmdList, 1, &barrier);

    cmdList->CopyBufferRegion(readback, 0, uav, 0, static_cast<UINT64>(kViewCbCandidates) * kViewCbBytes);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrierFn(cmdList, 1, &barrier);
    return true;
}

} // namespace mv
