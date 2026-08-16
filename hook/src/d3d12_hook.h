#pragma once

namespace mv {

// Bootstraps a temporary D3D12 device, swapchain, command-list purely to
// obtain live vtable pointers.  Hooks the shared underlying function code
// via MinHook (this affects the game's real instances too - see
// d3d12_hook.cpp for why), then tears the dummy objects down. Safe to call
// once from a background thread; not thread-safe to call concurrently.
bool InstallD3D12Hooks();

} // namespace mv
