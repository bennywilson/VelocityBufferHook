#include "d3d12_hook.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "capture.h"
#include "depth_identify.h"
#include "logging.h"
#include "overlay.h"
#include "resource_tracking.h"
#include "velocity_identify.h"
#include "view_cb.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace mv
{
namespace
{

// Vtable slot indices, verified against the Windows SDK IDL (dxgi.idl,
// dxgi1_2.idl, d3d12.idl - 10.0.26100.0), not guessed from memory: each
// interface's vtable is [inherited IUnknown/base methods][this interface's
// own methods in declaration order]. COM guarantees these indices are
// stable across vendors and OS versions.
//
// IDXGISwapChain:
//      [00-02] IUnknown                (QueryInterface, AddRef, Release)
//      [03-06] IDXGIObject
//      [07]    IDXGIDeviceSubObject    (GetDevice)
//      [08-17] IDXGISwapChain          (Present = 8, GetBuffer = 9, ...)
//
//  IDXGISwapChain1:
//      [00-17] Inherits IDXGISwapChain
//      [18-21] IDXGISwapChain1         (GetDesc1 .. GetCoreWindow)
//      [22]    Present1                <-- Target hook
//
//
//  ID3D12CommandQueue:
//      [00-02] IUnknown
//      [03-06] ID3D12Object
//      [07]    ID3D12DeviceChild/ ID3D12Pageables
//      [08]    UpdateTileMappings
//      [09]    CopyTileMappings
//      [10]    ExecuteCommandLists  <-- Target hook
//
//  ID3D12GraphicsCommandList:
//      [00-07] Base Interfaces      (IUnknown through ID3D12Pageable)
//      [08]    ID3D12CommandList    (GetType)
//      [09]    Close
//      [25]    SetPipelineState
//      [26]    ResourceBarrier
//      [27]    ExecuteBundle
//      [28]    SetDescriptorHeaps
//      [29]    SetComputeRootSignature             // Note: compute/graphics pairs altenerate
//      [30]    SetGraphicsRootSignature
//      [31]    SetComputeRootDescriptorTable
//      [32]    SetGraphicsRootDescriptorTable
//      [33]    SetComputeRoot32BitConstant
//      [34]    SetGraphicsRoot32BitConstant
//      [35]    SetComputeRoot32BitConstants
//      [36]    SetGraphicsRoot32BitConstants
//      [37]    SetComputeRootConstantBufferView
//      [38]    SetGraphicsRootConstantBufferView
//      [46]    OMSetRenderTargets
//      [48]    ClearRenderTargetView
//
//  ID3D12Device:
//      [00-02] IUnknown                  (QueryInterface, AddRef, Release)
//      [03-06] ID3D12Object              (GetPrivateData, SetPrivateData, ...)
//      [07]    GetNodeCount
//      [08]    CreateCommandQueue
//      [20]    CreateRenderTargetView
//      [25]    CreateCommittedResource
//      [27]    CreatePlacedResource

// Vtable indices required by hook
constexpr size_t kPresentIndex = 8;
constexpr size_t kPresent1Index = 22;
constexpr size_t kExecuteCommandListsIndex = 10;
constexpr size_t kResourceBarrierIndex = 26;
constexpr size_t kSetComputeRootConstantBufferViewIndex = 37;
constexpr size_t kSetGraphicsRootConstantBufferViewIndex = 38;
constexpr size_t kOmSetRenderTargetsIndex = 46;
constexpr size_t kClearRenderTargetViewIndex = 48;
constexpr size_t kCreateRenderTargetViewIndex = 20;
constexpr size_t kCreateCommittedResourceIndex = 25;
constexpr size_t kCreatePlacedResourceIndex = 27;

// Idiomatic way to hook COM interfaces in C++ using MinHook
void* VTableEntry(void* instance, size_t index)
{
    void** vtable = *reinterpret_cast<void***>(instance);
    return vtable[index];
}

// Hook Function Pointer Signatures.
// Note: First parameter is always the implicit 'this' interface pointer.
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ExecuteCommandLists_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ResourceBarrier_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using SetRootConstantBufferView_t =
    void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using OMSetRenderTargets_t = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ClearRenderTargetView_t = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*);
using CreateRenderTargetView_t = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateCommittedResource_t = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*,
    const D3D12_HEAP_PROPERTIES*,
    D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*,
    REFIID,
    void**);
using CreatePlacedResource_t = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*,
    ID3D12Heap*,
    UINT64,
    const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*,
    REFIID,
    void**);

Present_t g_originalPresent = nullptr;
Present1_t g_originalPresent1 = nullptr;
ExecuteCommandLists_t g_originalExecuteCommandLists = nullptr;
ResourceBarrier_t g_originalResourceBarrier = nullptr;
SetRootConstantBufferView_t g_originalSetComputeRootConstantBufferView = nullptr;
SetRootConstantBufferView_t g_originalSetGraphicsRootConstantBufferView = nullptr;
OMSetRenderTargets_t g_originalOMSetRenderTargets = nullptr;
ClearRenderTargetView_t g_originalClearRenderTargetView = nullptr;
CreateRenderTargetView_t g_originalCreateRenderTargetView = nullptr;
CreateCommittedResource_t g_originalCreateCommittedResource = nullptr;
CreatePlacedResource_t g_originalCreatePlacedResource = nullptr;

// Global frame key: single source of truth for pairing velocity and colour
// captures across both Present paths. Atomic since multiple threads record
// command lists in parallel; the counters below are diagnostics only.
std::atomic<uint64_t> g_frameCounter{0};

std::atomic<uint64_t> g_presentCount{0};
std::atomic<uint64_t> g_present1Count{0};
std::atomic<uint64_t> g_executeCommandListsCount{0};
std::atomic<uint64_t> g_resourceBarrierCount{0};
std::atomic<uint64_t> g_omSetRenderTargetsCount{0};
std::atomic<uint64_t> g_clearRenderTargetViewCount{0};

// Budget-capped dump of what's bound at each OMSetRenderTargets call
// (resolved format/dimensions), for manual correlation. Needed because most
// resources are allocated once before injection, so creation-time hooks
// only learn a resource's format/dimensions if watching when it was
// (re)created.
std::atomic<int64_t> g_rtvDumpBudget{20000};

// Periodic summary of high-frequency hooks
void LogHeartbeatIfDue()
{
    // Gate on activity from *either* Present path - modern D3D12 titles
    // (UE5 included) almost always present via IDXGISwapChain3::Present1
    // (flip-model/HDR/VRR support), so gating on legacy Present alone would
    // silently never fire even with everything working correctly.
    const uint64_t n = g_presentCount.load(std::memory_order_relaxed) + g_present1Count.load(std::memory_order_relaxed);
    if (n <= 5 || n % 300 == 0)
    {
        // Total alongside individual Present/Present1 counts reveals which
        // swapchain API the engine uses, rather than masking it in a sum.
        Log("heartbeat: frames=" + std::to_string(n) +
            " present=" + std::to_string(g_presentCount.load(std::memory_order_relaxed)) +
            " present1=" + std::to_string(g_present1Count.load(std::memory_order_relaxed)) +
            " executeCommandLists=" + std::to_string(g_executeCommandListsCount.load(std::memory_order_relaxed)) +
            " resourceBarrier=" + std::to_string(g_resourceBarrierCount.load(std::memory_order_relaxed)) +
            " omSetRenderTargets=" + std::to_string(g_omSetRenderTargetsCount.load(std::memory_order_relaxed)) +
            " clearRenderTargetView=" + std::to_string(g_clearRenderTargetViewCount.load(std::memory_order_relaxed)));
    }
}

// These hooks fully replace the game's Present/ExecuteCommandLists/etc.
// Every path through them MUST call the original.
//
// The capture hand-off happens *before* the counter is incremented and
// before chaining to the real Present: the back buffer still holds the
// finished frame at this point, and the frame index must match the one
// ResourceBarrier used earlier in the same frame so velocity/colour pair up.
//
// Back-buffer size is read every frame until identification has an answer
// (the search uses it), then polled (the resize check uses it).

// Window resizing abandons the identified velocity resource which requires
// reidentifying it. Poll every 30 frames
constexpr uint64_t kResizePollFrames = 30;

void NoteBackBufferSizeIfNeeded(IDXGISwapChain* const swapChain, const uint64_t frame)
{
    if (!swapChain)
    {
        return;
    }

    if (mv::IdentifiedVelocityResource() && (frame % kResizePollFrames))
    {
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(swapChain->GetDesc(&desc)))
    {
        if (mv::NoteBackBufferSize(desc.BufferDesc.Width, desc.BufferDesc.Height))
        {
            mv::ReopenIdentification("the back buffer changed size which abandons identified Scene Textures");
            mv::ReopenDepthIdentification("the velocity search was reopened after a back-buffer resize");
        }
    }
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    const uint64_t frame = g_frameCounter.load(std::memory_order_relaxed);
    mv::SetFrameIndex(frame);
    NoteBackBufferSizeIfNeeded(swapChain, frame);
    mv::OnFrameForIdentification(frame);
    mv::OnFrameForDepthIdentification(frame);
    mv::OnPresent(swapChain);
    mv::OverlayOnPresent(swapChain, mv::GetGameQueue(), frame);
    g_frameCounter.fetch_add(1, std::memory_order_relaxed);
    g_presentCount.fetch_add(1, std::memory_order_relaxed);
    LogHeartbeatIfDue();
    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE
HookPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* presentParams)
{
    const uint64_t frame = g_frameCounter.load(std::memory_order_relaxed);
    mv::SetFrameIndex(frame);
    NoteBackBufferSizeIfNeeded(swapChain, frame);
    mv::OnFrameForIdentification(frame);
    mv::OnFrameForDepthIdentification(frame);
    mv::OnPresent(swapChain);
    mv::OverlayOnPresent(swapChain, mv::GetGameQueue(), frame);
    g_frameCounter.fetch_add(1, std::memory_order_relaxed);
    g_present1Count.fetch_add(1, std::memory_order_relaxed);
    LogHeartbeatIfDue();
    return g_originalPresent1(swapChain, syncInterval, flags, presentParams);
}

void STDMETHODCALLTYPE
HookExecuteCommandLists(ID3D12CommandQueue* queue, UINT numCommandLists, ID3D12CommandList* const* commandLists)
{
    g_executeCommandListsCount.fetch_add(1, std::memory_order_relaxed);
    // Remember the game's graphics queue so our own back-buffer copy can be
    // submitted to it (and covered by the same fence as the velocity copy).
    mv::NoteCommandQueue(queue);
    // Check that the list carrying the velocity copy is submitted to that queue before we signal the fence.
    mv::NoteSubmission(queue, numCommandLists, commandLists);
    g_originalExecuteCommandLists(queue, numCommandLists, commandLists);
}

// SceneVelocity's Identification policy lives in velocity_identify.cpp.
// The cap on the max distinct resources we call GetDesc() on lives in
// resource_tracking.cpp, next to the "seen" set it belongs with - see
// TryTakeIdentifyBudget.

// ---------------------------------------------------------------------------
// Feeding the identifier.
//
// Every resource that turns up in a barrier gets its desc read once and
// handed to velocity_identify, keeping a behaviour profile for all of them.
//
// Profiles are keyed on raw pointers with no references, so a recycled
// address merges two resources' histories - this biases against selecting
// anything, since the merged history looks less like a clean 2-cycle life.
// The structural shortlist does hold references, so the resource actually
// acted upon can't be a recycled address.
void TryIdentifyResource(ID3D12Resource* resource)
{
    if (!resource || !TryMarkResourceSeen(resource))
    {
        return;
    }
    if (!TryTakeIdentifyBudget())
    {
        return;
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    mv::NoteResourceDesc(resource, desc);
    // Same desc feeds the depth search. Reading it once and sharing it is
    // deliberate: GetDesc() on a barrier-encountered resource is a considered
    // bet, not proven-safe (see the budget above and the
    // CreateRenderTargetView crash in DEBUGGING.md), so it's called once per
    // resource total, not once per module.
    mv::NoteResourceDescForDepth(resource, desc);
}

// Re-entrancy guard for the barrier hook.
//
// Under the D3D12 debug layer, every ResourceBarrier call reaches this hook
// twice: the validation layer forwards to the core layer through the same
// patched code, so one app call arrives as two nested calls. Left
// unguarded, this doubles every count (the real velocity buffer scores 4
// and gets rejected) and the continuity check sees RT->SRV twice and flags
// a violation that never happened - both behavioural filters invert under
// instrumentation.
thread_local int g_barrierHookDepth = 0;

void STDMETHODCALLTYPE
HookResourceBarrier(ID3D12GraphicsCommandList* cmdList, UINT numBarriers, const D3D12_RESOURCE_BARRIER* barriers)
{
    ++g_barrierHookDepth;
    g_originalResourceBarrier(cmdList, numBarriers, barriers);
    const int depth = --g_barrierHookDepth;
    if (depth > 0)
    {
        // An inner, forwarded call. The outer one describes the same barriers
        // and will do the work; doing it here as well double-counts everything.
        return;
    }
    g_resourceBarrierCount.fetch_add(1, std::memory_order_relaxed);

    if (!barriers)
    {
        return;
    }

    // Recurrence logging for known candidates
    const uint64_t frame = g_frameCounter.load(std::memory_order_relaxed);
    for (UINT i = 0; i < numBarriers; ++i)
    {
        switch (barriers[i].Type)
        {
            case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            {
                ID3D12Resource* const resource = barriers[i].Transition.pResource;
                const int stateBefore = static_cast<int>(barriers[i].Transition.StateBefore);
                const int stateAfter = static_cast<int>(barriers[i].Transition.StateAfter);
                TryIdentifyResource(resource);
                mv::NoteTransition(resource, frame, stateBefore, stateAfter);
                mv::NoteDepthTransition(resource, frame, stateBefore, stateAfter);
                // DEPTH_WRITE -> readable is the depth buffer's equivalent of
                // velocity's RENDER_TARGET -> shader-resource edge: the pass
                // that was writing it has finished and something is about to
                // read it. Same reason for copying here rather than at Present -
                // scene depth is a transient placed resource too.
                if (mv::IsDepthCandidate(resource) && stateBefore == D3D12_RESOURCE_STATE_DEPTH_WRITE &&
                    mv::IsDepthReadableState(stateAfter))
                {
                    if (mv::ValidateDepthCandidate(resource))
                    {
                        mv::SetFrameIndex(frame);
                        mv::OnDepthReadable(cmdList, resource, stateAfter);
                    }
                    else
                    {
                        mv::ReopenDepthIdentification("the selected depth resource's descriptor changed underneath us");
                    }
                }
                if (IsCandidate(resource))
                {
                    LogTo(
                        "candidates",
                        "frame=" + std::to_string(frame) +
                            " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(resource)) + " TRANSITION before=" +
                            std::to_string(static_cast<int>(barriers[i].Transition.StateBefore)) +
                            " after=" + std::to_string(static_cast<int>(barriers[i].Transition.StateAfter)));

                    // RENDER_TARGET -> shader-resource is the moment velocity
                    // has been fully written and is about to be read - the
                    // only safe point to copy it, since it's a
                    // transient/placed resource whose heap memory gets
                    // aliased later in the frame.
                    //
                    // Destination state is matched as any subset of the
                    // shader-resource bits, not ALL_SHADER_RESOURCE exactly:
                    // UE5's RDG usually maps to ALL_SHADER_RESOURCE (0xC0),
                    // but a pixel-shader-only read gets PIXEL_SHADER_RESOURCE
                    // (0x80). The actual state is passed through and restored
                    // rather than assumed.
                    if (stateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET && IsShaderResourceState(stateAfter))
                    {
                        // Confirm the resource still has the desc it was
                        // accepted with before recording a barrier/copy sized
                        // from its footprints - cheap (once per frame), and
                        // the only guard against a recycled address or
                        // resolution change causing silent corruption.
                        if (ValidateCandidate(resource))
                        {
                            mv::SetFrameIndex(frame);
                            mv::OnVelocityReadable(cmdList, resource, stateAfter);
                            // Same moment, same validity argument - the live
                            // overlay samples a GPU-side copy taken here rather
                            // than anything read back to the CPU.
                            mv::OverlayCaptureVelocity(
                                cmdList, resource, static_cast<D3D12_RESOURCE_STATES>(stateAfter), frame);
                        }
                        else
                        {
                            mv::ReopenIdentification(
                                "the selected resource's descriptor changed underneath us (resolution change, "
                                "dynamic resolution scaling, or a swapchain resize)");
                            // Depth identification is gated on the velocity
                            // extent, so a velocity resolution change
                            // invalidates the depth answer too - silently,
                            // since the depth resource is usually recreated
                            // at the same moment and passes its own
                            // descriptor check right up until it doesn't.
                            mv::ReopenDepthIdentification("the velocity search was reopened");
                        }
                    }
                }
                else if (stateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET && IsShaderResourceState(stateAfter))
                {
                    // Not the identified resource. Uninteresting normally -
                    // but if the identified one is mid-stall, a same-format
                    // texture taking this exact edge is the difference
                    // between "stopped rendering velocity" and "rendering it
                    // somewhere else". Gated on an in-progress stall, so free
                    // otherwise.
                    if (mv::NoteRivalVelocityEdge(resource, frame))
                    {
                        // Adopted, not reidentified: marking it a candidate
                        // puts it on the ordinary capture path above, so from
                        // the next edge onward it validates/copies/feeds the
                        // overlay like the first target. Reopening the search
                        // instead would throw away a correct answer and land
                        // in AMBIGUOUS - both widths are live and neither is
                        // wrong.
                        mv::MarkAsCandidate(resource, resource->GetDesc());
                    }
                }
                break;
            }
            case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            {
                ID3D12Resource* resource = barriers[i].UAV.pResource;
                TryIdentifyResource(resource);
                mv::NoteUavBarrier(resource);
                if (IsCandidate(resource))
                {
                    LogTo(
                        "candidates",
                        "frame=" + std::to_string(frame) +
                            " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(resource)) + " UAV");
                }
                break;
            }
            default:
            {
                break;
            }
        }
    }
}

// UE5's D3D12 RHI binds every uniform buffer (including the View buffer) as
// a ROOT constant buffer view - note it here. Hottest hook in the file by a
// wide margin: fires for every uniform buffer of every draw.
void STDMETHODCALLTYPE HookSetGraphicsRootConstantBufferView(
    ID3D12GraphicsCommandList* cmdList, UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    mv::ViewCbNoteRootCbv(address);
    g_originalSetGraphicsRootConstantBufferView(cmdList, rootParameterIndex, address);
}

void STDMETHODCALLTYPE HookSetComputeRootConstantBufferView(
    ID3D12GraphicsCommandList* cmdList, UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    mv::ViewCbNoteRootCbv(address);
    g_originalSetComputeRootConstantBufferView(cmdList, rootParameterIndex, address);
}

void STDMETHODCALLTYPE HookOMSetRenderTargets(
    ID3D12GraphicsCommandList* cmdList,
    UINT numRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* renderTargetDescriptors,
    BOOL rtsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depthStencilDescriptor)
{
    const uint64_t callIndex = g_omSetRenderTargetsCount.fetch_add(1, std::memory_order_relaxed);

    if (g_rtvDumpBudget.fetch_sub(1, std::memory_order_relaxed) > 0 && renderTargetDescriptors)
    {
        const uint64_t frame = g_frameCounter.load(std::memory_order_relaxed);
        std::string line = "frame=" + std::to_string(frame) + " call=" + std::to_string(callIndex) +
                           " numRTs=" + std::to_string(numRenderTargetDescriptors) + " [";
        for (UINT i = 0; i < numRenderTargetDescriptors; ++i)
        {
            ID3D12Resource* resource = GetResourceForRenderTargetView(renderTargetDescriptors[i].ptr);
            const auto info = resource != nullptr ? GetResourceInfo(resource) : std::nullopt;
            if (info.has_value())
            {
                line += "(slot=" + std::to_string(i) + ",fmt=" + std::to_string(static_cast<int>(info->format)) +
                        ",w=" + std::to_string(info->width) + ",h=" + std::to_string(info->height) + ") ";
            }
            else
            {
                line += "(slot=" + std::to_string(i) + ",UNKNOWN) ";
            }
        }
        line += "]";
        LogTo("rtv_dump", line);
    }

    g_originalOMSetRenderTargets(
        cmdList,
        numRenderTargetDescriptors,
        renderTargetDescriptors,
        rtsSingleHandleToDescriptorRange,
        depthStencilDescriptor);
}

void STDMETHODCALLTYPE HookClearRenderTargetView(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView,
    const FLOAT colorRGBA[4],
    UINT numRects,
    const D3D12_RECT* rects)
{
    g_clearRenderTargetViewCount.fetch_add(1, std::memory_order_relaxed);
    g_originalClearRenderTargetView(cmdList, renderTargetView, colorRGBA, numRects, rects);
}

void STDMETHODCALLTYPE HookCreateRenderTargetView(
    ID3D12Device* device,
    ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
    g_originalCreateRenderTargetView(device, resource, desc, destDescriptor);
    if (resource != nullptr)
    {
        RecordRenderTargetView(destDescriptor.ptr, resource);
        // Deliberately NOT calling resource->GetDesc() here - tried it and
        // it crashed the game (EXCEPTION_ACCESS_VIOLATION, confirmed via
        // UE5's crash report). Root cause: view-creation calls don't take a
        // reference on the resource (a view is just a descriptor-table
        // entry), so touching the pointer again afterward races whatever the
        // engine does with its own reference next. Sticking to creation-time
        // hooks only misses anything allocated before injection, but never
        // touches a resource outside the window the engine guarantees valid.
    }
}

HRESULT STDMETHODCALLTYPE HookCreateCommittedResource(
    ID3D12Device* device,
    const D3D12_HEAP_PROPERTIES* heapProperties,
    D3D12_HEAP_FLAGS heapFlags,
    const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* optimizedClearValue,
    REFIID riidResource,
    void** ppvResource)
{
    const HRESULT hr = g_originalCreateCommittedResource(
        device, heapProperties, heapFlags, desc, initialState, optimizedClearValue, riidResource, ppvResource);
    if (SUCCEEDED(hr) && ppvResource != nullptr && *ppvResource != nullptr && desc != nullptr)
    {
        // *ppvResource satisfies riidResource (ID3D12Resource or a
        // binary-compatible newer version of it) - used here purely as an
        // identity key, not through its vtable, so the exact requested
        // interface version doesn't matter.
        auto* resource = static_cast<ID3D12Resource*>(*ppvResource);
        RecordResource(resource, ResourceInfo{desc->Format, desc->Width, desc->Height});
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreatePlacedResource(
    ID3D12Device* device,
    ID3D12Heap* heap,
    UINT64 heapOffset,
    const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* optimizedClearValue,
    REFIID riidResource,
    void** ppvResource)
{
    const HRESULT hr = g_originalCreatePlacedResource(
        device, heap, heapOffset, desc, initialState, optimizedClearValue, riidResource, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource && desc)
    {
        auto* resource = static_cast<ID3D12Resource*>(*ppvResource);
        RecordResource(resource, ResourceInfo{desc->Format, desc->Width, desc->Height});
    }
    return hr;
}

bool CreateHook(void* target, void* detour, void** original, const char* name)
{
    const MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK)
    {
        Log(std::string("MH_CreateHook failed for ") + name + ": " + MH_StatusToString(status));
        return false;
    }
    return true;
}

} // namespace

bool InstallD3D12Hooks()
{
    // Register a message-only window to create a swapchain against
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"MvHookDummyWindowClass";
    RegisterClassExW(&windowClass);

    HWND hwnd = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"",
        WS_POPUP,
        0,
        0,
        64,
        64,
        HWND_MESSAGE,
        nullptr,
        windowClass.hInstance,
        nullptr);
    if (!hwnd)
    {
        Log("InstallD3D12Hooks: CreateWindowExW failed: " + std::to_string(GetLastError()));
        return false;
    }

    // Select the high-performance adapter, in case the user is on a
    // hybrid-graphics laptop.
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: CreateDXGIFactory1 failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: EnumAdapterByGpuPreference failed: 0x" +
            std::to_string(static_cast<unsigned long>(hr)) + ", falling back to default adapter");

        // nullptr falls back to system default
        adapter.Reset();
    }
    else
    {
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        char nameBuf[128]{};
        WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);
        Log(std::string("InstallD3D12Hooks: selected high-performance adapter: ") + nameBuf);
    }

    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: D3D12CreateDevice failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> commandQueue;
    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: CreateCommandQueue failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Width = 64;
    swapChainDesc.Height = 64;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    hr = factory->CreateSwapChainForHwnd(commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain);
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: CreateSwapChainForHwnd failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }

    // 6. Dummy command list (to get ID3D12GraphicsCommandList's vtable).
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: CreateCommandAllocator failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> commandList;
    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: CreateCommandList failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }
    commandList->Close();

    // Extract vtable pointers and install hooks. MinHook patches the
    // underlying function code (shared by every instance of a given
    // interface), not anything specific to our dummy instance - once
    // patched, every caller, including the game, goes through our hook.
    Log("InstallD3D12Hooks: dummy device/swapchain/command-list created, installing hooks");

    if (MH_Initialize() != MH_OK)
    {
        Log("InstallD3D12Hooks: MH_Initialize failed");
        DestroyWindow(hwnd);
        return false;
    }

    bool ok = true;
    ok &= CreateHook(
        VTableEntry(swapChain.Get(), kPresentIndex),
        reinterpret_cast<void*>(&HookPresent),
        reinterpret_cast<void**>(&g_originalPresent),
        "Present");
    ok &= CreateHook(
        VTableEntry(swapChain.Get(), kPresent1Index),
        reinterpret_cast<void*>(&HookPresent1),
        reinterpret_cast<void**>(&g_originalPresent1),
        "Present1");
    ok &= CreateHook(
        VTableEntry(commandQueue.Get(), kExecuteCommandListsIndex),
        reinterpret_cast<void*>(&HookExecuteCommandLists),
        reinterpret_cast<void**>(&g_originalExecuteCommandLists),
        "ExecuteCommandLists");
    ok &= CreateHook(
        VTableEntry(commandList.Get(), kResourceBarrierIndex),
        reinterpret_cast<void*>(&HookResourceBarrier),
        reinterpret_cast<void**>(&g_originalResourceBarrier),
        "ResourceBarrier");
    ok &= CreateHook(
        VTableEntry(commandList.Get(), kSetGraphicsRootConstantBufferViewIndex),
        reinterpret_cast<void*>(&HookSetGraphicsRootConstantBufferView),
        reinterpret_cast<void**>(&g_originalSetGraphicsRootConstantBufferView),
        "SetGraphicsRootConstantBufferView");
    ok &= CreateHook(
        VTableEntry(commandList.Get(), kSetComputeRootConstantBufferViewIndex),
        reinterpret_cast<void*>(&HookSetComputeRootConstantBufferView),
        reinterpret_cast<void**>(&g_originalSetComputeRootConstantBufferView),
        "SetComputeRootConstantBufferView");
    ok &= CreateHook(
        VTableEntry(commandList.Get(), kOmSetRenderTargetsIndex),
        reinterpret_cast<void*>(&HookOMSetRenderTargets),
        reinterpret_cast<void**>(&g_originalOMSetRenderTargets),
        "OMSetRenderTargets");
    ok &= CreateHook(
        VTableEntry(commandList.Get(), kClearRenderTargetViewIndex),
        reinterpret_cast<void*>(&HookClearRenderTargetView),
        reinterpret_cast<void**>(&g_originalClearRenderTargetView),
        "ClearRenderTargetView");

    // Todo: Investigate why installing CreateRenderTargetView,
    // CreateCommittedResource, CreatePlacedResource crash here

    if (ok)
    {
        const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
        if (enableStatus != MH_OK)
        {
            Log(std::string("InstallD3D12Hooks: MH_EnableHook failed: ") + MH_StatusToString(enableStatus));
            ok = false;
        }
    }

    // Hand the capture module the *original* function pointers
    if (ok)
    {
        mv::SetOriginalFunctions(g_originalResourceBarrier, g_originalExecuteCommandLists);
        mv::StartCaptureHotkeyThread();
        mv::OverlaySetOriginals(g_originalResourceBarrier, g_originalExecuteCommandLists);
        mv::StartOverlayHotkeyThread();
    }

    // Tear down temp objects
    DestroyWindow(hwnd);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);

    Log(std::string("InstallD3D12Hooks: ") + (ok ? "all hooks installed" : "one or more hooks failed"));
    return ok;
}

} // namespace mv
