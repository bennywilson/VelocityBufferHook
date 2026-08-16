#pragma once

#include <d3d12.h>

#include <cstdint>

// Capturing UE5's View uniform buffer.
//
// Harder than the textures: SceneVelocity/SceneDepth arrive as
// ID3D12Resource pointers in a barrier and can be identified by descriptor.
// The View buffer doesn't - UE5's D3D12 RHI binds it as a ROOT constant
// buffer view, so only a raw D3D12_GPU_VIRTUAL_ADDRESS crosses the API
// boundary. No resource pointer, no call to map the address back to it.
//
// Solution: read the address directly. Root descriptors take a raw GPU
// address with no resource/heap, so a one-line compute shader can read that
// memory into a buffer we own - no correlation needed, works regardless of
// when the backing resource was created. Cost: one compute dispatch per
// captured frame, and the risk that a root-descriptor read past the end of
// its resource is UB and can fault the GPU (see kViewCbBytes and the 64KB
// guard in view_cb.cpp).
//
// WHICH address is the View buffer isn't decided here - the hook copies the
// few most-frequently-bound constant buffers per frame, and the offline pass
// picks the right one by content (View_ViewSizeAndInvSize, a float4 already
// known from velocity's extent). Deciding it in-process would mean
// hardcoding a struct offset for one build of one game.

namespace mv
{

// Bytes to copy per candidate constant buffer. FViewUniformShaderParameters
// is a few KB (needed fields sit at 1872-2080 on UE 5.2), so 4096 covers
// it - also the upper bound on how far a root-descriptor read can run past
// a smaller-than-expected buffer (see the alignment guard in the .cpp).
constexpr uint32_t kViewCbBytes = 4096;

// Distinct constant buffers to copy per frame, most-frequently-bound first.
// The View buffer is bound by nearly every draw, so it ranks near the top;
// taking several lets the offline pass identify it by content.
constexpr int kViewCbCandidates = 4;

struct ViewCbCandidate
{
    uint64_t address = 0;
    uint32_t bindCount = 0;
    uint32_t bytes = 0; // may be less than kViewCbBytes near a 64KB boundary
};

// Called from the SetGraphicsRootConstantBufferView /
// SetComputeRootConstantBufferView hooks. Hot path - runs for every uniform
// buffer of every draw. No-op unless a capture burst has switched tracking on.
void ViewCbNoteRootCbv(uint64_t gpuVirtualAddress);

void ViewCbSetTracking(bool enabled);

// Snapshots the current frame's bind counts, sorts by count and clears the
// table for the next frame. Returns how many candidates were written.
int ViewCbTakeCandidates(ViewCbCandidate* out, int maxOut);

// Records, onto `cmdList`, one dispatch per candidate reading kViewCbBytes
// from its raw GPU address into `uav` at candidate-index * kViewCbBytes,
// then copies `uav` into `readback`. `uav` must be DEFAULT-heap,
// kViewCbCandidates * kViewCbBytes, UNORDERED_ACCESS; `readback` a READBACK
// buffer of the same size.
//
// Returns false if the shader/root signature couldn't be created - nothing
// is recorded, and the caller should continue without the View buffer
// rather than skip the frame.
bool ViewCbRecordCopies(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* uav,
    ID3D12Resource* readback,
    const ViewCbCandidate* candidates,
    int count,
    void(STDMETHODCALLTYPE* barrierFn)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*));

} // namespace mv
