// D3D12 validation harness for the capture path.
//
// WHY THIS EXISTS
//
// Every barrier-correctness claim this project makes used to rest on "the game
// didn't crash". That is weak evidence and it is weak in a specific way: the
// D3D12 debug layer is exactly the thing that catches a transition barrier
// asserting the wrong StateBefore, a copy sized from the wrong footprint, or a
// descriptor overwritten while still referenced - and none of it can be turned
// on inside a shipping game, because a Shipping build does not create its
// device with the debug layer and we cannot make it.
//
// The previous version of this file did nothing D3D12-related at all. It
// smoke-tested the MinHook bootstrap ("do six vtable patches land without
// crashing this process") and nothing else, so the whole capture path - the
// part with the actual risk in it - was never validated by anything but the
// absence of a crash in a program that would not necessarily crash.
//
// WHAT IT DOES
//
// Stands up a real D3D12 device with the debug layer AND GPU-based validation
// enabled, then builds a line-up of PLACED resources (not committed, because the
// real SceneVelocity buffer comes from UE5's TransientResourceAllocator and
// placed resources have their own aliasing rules the debug layer checks
// separately). One of them is a faithful look-alike of SceneVelocity; the rest
// are the plausible mistakes.
//
// Then it loads mv_hook.dll into itself and renders frames. The hook cannot
// tell the difference, so the real capture path runs against resources the
// debug layer is watching. Anything the layer objects to is retrieved from the
// info queue and printed, so the result is an artefact rather than something
// you had to have a debugger attached to see.
//
// WHAT IT ASSERTS, AND WHY THE LINE-UP EXISTS
//
// This used to place a single decoy with SceneVelocity's exact descriptor,
// hardcoded to the same 1212x760 the hook's filter was hardcoded to. That could
// only ever confirm that a constant matched itself, and the assertion it
// supported - "a capture happened" - stayed true throughout the multi-day
// episode in which the filter was selecting the wrong resource entirely (see
// DEBUGGING.md, the R16G16B16A16_FLOAT/_UNORM digit).
//
// So there are now two independent verdicts:
//
//   * debug-layer errors across the run must be zero (the original claim), and
//   * identification must pick the RIGHT resource out of the line-up, checked
//     by pointer through the DLL's MvIdentifiedVelocityResource export - not by
//     the presence of a dump.
//
// Each decoy is rejected by exactly one of the two filters the hook applies, so
// neither filter can be quietly load-bearing on its own. `--ambiguous` adds a
// perfect twin of the real buffer, where the only correct answer is to refuse
// to pick; that path is asserted too.
//
// `--pipeline skyrunner` swaps the real look-alike's barrier pattern for the
// one measured on a third-party UE 5.2 title - five transitions a frame, via
// UNORDERED_ACCESS - which every behavioural rule this filter once enforced
// would have rejected. Both pipelines must select the right resource, which is
// what turns "works across titles" from an anecdote into a test.
//
// USAGE
//   mv_testhost.exe [--hook <dll>] [--frames N] [--no-gbv] [--ambiguous]
//                   [--pipeline oxi|skyrunner]
//   mv_testhost.exe --wpo-encode-test <output_dir>
//
// Set MV_AUTOCAPTURE=1 in the environment to have the hook start a capture
// burst by itself, since there is no one here to press F8.
//
// --wpo-encode-test is unrelated to the rest of this file: it does not load
// the hook at all. It renders a known synthetic velocity grid through the
// real EncodeVelocityToTexture()/VELOCITY_ENCODE_DEPTH path (transcribed
// exactly, see wpo_encode_test.cpp) and dumps it in the same shape a real
// capture uses, so tools/wpo_synthetic_test.py can check that the shipping
// decode correctly recovers values representative of WPO-animated pixels -
// which the analytical reprojection cannot exercise, by construction.

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

// Deliberately NOT 1212x760. That was the resolution of the one title this
// project was developed against, and the harness used to hardcode it "to match
// the identification filter in d3d12_hook.cpp" - which meant the harness could
// only ever confirm that a filter matching a constant matched the same
// constant. Identification is now property-based, so the harness picks a
// resolution the hook has never been told about and the hook has to derive it.
constexpr UINT64 kVelocityWidth = 1120;
constexpr UINT kVelocityHeight = 630;
constexpr DXGI_FORMAT kVelocityFormat = DXGI_FORMAT_R16G16B16A16_UNORM;

constexpr UINT kBackBufferCount = 3;
constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;

// What every look-alike is cleared to, chosen so all four channels decode to
// something checkable rather than only the two that carry displacement.
//
//   x, y = 0.5      -> exactly where UE5's encoding puts zero motion
//                      (32767/65535), so the decode must return 0.00000 px.
//   z, w            -> the two halves of a float32 bit pattern, because that is
//                      what VELOCITY_ENCODE_DEPTH packs there. 13696 = 0x3580
//                      in the high half and 0 in the low half reassemble to
//                      0x35800000 = +9.5367e-07, which is also the ~1e-6
//                      magnitude a real capture shows for the DeviceZ delta.
//
// The previous value cleared z to 0.5 as well, which reassembles to 0x7FFF0000
// - a NaN. That was not wrong, exactly: the harness only claimed to check the
// displacement channels, and it checked them correctly. But it meant the
// depth-channel decode was the one part of the pipeline the harness silently
// did not cover, and "NaN" is indistinguishable from a decode bug when you are
// reading tool output rather than reasoning about the clear value.
constexpr float kVelocityClear[4] = {0.5f, 0.5f, 13696.0f / 65535.0f, 0.0f};

// How each look-alike behaves at the barrier level. The decoys are chosen so
// that each one is rejected by exactly one of the two independent filters - a
// filter that is right for the wrong reason fails here rather than in a game.
enum class Behaviour {
    Velocity,       // RT -> shader-resource -> RT. Exactly 2 events per frame.
    ComputeWritten, // goes through UNORDERED_ACCESS and takes a UAV barrier
    ExtraPass,      // the same RT <-> SRV cycle, but twice per frame
    // Measured on a third-party UE 5.2 title, and the reason --pipeline exists.
    // Its SceneVelocity does five transitions a frame and passes through
    // UNORDERED_ACCESS on the way:
    //     RT->UAV   UAV->NON_PS_SRV   NON_PS_SRV->RT   RT->NON_PS_SRV   ...->RT
    // Nothing about that is unusual for a renderer; it is just not what the
    // first game did. Every behavioural rule the identification filter once had
    // rejects this, which is why they are scored signals now - and why the
    // harness has to be able to reproduce a pipeline other than the one the
    // project grew up on, or "works across titles" stays an anecdote.
    VelocityComputeAssisted,
};

struct LookAlike {
    const char* name;
    const char* why;             // which filter is supposed to reject it, and why
    DXGI_FORMAT format;
    UINT64 width;
    UINT height;
    Behaviour behaviour;
    bool shouldBeSelected;

    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> resource;
    UINT rtvIndex = 0;
};

// The depth line-up. Same idea as the velocity one and the same argument for
// having it: "a depth dump appeared" is satisfied by copying back the wrong
// depth target, and a half-resolution or shadow-map depth buffer decodes to a
// perfectly plausible depth field. Each decoy here is excluded by exactly one
// of the three things the depth filter tests - format, the ALLOW_DEPTH_STENCIL
// / DENY_SHADER_RESOURCE flag pair, and the extent - so none of them can be
// quietly load-bearing.
//
// UE5 creates scene depth TYPELESS (GPixelFormats[PF_DepthStencil], D3D12RHI.cpp:117/127)
// so the look-alikes are typeless too. That is not cosmetic: a typeless
// depth-stencil resource is multi-plane, and copying it back exercises the
// plane/subresource path in capture.cpp that the single-plane velocity buffer
// never reaches.
struct DepthLookAlike {
    const char* name;
    const char* why;
    DXGI_FORMAT format;
    UINT64 width;
    UINT height;
    bool denyShaderResource;
    bool shouldBeSelected;
    // How many DEPTH_WRITE -> readable edges this target takes per frame.
    //
    // The only thing that separates scene depth from CustomDepth, which is why
    // it is a field rather than a constant. UE5 creates both at the same
    // extent, in the same format, with the same
    // TexCreate_DepthStencilTargetable | TexCreate_ShaderResource pair, so
    // their D3D12 descriptors are identical and no structural test can tell
    // them apart. What differs is that the whole frame is built around scene
    // depth and CustomDepth is rendered at most once.
    int edgesPerFrame;

    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> resource;
    UINT dsvIndex = 0;
};

// The offsets this project's offline pass expects to find in UE 5.2's View
// uniform buffer. The harness writes a synthetic View buffer using them so the
// whole chain - hook copies a raw GPU address, tool finds the anchor, tool
// reads the matrix - can be run end to end without a game.
//
// They are NOT asserted here as facts about any title. Which is the point: the
// harness's buffer is one whose layout we chose, so a tool that finds the
// anchor here has demonstrated it can find an anchor, not that 2064 is right
// for Skyrunner. That has to come from Skyrunner's own bytes.
constexpr UINT kViewClipToPrevClipOffset = 1872;
constexpr UINT kViewTemporalAAJitterOffset = 2000;
constexpr UINT kViewRectMinOffset = 2048;
constexpr UINT kViewSizeAndInvSizeOffset = 2064;
constexpr UINT kSyntheticViewCbBytes = 4096;

// How many times per frame each synthetic constant buffer is bound. The hook
// ranks candidates by bind count, so the "View" buffer has to out-bind the
// decoy the way UE5's does - it is bound by every draw, a per-object buffer by
// one.
constexpr int kViewCbBindsPerFrame = 200;
constexpr int kDecoyCbBindsPerFrame = 3;

bool Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        printf("[testhost] FAILED %s: 0x%08lX\n", what, static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

// Pulls everything the debug layer has said since the last call and prints it.
// Returns the number of messages at ERROR severity or worse - the number that
// decides whether this harness passed.
int DrainInfoQueue(ID3D12InfoQueue* infoQueue, const char* phase) {
    if (infoQueue == nullptr) {
        return 0;
    }
    int errors = 0;
    const UINT64 count = infoQueue->GetNumStoredMessages();
    std::vector<uint8_t> buffer;
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &length))) {
            continue;
        }
        buffer.resize(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(infoQueue->GetMessage(i, message, &length))) {
            continue;
        }
        const char* severity = "INFO";
        switch (message->Severity) {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION: severity = "CORRUPTION"; ++errors; break;
            case D3D12_MESSAGE_SEVERITY_ERROR:      severity = "ERROR";      ++errors; break;
            case D3D12_MESSAGE_SEVERITY_WARNING:    severity = "WARNING";    break;
            default: break;
        }
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            printf("[debug-layer/%s] %s (id %d): %.*s\n", phase, severity, static_cast<int>(message->ID),
                   static_cast<int>(message->DescriptionByteLength), message->pDescription);
        }
    }
    infoQueue->ClearStoredMessages();
    return errors;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

// Defined in wpo_encode_test.cpp. Self-contained (own device, no window or
// swapchain) - kept out of the harness below entirely rather than threaded
// through its device/window setup, since it validates a different thing (the
// encode format on synthetic WPO-representative values) than the rest of
// this file (identification and barrier correctness against a real, if
// synthetic, capture pipeline).
int RunWpoEncodeTest(const std::string& outputDir);

int main(int argc, char** argv) {
    std::wstring hookPath;
    int frames = 240;
    bool gpuValidation = true;
    bool ambiguous = false;
    // Which shape of velocity pass to model. "oxi" is the strict 2-cycle this
    // project was developed against; "skyrunner" is the 5-transition,
    // UNORDERED_ACCESS-touching pass measured on a third-party UE 5.2 title.
    // Identification has to find the right resource under both, or "works
    // across titles" is an anecdote rather than a test.
    std::string pipeline = "oxi";
    std::string wpoEncodeTestDir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--hook" && i + 1 < argc) {
            const std::string next = argv[++i];
            hookPath.assign(next.begin(), next.end());
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (arg == "--no-gbv") {
            gpuValidation = false;
        } else if (arg == "--ambiguous") {
            ambiguous = true;
        } else if (arg == "--pipeline" && i + 1 < argc) {
            pipeline = argv[++i];
        } else if (arg == "--wpo-encode-test" && i + 1 < argc) {
            wpoEncodeTestDir = argv[++i];
        }
    }

    if (!wpoEncodeTestDir.empty()) {
        return RunWpoEncodeTest(wpoEncodeTestDir);
    }

    printf("[testhost] pid=%lu, %d frames, pipeline=%s%s\n", GetCurrentProcessId(), frames,
           pipeline.c_str(),
           ambiguous ? ", ambiguous line-up (identification must refuse to pick)" : "");

    // Identification now watches barriers for a settling window before
    // deciding, so the window has to fit inside the run. A third of the frames
    // leaves room for the 60-frame autocapture burst afterwards. Set before
    // LoadLibrary, because the hook reads its environment on first use.
    const int identifyFrames = frames / 3 > 40 ? frames / 3 : 40;
    char identifyFramesText[32];
    snprintf(identifyFramesText, sizeof(identifyFramesText), "%d", identifyFrames);
    SetEnvironmentVariableA("MV_IDENTIFY_FRAMES", identifyFramesText);
    printf("[testhost] MV_IDENTIFY_FRAMES=%d (decision expected around that frame)\n", identifyFrames);

    // 1. Debug layer BEFORE device creation - it has no effect afterwards.
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        printf("[testhost] debug layer enabled\n");
        ComPtr<ID3D12Debug1> debug1;
        if (gpuValidation && SUCCEEDED(debug.As(&debug1))) {
            // GPU-based validation is what catches the errors that matter
            // here: a resource used in a state it is not actually in, and
            // descriptor lifetime problems. It is very slow, hence --no-gbv.
            debug1->SetEnableGPUBasedValidation(TRUE);
            printf("[testhost] GPU-based validation enabled (slow)\n");
        }
    } else {
        printf("[testhost] WARNING: debug layer unavailable - install the Graphics Tools "
               "optional feature. This run validates nothing.\n");
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MvTestHostWindow";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"mv_testhost", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        printf("[testhost] CreateWindowExW failed\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    ComPtr<IDXGIFactory6> factory;
    if (!Check(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")) {
        return 1;
    }
    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));

    ComPtr<ID3D12Device> device;
    if (!Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice")) {
        return 1;
    }

    // Break-on-error is deliberately NOT set: the point is to collect every
    // complaint and report them all, not to stop at the first.
    ComPtr<ID3D12InfoQueue> infoQueue;
    device.As(&infoQueue);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (!Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue")) {
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.BufferCount = kBackBufferCount;
    scDesc.Width = kWidth;
    scDesc.Height = kHeight;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> swapChain1;
    if (!Check(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1),
               "CreateSwapChainForHwnd")) {
        return 1;
    }
    ComPtr<IDXGISwapChain3> swapChain;
    swapChain1.As(&swapChain);

    // 2. The line-up. One of these is SceneVelocity; the others are the
    // plausible mistakes. The hook is told none of it.
    //
    // Every entry is a PLACED resource (not committed), because the real one
    // comes from UE5's TransientResourceAllocator and placed resources have
    // aliasing rules the debug layer checks separately.
    std::vector<LookAlike> lineUp = {
        {"SceneVelocity", "the real thing: velocity format, RT|UAV", kVelocityFormat,
         kVelocityWidth, kVelocityHeight,
         pipeline == "skyrunner" ? Behaviour::VelocityComputeAssisted : Behaviour::Velocity, true},

        // The exact shape of this project's most expensive bug: structurally
        // identical, behaviourally identical, one digit different in the format
        // enum. R16G16B16A16_FLOAT is 10; _UNORM is 11. UE5 never asks for
        // FLOAT here - FVelocityRendering::GetFormat cannot return it - so the
        // structural filter must reject this even though everything it DOES
        // trigger on is present.
        {"HDR pool target", "rejected on structure: R16G16B16A16_FLOAT is not a format "
                            "FVelocityRendering::GetFormat can return",
         DXGI_FORMAT_R16G16B16A16_FLOAT, kVelocityWidth, kVelocityHeight, Behaviour::Velocity, false},

        // Right format, right flags, right resolution - and written by compute.
        // Only the behaviour filter can tell this apart, and only via the UAV
        // barrier. This is the discriminator the README claims separates
        // velocity from the compute-written targets; here it is under test
        // rather than asserted.
        {"compute-written RG16", "rejected on behaviour: goes through UNORDERED_ACCESS and takes a UAV barrier",
         DXGI_FORMAT_R16G16_UNORM, kVelocityWidth, kVelocityHeight, Behaviour::ComputeWritten, false},

        // Byte-identical descriptor to the real one. Nothing structural can
        // separate these two; only "exactly 2 barrier events per frame" does.
        {"twin, read twice", "rejected on behaviour: 4 RT<->SRV events per frame, not 2",
         kVelocityFormat, kVelocityWidth, kVelocityHeight, Behaviour::ExtraPass, false},
    };

    if (pipeline == "skyrunner") {
        // Drop the "twin, read twice" decoy for this pipeline, and note WHY,
        // because the reason is a finding rather than a convenience.
        //
        // That decoy exists to prove the strict two-events-per-frame signal
        // discriminates. When the real velocity pass is itself not a 2-cycle,
        // the signal fires for neither of them - and the two then score
        // identically (11 and 11, measured), so identification correctly
        // refuses to pick. That is the honest answer: on a title whose velocity
        // pass looks like this one's, a second velocity-format RT|UAV texture
        // read twice a frame really would be indistinguishable from outside the
        // process, and saying so beats guessing.
        //
        // Keeping it would test the refusal path, which --ambiguous already
        // covers. Removing it tests what this pipeline exists to test: that a
        // 5-transition, UNORDERED_ACCESS-touching velocity pass is still found.
        lineUp.erase(std::remove_if(lineUp.begin(), lineUp.end(),
                                    [](const LookAlike& t) {
                                        return t.behaviour == Behaviour::ExtraPass;
                                    }),
                     lineUp.end());
    }

    if (ambiguous) {
        // A perfect twin: same descriptor, same behaviour, indistinguishable by
        // anything this hook can observe. The correct answer is not "pick one",
        // it is "refuse and say so" - which is the behaviour under test.
        //
        // "Same behaviour" has to mean the SAME behaviour as whichever
        // SceneVelocity variant this pipeline actually uses, not a hardcoded
        // one. Hardcoding Behaviour::Velocity here made the twin a clean
        // 2-cycle resource while skyrunner's own SceneVelocity (above) is
        // Behaviour::VelocityComputeAssisted (5 transitions, through UAV) -
        // two genuinely different signatures scoring 16 vs 11, a comfortable
        // margin. Identification then correctly - by its own scoring - picked
        // the twin over the real resource instead of refusing, because the
        // fixture was no longer constructing a real tie for this pipeline.
        lineUp.push_back({"perfect twin", "indistinguishable - identification must REFUSE, not guess",
                          kVelocityFormat, kVelocityWidth, kVelocityHeight, lineUp.front().behaviour, false});
    }

    // RTVs: one per back buffer plus one per look-alike.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = kBackBufferCount + static_cast<UINT>(lineUp.size());
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (!Check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap")) {
        return 1;
    }
    const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtvAt = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * rtvSize;
        return h;
    };

    for (size_t i = 0; i < lineUp.size(); ++i) {
        LookAlike& target = lineUp[i];
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = target.width;
        desc.Height = target.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = target.format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const D3D12_RESOURCE_ALLOCATION_INFO alloc = device->GetResourceAllocationInfo(0, 1, &desc);
        D3D12_HEAP_DESC heapDesc{};
        heapDesc.SizeInBytes = alloc.SizeInBytes;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapDesc.Alignment = alloc.Alignment;
        // Match the real thing rather than the convenient thing.
        //
        // RenderDoc on the third-party target shows SceneVelocity placed at
        // offset 0 of a 128 MiB DEFAULT heap created with CREATE_NOT_ZEROED and
        // NO restriction flags - i.e. a resource-heap-tier-2 pool that can hold
        // buffers and textures alike, with 114 MiB left over for whatever the
        // transient allocator puts there next. This harness was creating an
        // ALLOW_ONLY_RT_DS_TEXTURES heap sized to exactly one texture, which is
        // a different object with different aliasing rules - and aliasing rules
        // are precisely what the debug layer checks about placed resources, so
        // modelling the wrong kind of heap weakens the one thing this harness
        // exists to verify.
        //
        // CREATE_NOT_ZEROED is copied over too. It is not cosmetic: asking the
        // driver not to zero memory is what you do when you expect the previous
        // tenant's garbage, which is the clearest signal in the capture that
        // this memory is recycled.
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        const bool tier2 =
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
            options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2;
        heapDesc.Flags = (tier2 ? D3D12_HEAP_FLAG_NONE : D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES) |
                         D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
        if (!Check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&target.heap)), "CreateHeap")) {
            return 1;
        }

        // Must match the value actually cleared to below, or every frame
        // produces a debug-layer warning about a slow clear and drowns the
        // output we care about.
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = target.format;
        memcpy(clearValue.Color, kVelocityClear, sizeof(kVelocityClear));
        if (!Check(device->CreatePlacedResource(
                       target.heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
                       IID_PPV_ARGS(&target.resource)),
                   "CreatePlacedResource")) {
            return 1;
        }
        target.rtvIndex = kBackBufferCount + static_cast<UINT>(i);
        device->CreateRenderTargetView(target.resource.Get(), nullptr, rtvAt(target.rtvIndex));
        printf("[testhost] look-alike %zu: %-22s %llux%u fmt=%2d - %s\n", i, target.name, target.width,
               target.height, static_cast<int>(target.format), target.why);
    }

    // 2b. The depth line-up, and the descriptor heap for its DSVs.
    std::vector<DepthLookAlike> depthLineUp = {
        {"SceneDepth", "the real thing: PF_DepthStencil typeless, DS|SRV, at the velocity extent",
         DXGI_FORMAT_R32G8X24_TYPELESS, kVelocityWidth, kVelocityHeight, false, true, 4},
        {"shadow atlas", "rejected on flags AND extent: DENY_SHADER_RESOURCE, 2048x2048",
         DXGI_FORMAT_R32G8X24_TYPELESS, 2048, 2048, true, false, 1},
        {"half-res depth", "rejected on extent alone: same format and flags, half the size",
         DXGI_FORMAT_R32G8X24_TYPELESS, kVelocityWidth / 2, kVelocityHeight / 2, false, false, 1},
        {"occluder depth", "rejected on flags alone: right extent, but DENY_SHADER_RESOURCE",
         DXGI_FORMAT_R32G8X24_TYPELESS, kVelocityWidth, kVelocityHeight, true, false, 1},
        // The one no structural test can reject, and the reason the depth
        // filter needed a behavioural tie-break at all. This is not a
        // hypothetical decoy: identification hit exactly this on the
        // third-party title, found two survivors with identical scores, and
        // refused to pick - which was the right answer and still left the
        // reprojection with no depth. Only "how hard is it worked" separates
        // them.
        {"CustomDepth", "structurally IDENTICAL to scene depth - separable only by per-frame usage",
         DXGI_FORMAT_R32G8X24_TYPELESS, kVelocityWidth, kVelocityHeight, false, false, 1},
    };

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.NumDescriptors = static_cast<UINT>(depthLineUp.size());
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    if (!Check(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)), "CreateDescriptorHeap(DSV)")) {
        return 1;
    }
    const UINT dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    auto dsvAt = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * dsvSize;
        return h;
    };

    for (size_t i = 0; i < depthLineUp.size(); ++i) {
        DepthLookAlike& target = depthLineUp[i];
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = target.width;
        desc.Height = target.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = target.format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (target.denyShaderResource) {
            desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }

        const D3D12_RESOURCE_ALLOCATION_INFO alloc = device->GetResourceAllocationInfo(0, 1, &desc);
        D3D12_HEAP_DESC heapDesc{};
        heapDesc.SizeInBytes = alloc.SizeInBytes;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapDesc.Alignment = alloc.Alignment;
        D3D12_FEATURE_DATA_D3D12_OPTIONS depthOptions{};
        const bool depthTier2 =
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &depthOptions, sizeof(depthOptions))) &&
            depthOptions.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2;
        heapDesc.Flags = (depthTier2 ? D3D12_HEAP_FLAG_NONE : D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES) |
                         D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
        if (!Check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&target.heap)), "CreateHeap(depth)")) {
            return 1;
        }

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        // UE5 uses a reversed-Z buffer, so the far plane is 0.0 - but clearing
        // to 0.0 here would make "the depth decode works" and "the depth buffer
        // is all far plane" produce identical output, and the offline tools
        // treat DeviceZ == 0 as "no geometry". 0.5 is a value that can only
        // have come through the copy and the plane-0 unpacking intact.
        clearValue.DepthStencil.Depth = 0.5f;
        clearValue.DepthStencil.Stencil = 0;
        if (!Check(device->CreatePlacedResource(
                       target.heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                       IID_PPV_ARGS(&target.resource)),
                   "CreatePlacedResource(depth)")) {
            return 1;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        target.dsvIndex = static_cast<UINT>(i);
        device->CreateDepthStencilView(target.resource.Get(), &dsvDesc, dsvAt(target.dsvIndex));
        printf("[testhost] depth look-alike %zu: %-16s %llux%u fmt=%2d - %s\n", i, target.name, target.width,
               target.height, static_cast<int>(target.format), target.why);
    }

    // 2c. Two synthetic constant buffers, so the View-uniform-buffer path has
    // something to find. This is an UPLOAD-heap buffer bound as a ROOT CBV,
    // which is exactly how UE5's D3D12 RHI binds uniform buffers - the hook
    // never sees a resource pointer for it, only a GPU virtual address, and the
    // whole point of the exercise is that it reads that address anyway.
    ComPtr<ID3D12Resource> viewCb;
    ComPtr<ID3D12Resource> decoyCb;
    {
        D3D12_HEAP_PROPERTIES uploadProps{};
        uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbDesc{};
        cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbDesc.Width = 65536;
        cbDesc.Height = 1;
        cbDesc.DepthOrArraySize = 1;
        cbDesc.MipLevels = 1;
        cbDesc.Format = DXGI_FORMAT_UNKNOWN;
        cbDesc.SampleDesc.Count = 1;
        cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!Check(device->CreateCommittedResource(
                       &uploadProps, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                       IID_PPV_ARGS(&viewCb)),
                   "CreateCommittedResource(viewCb)") ||
            !Check(device->CreateCommittedResource(
                       &uploadProps, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                       IID_PPV_ARGS(&decoyCb)),
                   "CreateCommittedResource(decoyCb)")) {
            return 1;
        }
        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (SUCCEEDED(viewCb->Map(0, &noRead, &mapped)) && mapped != nullptr) {
            auto* bytes = static_cast<uint8_t*>(mapped);
            memset(bytes, 0, kSyntheticViewCbBytes);
            // A ClipToPrevClip for a camera that has not moved is the identity,
            // and that is what the harness models: it makes the expected
            // reprojection exactly zero everywhere, so a non-zero result from
            // the offline tool is a bug in the tool rather than a judgement
            // call about a scene.
            float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            memcpy(bytes + kViewClipToPrevClipOffset, identity, sizeof(identity));
            // ClipToPrevClipWithAA is the adjacent member, and the offline
            // pass uses "these two are nearly but not exactly equal" as its
            // structural check that it has found the right pair. Writing only
            // the first one leaves the second as zeroes, which fails that
            // check - so the harness would be testing the tool against a
            // buffer that does not look like the thing it is for.
            float withAa[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            withAa[12] = 0.0004f; // the jitter delta, in screen-position units
            withAa[13] = -0.0003f;
            memcpy(bytes + kViewClipToPrevClipOffset + 64, withAa, sizeof(withAa));
            const float jitter[4] = {0.0002f, -0.00015f, -0.0001f, 0.00025f};
            memcpy(bytes + kViewTemporalAAJitterOffset, jitter, sizeof(jitter));
            const float rectMin[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            memcpy(bytes + kViewRectMinOffset, rectMin, sizeof(rectMin));
            const float sizeAndInv[4] = {
                static_cast<float>(kVelocityWidth), static_cast<float>(kVelocityHeight),
                1.0f / static_cast<float>(kVelocityWidth), 1.0f / static_cast<float>(kVelocityHeight)};
            memcpy(bytes + kViewSizeAndInvSizeOffset, sizeAndInv, sizeof(sizeAndInv));
            viewCb->Unmap(0, nullptr);
        }
        if (SUCCEEDED(decoyCb->Map(0, &noRead, &mapped)) && mapped != nullptr) {
            memset(mapped, 0x5A, kSyntheticViewCbBytes);
            decoyCb->Unmap(0, nullptr);
        }
    }

    // A root signature whose only parameter is a root CBV. Root arguments can
    // only be set once a matching root signature is bound, so this exists purely
    // to make the SetGraphicsRootConstantBufferView calls below legal.
    ComPtr<ID3D12RootSignature> cbRootSignature;
    {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.Descriptor.ShaderRegister = 0;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 1;
        rsDesc.pParameters = &param;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> error;
        if (!Check(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                   "D3D12SerializeRootSignature") ||
            !Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&cbRootSignature)),
                   "CreateRootSignature")) {
            return 1;
        }
    }

    ComPtr<ID3D12Resource> backBuffers[kBackBufferCount];
    for (UINT i = 0; i < kBackBufferCount; ++i) {
        if (!Check(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])), "GetBuffer")) {
            return 1;
        }
        device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, rtvAt(i));
    }

    ComPtr<ID3D12CommandAllocator> allocators[kBackBufferCount];
    for (UINT i = 0; i < kBackBufferCount; ++i) {
        if (!Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])),
                   "CreateCommandAllocator")) {
            return 1;
        }
    }
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    if (!Check(device->CreateCommandList(
                   0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&cmdList)),
               "CreateCommandList")) {
        return 1;
    }
    cmdList->Close();

    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    int errorsBeforeHook = DrainInfoQueue(infoQueue.Get(), "setup");

    // 3. Load the hook into ourselves. LoadLibrary rather than remote
    // injection: it reaches the same DllMain and the same InstallD3D12Hooks,
    // and what is under test here is the capture path, not the injector.
    HMODULE hookModule = nullptr;
    if (!hookPath.empty()) {
        hookModule = LoadLibraryW(hookPath.c_str());
        if (hookModule == nullptr) {
            printf("[testhost] LoadLibraryW failed: %lu\n", GetLastError());
            return 1;
        }
        printf("[testhost] loaded hook, waiting for it to install...\n");
        Sleep(1500); // InstallD3D12Hooks runs on its own thread
    } else {
        printf("[testhost] no --hook given; running unhooked (baseline)\n");
    }

    auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cmdList->ResourceBarrier(1, &b);
    };

    // Where the depth edges sit relative to the velocity edge.
    //
    // A UE5 frame settles scene depth in the prepass and base pass and only
    // then finishes velocity, so depth becomes readable FIRST - that is the
    // ordering the capture path optimises for, copying depth until the velocity
    // edge arrives and then stopping. But nothing guarantees it on a title
    // whose frame graph is arranged differently, and when the assumption fails
    // the failure is silent: velocity and colour still arrive and the dump
    // looks complete with no depth in it. So both orderings are modelled, and
    // whichever this run uses, a depth file has to reach disk.
    const bool depthBeforeVelocity = pipeline != "skyrunner";
    printf("[testhost] depth edges are emitted %s the velocity edge\n",
           depthBeforeVelocity ? "BEFORE (the UE5 prepass ordering)"
                               : "AFTER (the ordering that exercises the fallback)");

    // The depth line-up. Cleared through a DSV first so the placed memory holds
    // real bytes - GPU-based validation objects to reading uninitialised
    // placed-resource memory, and so it should.
    //
    // The real one gets TWO DEPTH_WRITE -> readable edges per frame, which is
    // the shape UE5 produces on a title with a partial prepass (depth settles
    // once after the prepass and again after the base pass). The capture path
    // is supposed to record a copy on each and let the last one before velocity
    // land; running it that way here is what makes that claim tested rather
    // than asserted.
    auto emitDepthEdges = [&]() {
        for (DepthLookAlike& target : depthLineUp) {
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvAt(target.dsvIndex);
            cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            cmdList->ClearDepthStencilView(
                dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.5f, 0, 0, nullptr);
            ID3D12Resource* resource = target.resource.Get();
            // Alternating readable states, because the real title uses both:
            // DEPTH_READ for the passes that still depth-test against it, and
            // the shader-resource states for the passes that sample it.
            for (int edge = 0; edge < target.edgesPerFrame; ++edge) {
                const D3D12_RESOURCE_STATES readable =
                    (edge % 2) == 0 ? D3D12_RESOURCE_STATE_DEPTH_READ
                                    : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barrier(resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, readable);
                barrier(resource, readable, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            }
        }
    };

    // Root constant buffer traffic, in UE5's proportions: the View uniform
    // buffer bound by every draw, a per-object buffer bound a handful of times.
    // Nothing is drawn - binding a root argument is the whole of what the hook
    // observes, and a draw would add a PSO and a pile of unrelated state for no
    // extra coverage.
    //
    // This moves with the depth edges, for the same reason. In a UE5 frame the
    // View uniform buffer is bound by every base-pass draw, i.e. before the
    // velocity buffer is finished, and the hook snapshots the bind counts at
    // the velocity barrier precisely so the constant buffer and the velocity
    // buffer come from the same point in the command stream. Emitting the binds
    // after that point instead exercises the fallback - and produced a dump
    // with zero View buffers in it the first time, which is how the fallback
    // came to exist.
    auto emitCbTraffic = [&]() {
        cmdList->SetGraphicsRootSignature(cbRootSignature.Get());
        for (int i = 0; i < kViewCbBindsPerFrame; ++i) {
            cmdList->SetGraphicsRootConstantBufferView(0, viewCb->GetGPUVirtualAddress());
        }
        for (int i = 0; i < kDecoyCbBindsPerFrame; ++i) {
            cmdList->SetGraphicsRootConstantBufferView(0, decoyCb->GetGPUVirtualAddress());
        }
    };

    int totalErrors = 0;

    for (int frame = 0; frame < frames; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const UINT index = swapChain->GetCurrentBackBufferIndex();
        allocators[index]->Reset();
        cmdList->Reset(allocators[index].Get(), nullptr);

        if (depthBeforeVelocity) {
            emitDepthEdges();
            emitCbTraffic();
        }

        for (LookAlike& target : lineUp) {
            // Write each look-alike as a render target, so it holds real bytes
            // rather than undefined memory - GPU-based validation objects to
            // reading uninitialised placed-resource memory, and so it should.
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvAt(target.rtvIndex);
            cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            cmdList->ClearRenderTargetView(rtv, kVelocityClear, 0, nullptr);

            ID3D12Resource* resource = target.resource.Get();
            switch (target.behaviour) {
                case Behaviour::Velocity:
                    // The transition the hook triggers on. Everything the
                    // capture path does - its own barriers, its
                    // CopyTextureRegion, its footprints - is recorded onto THIS
                    // command list, inside this pair of barriers, and is
                    // therefore fully covered by the debug layer.
                    barrier(resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    barrier(resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
                    break;
                case Behaviour::ComputeWritten: {
                    barrier(resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    D3D12_RESOURCE_BARRIER uav{};
                    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    uav.UAV.pResource = resource;
                    cmdList->ResourceBarrier(1, &uav);
                    barrier(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    barrier(resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
                    break;
                }
                case Behaviour::VelocityComputeAssisted:
                    // Transition counts and states taken from the real title.
                    // Note it passes through UNORDERED_ACCESS without ever
                    // issuing a UAV barrier, which is why "takes a UAV barrier"
                    // and "enters UNORDERED_ACCESS" are two separate signals
                    // rather than one - collapsing them would make this
                    // indistinguishable from the compute-written decoy.
                    barrier(resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    barrier(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    barrier(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
                    barrier(resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    barrier(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
                    break;
                case Behaviour::ExtraPass:
                    for (int pass = 0; pass < 2; ++pass) {
                        barrier(resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                        barrier(resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
                    }
                    break;
            }
        }

        if (!depthBeforeVelocity) {
            emitDepthEdges();
            emitCbTraffic();
        }

        // Something trivial on the back buffer, so Present has real content
        // and the overlay (if enabled) has something to composite over.
        barrier(backBuffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float t = static_cast<float>(frame) / static_cast<float>(frames);
        const float clear[4] = {0.1f + 0.4f * t, 0.15f, 0.5f - 0.3f * t, 1.0f};
        cmdList->ClearRenderTargetView(rtvAt(index), clear, 0, nullptr);
        barrier(backBuffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

        cmdList->Close();
        ID3D12CommandList* lists[] = {cmdList.Get()};
        queue->ExecuteCommandLists(1, lists);

        swapChain->Present(1, 0);

        // Serialise every frame. Slower than the real thing, but it means a
        // debug-layer message can be attributed to the frame that caused it.
        queue->Signal(fence.Get(), ++fenceValue);
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, 5000);
        }

        char phase[32];
        snprintf(phase, sizeof(phase), "frame%d", frame);
        totalErrors += DrainInfoQueue(infoQueue.Get(), phase);
    }

    // ---- Did it identify the right one? --------------------------------
    //
    // This is a separate question from "did the capture path upset the debug
    // layer", and it is the one the previous harness could not ask: with a
    // single decoy matching a hardcoded descriptor, "something was captured"
    // was the only available assertion, and that assertion was equally true
    // when the filter was selecting the wrong resource - which it did, for
    // days, during the R16G16B16A16_FLOAT/_UNORM episode.
    bool identificationPassed = true;
    if (hookModule != nullptr) {
        using IdentifiedFn = void*(*)();
        auto identified = reinterpret_cast<IdentifiedFn>(
            reinterpret_cast<void*>(GetProcAddress(hookModule, "MvIdentifiedVelocityResource")));
        if (identified == nullptr) {
            printf("\n[testhost] FAIL - mv_hook.dll does not export MvIdentifiedVelocityResource; "
                   "cannot check which resource was picked\n");
            identificationPassed = false;
        } else {
            void* picked = identified();
            const LookAlike* expected = nullptr;
            for (const LookAlike& target : lineUp) {
                if (target.shouldBeSelected) {
                    expected = &target;
                }
            }
            printf("\n[testhost] ==== identification ====\n");
            for (const LookAlike& target : lineUp) {
                const bool isPicked = picked == static_cast<void*>(target.resource.Get());
                printf("[testhost]   %-22s %p %s\n", target.name, static_cast<void*>(target.resource.Get()),
                       isPicked ? "<== SELECTED" : "");
            }
            if (ambiguous) {
                // Two indistinguishable candidates. The only correct answer is
                // to refuse: a ranked guess between them would be a coin toss
                // presented as an identification.
                identificationPassed = picked == nullptr;
                printf("[testhost] %s\n",
                       identificationPassed
                           ? "PASS - identification refused to pick between two indistinguishable candidates"
                           : "FAIL - identification picked one of two indistinguishable candidates instead of "
                             "reporting the ambiguity");
            } else if (expected != nullptr && picked == static_cast<void*>(expected->resource.Get())) {
                printf("[testhost] PASS - selected the velocity buffer out of %zu look-alikes, with no "
                       "hardcoded resolution or descriptor\n", lineUp.size());
            } else {
                printf("[testhost] FAIL - expected %p (%s), got %p\n",
                       expected != nullptr ? static_cast<void*>(expected->resource.Get()) : nullptr,
                       expected != nullptr ? expected->name : "?", picked);
                printf("[testhost]        (nullptr means identification refused or never concluded - "
                       "check %%TEMP%%\\mv_hook.log for the 'identify:' lines)\n");
                identificationPassed = false;
            }
        }
    }

    // ---- Did it identify the right depth buffer? ------------------------
    bool depthIdentificationPassed = true;
    if (hookModule != nullptr) {
        using IdentifiedFn = void*(*)();
        auto identifiedDepth = reinterpret_cast<IdentifiedFn>(
            reinterpret_cast<void*>(GetProcAddress(hookModule, "MvIdentifiedDepthResource")));
        if (identifiedDepth == nullptr) {
            printf("\n[testhost] FAIL - mv_hook.dll does not export MvIdentifiedDepthResource\n");
            depthIdentificationPassed = false;
        } else {
            void* picked = identifiedDepth();
            const DepthLookAlike* expected = nullptr;
            for (const DepthLookAlike& target : depthLineUp) {
                if (target.shouldBeSelected) {
                    expected = &target;
                }
            }
            printf("\n[testhost] ==== depth identification ====\n");
            for (const DepthLookAlike& target : depthLineUp) {
                const bool isPicked = picked == static_cast<void*>(target.resource.Get());
                printf("[testhost]   %-16s %p %s\n", target.name, static_cast<void*>(target.resource.Get()),
                       isPicked ? "<== SELECTED" : "");
            }
            if (ambiguous) {
                // Velocity identification refuses in this mode, and depth
                // identification is gated on the velocity extent, so the only
                // correct answer here is also to have picked nothing. Worth
                // asserting rather than skipping: a depth search that quietly
                // ran anyway would be keying off an extent nobody established.
                depthIdentificationPassed = picked == nullptr;
                printf("[testhost] %s\n",
                       depthIdentificationPassed
                           ? "PASS - no depth selected, because velocity was never identified to key off"
                           : "FAIL - depth was selected even though velocity identification refused");
            } else if (expected != nullptr && picked == static_cast<void*>(expected->resource.Get())) {
                printf("[testhost] PASS - selected scene depth out of %zu depth look-alikes\n",
                       depthLineUp.size());
            } else {
                printf("[testhost] FAIL - expected %p (%s), got %p\n",
                       expected != nullptr ? static_cast<void*>(expected->resource.Get()) : nullptr,
                       expected != nullptr ? expected->name : "?", picked);
                depthIdentificationPassed = false;
            }
        }
    }

    if (hookModule != nullptr) {
        // Drain the writer thread before counting dump files below - it
        // finishes writes on a background thread, so counting without this
        // first measures whatever fraction of the burst happened to reach
        // disk by the time we looked, not what capture actually produced.
        // The module is left loaded; a real game never unloads the hook
        // either, so there is nothing else to exercise here.
        using FlushCaptureFn = void (*)();
        auto flushCapture = reinterpret_cast<FlushCaptureFn>(
            reinterpret_cast<void*>(GetProcAddress(hookModule, "MvFlushCapture")));
        if (flushCapture != nullptr) {
            flushCapture();
        } else {
            printf("[testhost] WARNING: no MvFlushCapture export; dump file counts below may be racy\n");
        }
    }

    // ---- Did the capture actually write depth and the View buffer? ------
    //
    // "Identification picked the right resource" and "the bytes reached disk"
    // are different claims, and the gap between them is not theoretical. A
    // change that stopped recording depth copies once the velocity edge arrived
    // left this harness reporting PASS on both identification verdicts and zero
    // debug-layer errors, while producing a dump with no depth in it at all -
    // because here the depth edges come after the velocity edge. Nothing was
    // checking the files.
    //
    // This has to run AFTER the flush above, not before it. The writer thread is
    // still draining when the last frame is presented, so counting files any
    // earlier counts a directory that is still being written - which is exactly
    // what the first version of this check did, reporting a phantom off-by-one
    // that cost a round of chasing a capture bug that was not there.
    // Read the same way the hook reads it, so the harness and the thing it is
    // testing cannot disagree about what the switch is set to.
    bool depthCaptureEnabled = true;
    {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("MV_CAPTURE_DEPTH", value, sizeof(value));
        depthCaptureEnabled = !(length > 0 && length < sizeof(value) && std::string(value, length) == "0");
    }

    bool dumpContentsPassed = true;
    if (hookModule != nullptr && GetEnvironmentVariableA("MV_AUTOCAPTURE", nullptr, 0) != 0) {
        char dumpDir[MAX_PATH]{};
        if (GetEnvironmentVariableA("MV_DUMP_DIR", dumpDir, MAX_PATH) != 0) {
            std::string dir = dumpDir;
            if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') {
                dir += '\\';
            }
            auto count = [&](const char* pattern) {
                WIN32_FIND_DATAA found{};
                HANDLE h = FindFirstFileA((dir + pattern).c_str(), &found);
                if (h == INVALID_HANDLE_VALUE) {
                    return 0;
                }
                int n = 0;
                do {
                    ++n;
                } while (FindNextFileA(h, &found));
                FindClose(h);
                return n;
            };
            const int velocities = count("vel_*.bin");
            const int depths = count("depth_*.bin");
            const int viewCbs = count("viewcb_*.bin");
            printf("\n[testhost] ==== dump contents ====\n");
            printf("[testhost]   velocity frames: %d\n", velocities);
            printf("[testhost]   depth frames:    %d\n", depths);
            printf("[testhost]   View CB frames:  %d\n", viewCbs);
            if (ambiguous) {
                // Identification refuses to pick in this mode, so capturing
                // NOTHING is the correct outcome and an empty dump is the
                // assertion. A file here would mean the hook captured from a
                // resource it had just said it could not identify.
                dumpContentsPassed = velocities == 0 && depths == 0 && viewCbs == 0;
                printf("[testhost] %s\n",
                       dumpContentsPassed
                           ? "PASS - nothing was captured, which is correct when identification refused to pick"
                           : "FAIL - something was captured despite identification refusing to pick");
            } else if (GetEnvironmentVariableA("MV_CAPTURE_DEPTH", nullptr, 0) != 0 && !depthCaptureEnabled) {
                // MV_CAPTURE_DEPTH=0 is the bandwidth control test: the hook is
                // asked not to copy depth, so a dump with depth in it is the
                // failure and a dump without it is the pass. Asserted rather
                // than assumed, because "the switch did nothing" and "the switch
                // worked" produce the same console output otherwise - which is
                // the exact shape of the bug this harness exists to catch.
                dumpContentsPassed = velocities > 0 && depths == 0 && viewCbs == velocities;
                printf("[testhost] %s\n",
                       dumpContentsPassed
                           ? "PASS - MV_CAPTURE_DEPTH=0 suppressed depth and left the rest of the frame intact"
                           : "FAIL - MV_CAPTURE_DEPTH=0 did not produce a velocity+View dump with no depth");
            } else if (velocities > 0 && depths == velocities && viewCbs == velocities) {
                printf("[testhost] PASS - every captured frame has velocity, depth and a View constant buffer\n");
            } else {
                printf("[testhost] FAIL - the dump is missing one of the three per-frame artefacts\n");
                dumpContentsPassed = false;
            }
        }
    }

    printf("\n[testhost] ==== result ====\n");
    printf("[testhost] debug-layer errors during setup (before the hook loaded): %d\n", errorsBeforeHook);
    printf("[testhost] debug-layer errors during %d hooked frames:               %d\n", frames, totalErrors);
    printf("[testhost] %s\n", totalErrors == 0 ? "PASS - the capture path produced no debug-layer errors"
                                               : "FAIL - see the messages above");
    printf("[testhost] %s\n", identificationPassed ? "PASS - velocity identification"
                                                   : "FAIL - velocity identification");
    printf("[testhost] %s\n", depthIdentificationPassed ? "PASS - depth identification"
                                                        : "FAIL - depth identification");
    printf("[testhost] %s\n", dumpContentsPassed ? "PASS - dump contents"
                                                 : "FAIL - dump contents");

    CloseHandle(fenceEvent);
    DestroyWindow(hwnd);
    const int exitCode =
        (totalErrors == 0 && identificationPassed && depthIdentificationPassed && dumpContentsPassed) ? 0 : 1;
    printf("[testhost] exiting with code %d\n", exitCode);
    fflush(stdout);
    return exitCode;
}
