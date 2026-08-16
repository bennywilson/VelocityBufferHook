#pragma once

#include <d3d12.h>

#include <cstdint>

// Finding SceneDepth, by the same rules as SceneVelocity.
//
// UE5 creates scene depth in SceneTextures.cpp as PF_DepthStencil with
// TexCreate_DepthStencilTargetable | TexCreate_ShaderResource, at the same
// extent as velocity. On D3D12 that's a TYPELESS depth format with
// ALLOW_DEPTH_STENCIL - unique on both titles measured, and cheap to check
// since the extent is already known once velocity identification has run.
//
// Not an independent identification: it's linked to velocity's extent, so a
// mis-identified velocity buffer would mis-identify depth the same way.

namespace mv
{

// True for the states a depth buffer is read in: DEPTH_READ or any subset
// of the shader-resource bits (UE5 consumes depth both ways).
inline bool IsDepthReadableState(int state)
{
    if (state == 0)
    {
        return false;
    }

    constexpr int kReadable =
        static_cast<int>(D3D12_RESOURCE_STATE_DEPTH_READ) | static_cast<int>(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return (state & ~kReadable) == 0;
}

// Every resource the barrier hook walks past, once per resource.
void NoteResourceDescForDepth(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc);

// Every transition barrier, for the behaviour profile.
void NoteDepthTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter);

// Called once per presented frame, after velocity identification has run.
void OnFrameForDepthIdentification(uint64_t frame);

// Throws away the answer and starts again.
void ReopenDepthIdentification(const char* reason);

// The identified resource, or nullptr while the search is running,
// ambiguous, or waiting for velocity. Exported so the harness can read it.
ID3D12Resource* IdentifiedDepthResource();

// Fast path for the barrier hook: is this the resource we selected?
bool IsDepthCandidate(ID3D12Resource* resource);

// Confirms the selected resource still has the descriptor it was accepted
// with, before recording a barrier/copy sized from its footprints.
bool ValidateDepthCandidate(ID3D12Resource* resource);

} // namespace mv
