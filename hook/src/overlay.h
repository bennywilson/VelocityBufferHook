#pragma once

#include "capture.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

namespace mv
{

// Draws the extracted SceneVelocity buffer into the game's back buffer at
// Present. F7 cycles through:
//
//   0  Normal            - chained straight through, zero GPU cost
//   1  SceneVelocity     - the stored channels mapped straight to RGB, the
//                          same picture the engine's own
//                          "visualizetexture SceneVelocity" draws
//   2  Over gameplay     - the same, composited over the live frame
//
enum class OverlayMode
{
    Off = 0,
    Velocity = 1,
    VelocityOverGameplay = 2,
    Count = 3
};

// Called from the Present hooks before chaining through, while the back buffer
// `frame` is the presented-frame counter. This function checsk that the velocity texture
// being drawn actually belongs to this frame.
void OverlayOnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, uint64_t frame);

// Records a copy of the velocity buffer into the overlay's own SRV-able
// texture. Called at the barrier where the source is known to hold valid
// contents as it is a transient resource.
// `frame` stamps the copy so OverlayOnPresent knows if its valid for this frame
void OverlayCaptureVelocity(
    ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source, D3D12_RESOURCE_STATES stateBefore, uint64_t frame);

// The overlay records its own barriers and submissions; they must go through
// the original function pointers, not our hooks, so they do not re-enter
// identification/logging.
void OverlaySetOriginals(ResourceBarrierFn resourceBarrier, ExecuteCommandListsFn executeCommandLists);

void StartOverlayHotkeyThread();

// Stops the F7 hotkey poller. Always detaches, never joins: by the time
// DLL_PROCESS_DETACH runs at process exit, Windows has already killed the
// thread, and destroying a still-joinable std::thread calls std::terminate().
void StopOverlayHotkeyThread();
bool OverlayActive();

} // namespace mv
