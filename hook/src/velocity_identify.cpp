#include "velocity_identify.h"

#include "logging.h"
#include "resource_tracking.h"

#include <windows.h>

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace mv
{

// Defined at the bottom of this file; called from OnFrameForIdentification.
void DumpBarrierProfiles();

namespace
{

// FVelocityRendering::GetFormat() return PF_A16B16G16R16 and PF_R16G16_UINT
// on Windows which map to D3D12's DXGI_FORMAT_R16G16_UNORM, and
// DXGI_FORMAT_R16G16B16A16_UNORM
struct FormatEntry
{
    DXGI_FORMAT format;
    int score; // desktop D3D12 path outranks the GLES-only fallbacks
    const char* name;
};

constexpr FormatEntry kVelocityFormats[] = {
    {DXGI_FORMAT_R16G16B16A16_UNORM, 4, "R16G16B16A16_UNORM (PF_A16B16G16R16, NeedVelocityDepth)"},
    {DXGI_FORMAT_R16G16_UNORM, 4, "R16G16_UNORM (PF_G16R16, no velocity depth)"},
    // Android GLES-only in the engine, so these should never appear on a D3D12 title.
    {DXGI_FORMAT_R16G16B16A16_UINT, 1, "R16G16B16A16_UINT (PF_R16G16B16A16_UINT, Android GLES path)"},
    {DXGI_FORMAT_R16G16_UINT, 1, "R16G16_UINT (PF_R16G16_UINT, Android GLES path)"},
};

// FVelocityRendering::GetCreateFlags: TexCreate_RenderTargetable | UAV |
// ShaderResource maps to ALLOW_RENDER_TARGET (0x01) | ALLOW_UNORDERED_ACCESS
// (0x04). The filter only requires those two bits set; an exact Flags == 5
// match scores higher than a superset with other flags.
constexpr UINT kRequiredFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

// Anything smaller than this is a downsampled/tile target. Deliberately
// generous - excludes 1/16-res things like MotionBlur.VelocityFlatten,
// doesn't try to guess the render resolution.
constexpr UINT64 kMinPlausibleWidth = 320;
constexpr UINT kMinPlausibleHeight = 180;

// Behaviour: how long to watch before deciding. The barrier signature is a
// per-frame invariant, not an average - 90 frames to be sure it's genuine.
constexpr uint64_t kDefaultSettleFrames = 90;
constexpr uint64_t kMinFramesObserved = 30;

// The survey dump (see README, "identifying by behaviour") stays on its
// original schedule: late enough that every resource has had thousands of
// frames to show its pattern.
constexpr uint64_t kProfileDumpFrame = 2000;

constexpr size_t kMaxProfiledResources = 20000;
constexpr size_t kMaxShortlisted = 64;

// ---------------------------------------------------------------------------

struct BarrierProfile
{
    UINT64 width = 0;
    UINT height = 0;
    int format = 0;
    unsigned flags = 0;
    uint64_t transitions = 0;
    uint64_t framesSeen = 0;
    uint64_t framesWithExactlyTwo = 0;
    uint64_t framesWithEvenCount = 0;
    uint64_t lastFrame = ~0ull;
    uint32_t eventsThisFrame = 0;
    uint64_t alternationViolations = 0;
    uint64_t rtToShaderResource = 0;
    uint64_t shaderResourceToRt = 0;
    uint64_t otherPairs = 0;
    uint64_t uavBarriers = 0;
    int lastAfter = -1;

    // the exact SRV state observed, see IsShaderResourceState
    int shaderResourceState = 0;

    // Whether the resource was ever transitioned into UNORDERED_ACCESS - the
    // discriminator separating velocity from compute-written targets that
    // share its descriptor. Tracked as evidence, not a rule.
    bool sawUnorderedAccess = false;

    // The actual state pairs seen.
    static constexpr int kMaxPairs = 12;
    struct PairCount
    {
        int before = 0;
        int after = 0;
        uint64_t count = 0;
    };
    PairCount pairs[kMaxPairs]{};
    int pairsUsed = 0;
};

// D3D12_RESOURCE_STATES is a bit field; these are the ones a render target
// realistically passes through. Names transcribed from d3d12.h.
std::string StateName(int state)
{
    if (state == 0)
    {
        return "COMMON/PRESENT";
    }
    struct Bit
    {
        int value;
        const char* name;
    };
    static constexpr Bit kBits[] = {
        {D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "VB_CB"},
        {D3D12_RESOURCE_STATE_INDEX_BUFFER, "IB"},
        {D3D12_RESOURCE_STATE_RENDER_TARGET, "RT"},
        {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "UAV"},
        {D3D12_RESOURCE_STATE_DEPTH_WRITE, "DEPTH_WRITE"},
        {D3D12_RESOURCE_STATE_DEPTH_READ, "DEPTH_READ"},
        {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "NON_PS_SRV"},
        {D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "PS_SRV"},
        {D3D12_RESOURCE_STATE_STREAM_OUT, "STREAM_OUT"},
        {D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, "INDIRECT"},
        {D3D12_RESOURCE_STATE_COPY_DEST, "COPY_DEST"},
        {D3D12_RESOURCE_STATE_COPY_SOURCE, "COPY_SRC"},
        {D3D12_RESOURCE_STATE_RESOLVE_DEST, "RESOLVE_DEST"},
        {D3D12_RESOURCE_STATE_RESOLVE_SOURCE, "RESOLVE_SRC"},
    };
    std::string out;
    int remaining = state;
    for (const Bit& bit : kBits)
    {
        if ((state & bit.value) == bit.value && bit.value != 0)
        {
            if (!out.empty())
            {
                out += "|";
            }
            out += bit.name;
            remaining &= ~bit.value;
        }
    }
    if (remaining != 0 || out.empty())
    {
        out += (out.empty() ? "" : "|") + std::string("0x") + std::to_string(remaining);
    }
    return out;
}

std::string PairsLine(const BarrierProfile& p)
{
    std::string out;
    for (int i = 0; i < p.pairsUsed; ++i)
    {
        out += " " + StateName(p.pairs[i].before) + "->" + StateName(p.pairs[i].after) + "=" +
               std::to_string(p.pairs[i].count);
    }
    return out.empty() ? " (none)" : out;
}

std::mutex g_profileMutex;
std::unordered_map<ID3D12Resource*, BarrierProfile> g_profiles;
std::atomic<bool> g_profileDumped{false};

// Structurally plausible resources. Small by construction (a handful), so
// the cost is negligible.
std::mutex g_shortlistMutex;
std::vector<ComPtr<ID3D12Resource>> g_shortlist;

// Histogram of every render-targetable 2D texture's size, so render
// resolution is *derived* rather than hardcoded. The modal resolution is a
// good prior (velocity shares its size with GBuffer/depth/most of the post
// chain) - used to rank survivors, never to exclude, since a heavily
// downscaled/upscaled pipeline could make the mode something else.
std::mutex g_resolutionMutex;
std::map<std::pair<UINT64, UINT>, int> g_resolutionHistogram;

std::atomic<uint64_t> g_backBufferWidth{0};
std::atomic<uint32_t> g_backBufferHeight{0};

std::atomic<ID3D12Resource*> g_identified{nullptr};
std::atomic<uint64_t> g_identifiedWidth{0};
std::atomic<uint32_t> g_identifiedHeight{0};
std::atomic<int> g_identifiedFormat{0};

// Output cap for the rival-edge diagnostic below - stalls recur every few
// seconds and would otherwise log forever; a few dozen lines is enough to
// tell whether a rival exists.
std::atomic<int> g_rivalReports{0};
constexpr int kMaxRivalReports = 40;

// Only look while the identified resource has actually stopped. One or two
// frames of gap is ordinary jitter; this is about the multi-frame stalls.
constexpr uint64_t kStallFramesBeforeRivalCheck = 3;

// A structurally-valid velocity texture on many consecutive frames, while the
// selected one takes none indicates the engine moved the pass.
constexpr uint64_t kRivalFramesBeforeAdopt = 8;
std::atomic<uint64_t> g_rivalRunLength{0};
std::atomic<uint64_t> g_lastRivalFrame{0};
ID3D12Resource* g_lastRivalResource = nullptr;

// The SECOND velocity target, when the engine is using two -
// FVelocityRendering::GetFormat returns a different width depending on
// NeedVelocityDepth() (PF_A16B16G16R16 vs PF_G16R16).
std::atomic<ID3D12Resource*> g_identifiedAlt{nullptr};
std::atomic<uint64_t> g_altWidth{0};
std::atomic<uint32_t> g_altHeight{0};
std::atomic<int> g_altFormat{0};

constexpr int kMaxAdoptions = 8;
std::atomic<int> g_adoptions{0};
std::atomic<bool> g_adoptLimitReported{false};
std::atomic<bool> g_decisionMade{false};
std::atomic<uint64_t> g_decideAtFrame{0};
std::atomic<int> g_attempt{0};

// Last presented frame the identified resource took its RT->SRV edge, and
// how long it may go without one before being reported silent.
//
// A menu/pause can silence the resource for hundreds of thousands of frames
// without it being lost - the same pointer can resume much later. So this
// only reports the gap, it does not reidentify.
//
// 300 frames (~3s at 100fps) is comfortably longer than ordinary hitches,
// short enough to still be useful as a diagnostic.
constexpr uint64_t kQuietFramesBeforeSilent = 300;
std::atomic<uint64_t> g_lastEdgeFrame{0};
std::atomic<bool> g_silenceReported{false};

std::string EnvOrEmpty(const char* name)
{
    char buffer[128]{};
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer))
    {
        return {};
    }
    return std::string(buffer, length);
}

uint64_t g_settleFrames = kDefaultSettleFrames;
std::once_flag g_overridesOnce;

// Called via std::call_once: runs once for the first caller (others stall
// until it finishes), then it's a no-op.
void ReadOverrides()
{
    const std::string frames = EnvOrEmpty("MV_IDENTIFY_FRAMES");
    if (!frames.empty())
    {
        const uint64_t parsed = strtoull(frames.c_str(), nullptr, 10);
        if (parsed > 0)
        {
            g_settleFrames = parsed;
            Log("identify: MV_IDENTIFY_FRAMES set, deciding after " + std::to_string(g_settleFrames) + " frames");
        }
    }
}

void ReadOverridesOnce()
{
    std::call_once(g_overridesOnce, ReadOverrides);
}

bool StructurallyPlausible(const D3D12_RESOURCE_DESC& desc, int* formatScore, const char** rejection)
{
    // Format tested first so *formatScore is set even on rejection - lets the
    // caller report near-misses (e.g. "velocity-shaped format, missing UAV flag").
    *formatScore = 0;
    *rejection = "";
    for (const FormatEntry& entry : kVelocityFormats)
    {
        if (desc.Format == entry.format)
        {
            *formatScore = entry.score;
            break;
        }
    }
    if (*formatScore == 0)
    {
        *rejection = "format is not one FVelocityRendering::GetFormat can return";
        return false;
    }

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        *rejection = "not a 2D texture";
        return false;
    }

    if (desc.MipLevels != 1)
    {
        *rejection = "mips != 1";
        return false;
    }

    if (desc.DepthOrArraySize != 1)
    {
        *rejection = "arraySize != 1";
        return false;
    }

    if (desc.SampleDesc.Count != 1)
    {
        *rejection = "sampleCount != 1";
        return false;
    }

    // D3D12 requires UNKNOWN for ordinary (non-cross-adapter, non-row-major)
    // textures, so this excludes occasional CPU-visible staging texture.
    if (desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN)
    {
        *rejection = "layout != UNKNOWN";
        return false;
    }

    if ((static_cast<UINT>(desc.Flags) & kRequiredFlags) != kRequiredFlags)
    {
        *rejection = "flags lack ALLOW_RENDER_TARGET|ALLOW_UNORDERED_ACCESS";
        return false;
    }

    if (desc.Width < kMinPlausibleWidth || desc.Height < kMinPlausibleHeight)
    {
        *rejection = "below plausible render resolution (tile/downsampled target)";
        return false;
    }

    // Bound size against the back buffer, but generously: "render target no
    // larger than back buffer" is false (measured 1707x1067 back buffer vs
    // 1708x1068 SceneVelocity - UE rounds extents up, and screen percentage
    // above 100 is also legitimate). 2x linear still excludes what this is
    // for - 4096x4096 shadow/lightmap atlases - while admitting rounding and
    // supersampling.
    const UINT64 bbWidth = g_backBufferWidth.load(std::memory_order_relaxed);
    const UINT bbHeight = g_backBufferHeight.load(std::memory_order_relaxed);
    if (bbWidth != 0 && (desc.Width > bbWidth * 2 || desc.Height > bbHeight * 2))
    {
        *rejection = "more than 2x the back buffer in a dimension";
        return false;
    }
    return true;
}

std::string DescLine(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc)
{
    return std::to_string(reinterpret_cast<uintptr_t>(resource)) + " " + std::to_string(desc.Width) + "x" +
           std::to_string(desc.Height) + " fmt=" + std::to_string(static_cast<int>(desc.Format)) +
           " mips=" + std::to_string(desc.MipLevels) + " arraySize=" + std::to_string(desc.DepthOrArraySize) +
           " samples=" + std::to_string(desc.SampleDesc.Count) +
           " flags=" + std::to_string(static_cast<unsigned>(desc.Flags)) +
           " layout=" + std::to_string(static_cast<int>(desc.Layout));
}

std::string ProfileLine(const BarrierProfile& p)
{
    return "framesSeen=" + std::to_string(p.framesSeen) + " exactly2=" + std::to_string(p.framesWithExactlyTwo) +
           " even=" + std::to_string(p.framesWithEvenCount) + " rt->srv=" + std::to_string(p.rtToShaderResource) +
           " srv->rt=" + std::to_string(p.shaderResourceToRt) + " otherPairs=" + std::to_string(p.otherPairs) +
           " violations=" + std::to_string(p.alternationViolations) + " uavBarriers=" + std::to_string(p.uavBarriers) +
           " srvState=" + std::to_string(p.shaderResourceState);
}

// ---------------------------------------------------------------------------
// Deciding, in two parts: what velocity MUST look like, and what makes one
// candidate more velocity-like than another.
//
// 1. Structure: a hard gate, since it's a necessary condition read from the
// engine's own code - UE5 can't produce velocity outside four formats with
// TexCreate_RenderTargetable|UAV.
//
// 2. Behaviour: scored evidence, since every behavioural property observed
// so far turned out to describe one game's frame graph rather than
// velocity itself - except one, which stays a gate: produced as a render
// target and consumed as a shader resource, every frame.

std::pair<UINT64, UINT> ModalResolution()
{
    std::lock_guard<std::mutex> lock(g_resolutionMutex);
    std::pair<UINT64, UINT> best{0, 0};
    int bestCount = 0;
    for (const auto& [size, count] : g_resolutionHistogram)
    {
        if (count > bestCount)
        {
            bestCount = count;
            best = size;
        }
    }
    return best;
}

// Outputs top 6 most common RTs with the format [width]x[height]=[count]
void LogResolutionHistogram()
{
    std::vector<std::pair<int, std::pair<UINT64, UINT>>> byCount;
    {
        std::lock_guard<std::mutex> lock(g_resolutionMutex);
        for (const auto& [size, count] : g_resolutionHistogram)
        {
            byCount.push_back({count, size});
        }
    }
    std::sort(byCount.rbegin(), byCount.rend());
    std::string line = "identify: render-target resolution histogram (top 6, derived not assumed):";
    for (size_t i = 0; i < byCount.size() && i < 6; ++i)
    {
        line += " " + std::to_string(byCount[i].second.first) + "x" + std::to_string(byCount[i].second.second) + "=" +
                std::to_string(byCount[i].first);
    }
    Log(line);
}

struct Signal
{
    const char* name;
    int weight;
    bool present;
};

// Necessary behavioural condition. Deliberately weak: "it gets written as a
// render target and then read, most frames". Anything stronger has already been
// falsified once.
bool BehaviourGate(const BarrierProfile& p)
{
    if (p.framesSeen < kMinFramesObserved)
    {
        return false;
    }
    // At least ~one produce->consume edge per frame. Half is the threshold so a
    // title that skips velocity on some frames (paused, menu, occluded) still
    // passes, while a resource that is merely render-targetable does not.
    return p.rtToShaderResource * 2 >= p.framesSeen;
}

void CollectSignals(
    const D3D12_RESOURCE_DESC& desc,
    const BarrierProfile& p,
    int formatScore,
    const std::pair<UINT64, UINT>& modal,
    Signal* out,
    int* count,
    int* total)
{
    const double perFrame = p.framesSeen > 0 ? static_cast<double>(p.rtToShaderResource) / p.framesSeen : 0.0;
    int n = 0;
    // Structural evidence. Format carries the most weight because it is the
    // narrowest engine-derived fact available.
    out[n++] = {"format is a desktop velocity format", formatScore, formatScore >= 4};
    out[n++] = {"at the modal render-target resolution", 3, desc.Width == modal.first && desc.Height == modal.second};
    out[n++] = {"flags are exactly RT|UAV", 2, static_cast<UINT>(desc.Flags) == kRequiredFlags};
    // Behavioural evidence, weakest-to-strongest in terms of how much it told us
    // on the two titles measured.
    out[n++] = {"produced once per frame (one producer)", 2, perFrame >= 0.9 && perFrame <= 1.1};
    out[n++] = {"never takes a UAV barrier", 2, p.uavBarriers == 0};
    out[n++] = {"never enters UNORDERED_ACCESS", 2, !p.sawUnorderedAccess};
    out[n++] = {
        "strict 2 events every frame (Oxi shape)", 3, p.framesSeen > 0 && p.framesWithExactlyTwo == p.framesSeen};
    out[n++] = {"no state-continuity violations", 1, p.alternationViolations == 0};
    *count = n;
    *total = 0;
    for (int i = 0; i < n; ++i)
    {
        if (out[i].present)
        {
            *total += out[i].weight;
        }
    }
}

struct Survivor
{
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_DESC desc{};
    BarrierProfile profile;
    Signal signals[12]{};
    int signalCount = 0;
    int score = 0;
};

// Prints the evidence, not only the verdict. Someone reading the log can then
// see which signals fired and disagree with the weighting
std::string SignalsLine(const Survivor& s)
{
    std::string out;
    for (int i = 0; i < s.signalCount; ++i)
    {
        out += std::string("\n identify:       [") + (s.signals[i].present ? "x" : " ") + "] +" +
               std::to_string(s.signals[i].weight) + "  " + s.signals[i].name;
    }
    return out;
}

void Decide(uint64_t frame)
{
    ReadOverridesOnce();

    std::vector<ComPtr<ID3D12Resource>> shortlist;
    {
        std::lock_guard<std::mutex> lock(g_shortlistMutex);
        shortlist = g_shortlist;
    }

    LogResolutionHistogram();
    const auto modal = ModalResolution();
    Log("identify: modal render-target resolution " + std::to_string(modal.first) + "x" + std::to_string(modal.second) +
        "; back buffer " + std::to_string(g_backBufferWidth.load()) + "x" + std::to_string(g_backBufferHeight.load()) +
        "; " + std::to_string(shortlist.size()) + " structurally plausible resource(s) after " + std::to_string(frame) +
        " frames");

    std::vector<Survivor> survivors;
    for (const ComPtr<ID3D12Resource>& held : shortlist)
    {
        ID3D12Resource* resource = held.Get();
        BarrierProfile profile;
        {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            const auto it = g_profiles.find(resource);
            if (it == g_profiles.end())
            {
                continue;
            }
            profile = it->second;
        }

        if (!BehaviourGate(profile))
        {
            continue;
        }

        Survivor s;
        s.resource = resource;
        // Safe to call GetDesc(): the shortlist holds a reference.
        s.desc = resource->GetDesc();
        s.profile = profile;
        int formatScore = 0;
        const char* rejection = "";
        if (!StructurallyPlausible(s.desc, &formatScore, &rejection))
        {
            // desc changed since shortlisting
            continue;
        }
        CollectSignals(s.desc, profile, formatScore, modal, s.signals, &s.signalCount, &s.score);
        survivors.push_back(s);
    }

    if (survivors.empty())
    {
        // Not a failure yet: the settling window may simply have been too short,
        // or the title may not have rendered anything interesting during it.
        // Widen and try again rather than giving up for the session.
        const int attempt = g_attempt.fetch_add(1) + 1;
        Log("identify: NO candidate after " + std::to_string(frame) + " frames (attempt " + std::to_string(attempt) +
            "). Structural shortlist had " + std::to_string(shortlist.size()) +
            " entries; none of them was written as a render target and then read as a shader resource on at "
            "least half the frames. Retrying with a longer window. If this repeats, either the title was not "
            "rendering a scene during the window (a menu will do this), or its velocity pass does not look like "
            "anything measured so far - read the observed transitions below and mv_barrier_profile.log.");
        for (const ComPtr<ID3D12Resource>& held : shortlist)
        {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            const auto it = g_profiles.find(held.Get());
            if (it != g_profiles.end())
            {
                Log("identify:   shortlisted " + DescLine(held.Get(), held->GetDesc()) + " | " +
                    ProfileLine(it->second));
                Log("identify:     observed transitions:" + PairsLine(it->second));
            }
        }
        // Measure the NEXT window fresh rather than continuing to accumulate.
        // The strict test ("framesWithExactlyTwo == framesSeen") is
        // all-or-nothing and counters are cumulative, so a stretch where
        // velocity behaves differently (e.g. injecting at a main menu, which
        // renders no scene) poisons the test permanently - identification
        // would fail forever with no more explanation than "none showed a
        // clean cycle".
        //
        // Resetting keeps the population/descs and restarts measurement, so
        // each attempt is a clean window. The barrier survey dumped at frame
        // 2000 then describes the last window, not the whole session.
        {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            for (auto& [resource, p] : g_profiles)
            {
                const UINT64 width = p.width;
                const UINT height = p.height;
                const int format = p.format;
                const unsigned flags = p.flags;
                p = BarrierProfile{};
                p.width = width;
                p.height = height;
                p.format = format;
                p.flags = flags;
            }
        }
        g_decideAtFrame.store(frame + g_settleFrames * 2, std::memory_order_relaxed);
        return;
    }

    std::sort(
        survivors.begin(), survivors.end(), [](const Survivor& a, const Survivor& b) { return a.score > b.score; });

    // Always dump every survivor, whether or not the pick is unambiguous.
    Log("identify: " + std::to_string(survivors.size()) +
        " resource(s) passed the structural gate and the produce-then-consume test. Evidence for each:");
    for (const Survivor& s : survivors)
    {
        Log("identify:   score=" + std::to_string(s.score) + " " + DescLine(s.resource, s.desc) + " | " +
            ProfileLine(s.profile) + SignalsLine(s));
        Log("identify:     observed transitions:" + PairsLine(s.profile));
    }

    if (survivors.size() > 1)
    {
        const int margin = survivors[0].score - survivors[1].score;
        if (margin < 2)
        {
            Log("identify: AMBIGUOUS - the top two survivors score " + std::to_string(survivors[0].score) + " and " +
                std::to_string(survivors[1].score) +
                ", which is not a large enough margin to pick between them. Refusing to capture rather than "
                "guessing: a wrong pick produces a dump that decodes to plausible garbage, which is the most "
                "expensive failure mode this project has. Take a RenderDoc capture and re-run.");
            g_decisionMade.store(true, std::memory_order_relaxed);
            return;
        }
        Log("identify: picking the top-scoring survivor by a margin of " + std::to_string(margin) +
            " points. This is a ranked choice among several matches, NOT the unique match the README describes for "
            "the original title.");
    }

    const Survivor& winner = survivors.front();
    MarkAsCandidate(winner.resource, winner.desc);
    g_identifiedWidth.store(winner.desc.Width, std::memory_order_relaxed);
    g_identifiedHeight.store(winner.desc.Height, std::memory_order_relaxed);
    g_identifiedFormat.store(static_cast<int>(winner.desc.Format), std::memory_order_relaxed);
    g_rivalReports.store(0, std::memory_order_relaxed);
    // Start the liveness clock from the decision, not from zero - otherwise a
    // decision reached at frame 90 is already "silent" the moment it is made.
    g_lastEdgeFrame.store(frame, std::memory_order_relaxed);
    g_silenceReported.store(false, std::memory_order_relaxed);
    g_identified.store(winner.resource, std::memory_order_release);
    g_decisionMade.store(true, std::memory_order_relaxed);
    Log("identify: SELECTED " + DescLine(winner.resource, winner.desc) + " | score=" + std::to_string(winner.score) +
        " | survivors=" + std::to_string(survivors.size()));
    if (survivors.size() == 1)
    {
        // Worth spelling out since it differs between titles: on Oxi the
        // barrier signature was decisive and format broke a tie; here it's
        // the reverse, and the log shouldn't make the reader infer which.
        Log("identify: this was the ONLY resource in the process that UE5 could have created as a velocity "
            "texture (one of four formats, RT|UAV, single mip/slice/sample) AND that is produced as a render "
            "target then read as a shader resource each frame. The behavioural signals above are corroboration; "
            "they are not what made it unique.");
    }
}

} // namespace

bool NoteBackBufferSize(uint64_t width, uint32_t height)
{
    const uint64_t previousWidth = g_backBufferWidth.exchange(width, std::memory_order_relaxed);
    const uint32_t previousHeight =
        g_backBufferHeight.exchange(static_cast<uint32_t>(height), std::memory_order_relaxed);

    // A swapchain resize makes UE rebuild scene textures at a new extent. The
    // selected velocity resource is abandoned and should be reopened
    const bool changed = previousWidth != 0 && previousHeight != 0 &&
                         (previousWidth != width || previousHeight != static_cast<uint32_t>(height));
    if (!changed)
    {
        return false;
    }
    if (g_identified.load(std::memory_order_acquire) == nullptr && !g_decisionMade.load(std::memory_order_relaxed))
    {
        // Nothing decided yet; the search will pick this up on its own.
        return false;
    }
    Log("identify: back buffer resized " + std::to_string(previousWidth) + "x" + std::to_string(previousHeight) +
        " -> " + std::to_string(width) + "x" + std::to_string(height) +
        ". UE rebuilds its scene textures at the new extent, so the selected velocity resource is abandoned "
        "rather than changed - its own descriptor stays valid, which is why nothing else here notices.");
    return true;
}

void NoteResourceDesc(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc)
{
    ReadOverridesOnce();

    // Every resource gets a behaviour profile, whatever its descriptor - that
    // is what makes the survey a survey rather than a confirmation.
    {
        std::lock_guard<std::mutex> lock(g_profileMutex);
        if (g_profiles.size() < kMaxProfiledResources && g_profiles.find(resource) == g_profiles.end())
        {
            BarrierProfile p;
            p.width = desc.Width;
            p.height = desc.Height;
            p.format = static_cast<int>(desc.Format);
            p.flags = static_cast<unsigned>(desc.Flags);
            g_profiles.emplace(resource, p);
        }
    }

    // The resolution histogram is over render-targetable 2D textures only, no buffers or staging textures
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        (static_cast<UINT>(desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0)
    {
        std::lock_guard<std::mutex> lock(g_resolutionMutex);
        ++g_resolutionHistogram[{desc.Width, desc.Height}];
    }

    int formatScore = 0;
    const char* rejection = "";
    if (!StructurallyPlausible(desc, &formatScore, &rejection))
    {
        if (formatScore > 0)
        {
            // A velocity-shaped format that failed some other test - worth a
            // line each, since these are what to check first if the search
            // finds nothing.
            Log(std::string("identify: near-miss ") + DescLine(resource, desc) + " - rejected: " + rejection);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(g_shortlistMutex);
    if (g_shortlist.size() >= kMaxShortlisted)
    {
        return;
    }

    g_shortlist.emplace_back(resource); // ComPtr ctor AddRefs
    Log("identify: shortlisted (structure) " + DescLine(resource, desc) + " - now watching its barriers");
}

void NoteTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter)
{
    // Liveness of the selected resource, tracked outside the profile map
    // since the map clears on reopen and this must keep working across
    // that. Only the produce-then-consume edge counts.
    const bool isTrackedVelocity = resource == g_identified.load(std::memory_order_acquire) ||
                                   resource == g_identifiedAlt.load(std::memory_order_acquire);
    if (isTrackedVelocity && stateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET && IsShaderResourceState(stateAfter))
    {
        g_lastEdgeFrame.store(frame, std::memory_order_relaxed);
        // Stall is over, so any rival run in progress wasn't a replacement.
        // Re-arm the output cap too - it's capped per episode, not per
        // session, or a later replacement goes unlogged.
        g_rivalRunLength.store(0, std::memory_order_relaxed);
        g_rivalReports.store(0, std::memory_order_relaxed);
        if (g_silenceReported.exchange(false))
        {
            Log("identify: the velocity pass has RESUMED on frame " + std::to_string(frame) +
                " - on one of the tracked targets, so nothing was reidentified.");
        }
    }

    std::lock_guard<std::mutex> lock(g_profileMutex);
    const auto it = g_profiles.find(resource);
    if (it == g_profiles.end())
    {
        return;
    }

    BarrierProfile& p = it->second;
    if (p.lastFrame != frame)
    {
        if (p.lastFrame != ~0ull)
        {
            ++p.framesSeen;
            if (p.eventsThisFrame == 2)
            {
                ++p.framesWithExactlyTwo;
            }
            if (p.eventsThisFrame % 2 == 0)
            {
                ++p.framesWithEvenCount;
            }
        }
        p.eventsThisFrame = 0;
        p.lastFrame = frame;
    }
    ++p.eventsThisFrame;
    ++p.transitions;

    bool recorded = false;
    for (int i = 0; i < p.pairsUsed; ++i)
    {
        if (p.pairs[i].before == stateBefore && p.pairs[i].after == stateAfter)
        {
            ++p.pairs[i].count;
            recorded = true;
            break;
        }
    }

    if (!recorded && p.pairsUsed < BarrierProfile::kMaxPairs)
    {
        p.pairs[p.pairsUsed].before = stateBefore;
        p.pairs[p.pairsUsed].after = stateAfter;
        p.pairs[p.pairsUsed].count = 1;
        ++p.pairsUsed;
    }

    if (stateBefore == D3D12_RESOURCE_STATE_UNORDERED_ACCESS || stateAfter == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        p.sawUnorderedAccess = true;
    }

    if (stateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET && IsShaderResourceState(stateAfter))
    {
        ++p.rtToShaderResource;
        p.shaderResourceState = stateAfter;
    }
    else if (IsShaderResourceState(stateBefore) && stateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        ++p.shaderResourceToRt;
        p.shaderResourceState = stateBefore;
    }
    else
    {
        ++p.otherPairs;
    }

    // State continuity: whatever a resource transitioned *to* last time must be
    // what it transitions *from* next time, or someone else moved it.
    if (p.lastAfter >= 0 && stateBefore != p.lastAfter)
    {
        ++p.alternationViolations;
    }
    p.lastAfter = stateAfter;
}

void NoteUavBarrier(ID3D12Resource* resource)
{
    std::lock_guard<std::mutex> lock(g_profileMutex);
    const auto it = g_profiles.find(resource);
    if (it != g_profiles.end())
    {
        ++it->second.uavBarriers;
    }
}

// Per-frame driver: arms the settle deadline on first use, calls Decide()
// once it's reached, and fires the one-time barrier-profile dump at kProfileDumpFrame.
void OnFrameForIdentification(uint64_t frame)
{
    ReadOverridesOnce();
    if (g_decideAtFrame.load(std::memory_order_relaxed) == 0)
    {
        // Relative to now, not to frame 0 - after ReopenIdentification the
        // session is already thousands of frames in, and an absolute deadline
        // of g_settleFrames would be long past, so the retry would decide
        // immediately on an empty profile set.
        g_decideAtFrame.store(frame + g_settleFrames, std::memory_order_relaxed);
    }
    if (!g_decisionMade.load(std::memory_order_relaxed) && frame >= g_decideAtFrame.load(std::memory_order_relaxed))
    {
        Decide(frame);
    }
    if (frame == kProfileDumpFrame)
    {
        DumpBarrierProfiles();
    }
}

void ReopenIdentification(const char* reason)
{
    Log(std::string("identify: REOPENING the search - ") + reason +
        ". The previously selected resource is no longer the thing it was identified as, so anything captured from "
        "it now would be sized from a stale footprint. Nothing is captured until a new decision is reached.");
    g_identified.store(nullptr, std::memory_order_release);
    g_identifiedAlt.store(nullptr, std::memory_order_release);
    g_altWidth.store(0, std::memory_order_relaxed);
    g_altHeight.store(0, std::memory_order_relaxed);
    g_altFormat.store(0, std::memory_order_relaxed);
    g_rivalRunLength.store(0, std::memory_order_relaxed);
    g_lastRivalResource = nullptr;
    g_identifiedWidth.store(0, std::memory_order_relaxed);
    g_identifiedHeight.store(0, std::memory_order_relaxed);
    g_decisionMade.store(false, std::memory_order_relaxed);
    g_decideAtFrame.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_shortlistMutex);
        g_shortlist.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_profileMutex);
        g_profiles.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_resolutionMutex);
        g_resolutionHistogram.clear();
    }
    // Without this the whole search is a no-op second time round: the "seen"
    // set exists to make sure GetDesc() is called once per resource, so every
    // resource in the process is already marked and NoteResourceDesc would
    // never be reached again.
    ForgetAllResourcesSeen();
}

bool VelocityPassIsSilent(uint64_t frame, uint64_t* framesSilent)
{
    if (g_identified.load(std::memory_order_acquire) == nullptr)
    {
        return false;
    }

    const uint64_t last = g_lastEdgeFrame.load(std::memory_order_relaxed);
    if (frame <= last + kQuietFramesBeforeSilent)
    {
        return false;
    }
    if (framesSilent != nullptr)
    {
        *framesSilent = frame - last;
    }
    // Log once per silence episode, not once per frame while it persists -
    // a menu can hold this silent for hundreds of thousands of frames.
    if (!g_silenceReported.exchange(true))
    {
        Log("identify: the velocity pass has STOPPED. The identified resource has not taken a "
            "RENDER_TARGET -> shader-resource edge for " +
            std::to_string(frame - last) + " presented frames (last on frame " + std::to_string(last) + ", now at " +
            std::to_string(frame) +
            "), while the game carries on presenting. That is what a menu, a "
            "pause, or the end of a run looks like from in here - the scene is no longer being drawn. "
            "It is NOT a reason to reidentify: on the one title where this was measured the same "
            "resource resumed 271,123 frames later.");
    }
    return true;
}

ID3D12Resource* IdentifiedVelocityResource()
{
    return g_identified.load(std::memory_order_acquire);
}

uint64_t FramesSinceVelocityEdge(uint64_t frame)
{
    if (g_identified.load(std::memory_order_acquire) == nullptr)
    {
        return 0;
    }
    const uint64_t last = g_lastEdgeFrame.load(std::memory_order_relaxed);
    return frame > last ? frame - last : 0;
}

bool NoteRivalVelocityEdge(ID3D12Resource* resource, uint64_t frame)
{
    // Cheapest checks first: this runs from the barrier hook, on every
    // RENDER_TARGET -> shader-resource edge in the frame.
    ID3D12Resource* selected = g_identified.load(std::memory_order_acquire);
    if (selected == nullptr || resource == selected || resource == nullptr ||
        resource == g_identifiedAlt.load(std::memory_order_acquire))
    {
        return false;
    }
    if (FramesSinceVelocityEdge(frame) < kStallFramesBeforeRivalCheck)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();

    // Judged by the SAME structural filter identification uses
    int formatScore = 0;
    const char* rejection = "";
    if (!StructurallyPlausible(desc, &formatScore, &rejection))
    {
        return false;
    }

    // Same render extent as the established answer - both widths of
    // SceneVelocity share SceneTexturesConfig::Extent, so a velocity-shaped
    // texture at a different extent is some other buffer.
    if (desc.Width != g_identifiedWidth.load(std::memory_order_relaxed) ||
        desc.Height != g_identifiedHeight.load(std::memory_order_relaxed))
    {
        return false;
    }

    // Run of CONSECUTIVE frames on which THIS resource carried the edge,
    // counted once per frame however many barriers that frame contains. Keyed
    // on the resource as well as the frame: two different rivals alternating
    // must not add up to a run.
    const uint64_t previous = g_lastRivalFrame.exchange(frame, std::memory_order_relaxed);
    const bool sameResource = (resource == g_lastRivalResource);
    g_lastRivalResource = resource;

    uint64_t run = g_rivalRunLength.load(std::memory_order_relaxed);
    if (sameResource && frame == previous)
    {
        // Same frame, another barrier - already counted.
    }
    else if (sameResource && frame == previous + 1)
    {
        run += 1;
        g_rivalRunLength.store(run, std::memory_order_relaxed);
    }
    else
    {
        run = 1;
        g_rivalRunLength.store(run, std::memory_order_relaxed);
    }

    if (run >= kRivalFramesBeforeAdopt)
    {
        g_rivalRunLength.store(0, std::memory_order_relaxed);
        if (g_adoptions.fetch_add(1, std::memory_order_relaxed) >= kMaxAdoptions)
        {
            if (!g_adoptLimitReported.exchange(true))
            {
                Log("identify: a further velocity target would have been adopted, but the limit of " +
                    std::to_string(kMaxAdoptions) +
                    " is spent - NOT adopting. Adopting without bound cannot be told apart from adopting "
                    "the wrong resources, so this stops and says so instead. Restart to reset it.");
            }
            return false;
        }

        g_identifiedAlt.store(resource, std::memory_order_release);
        g_altWidth.store(desc.Width, std::memory_order_relaxed);
        g_altHeight.store(desc.Height, std::memory_order_relaxed);
        g_altFormat.store(static_cast<int>(desc.Format), std::memory_order_relaxed);
        Log("identify: ADOPTED a second velocity target - " + DescLine(resource, desc) +
            " has taken the "
            "RENDER_TARGET -> shader-resource edge on " +
            std::to_string(run) + " consecutive frames while the selected one (fmt=" +
            std::to_string(g_identifiedFormat.load(std::memory_order_relaxed)) +
            ") took none. Same extent, same structural filter, different width: this is "
            "FVelocityRendering::GetFormat returning the other of its two answers because "
            "NeedVelocityDepth() changed at runtime. BOTH are now captured from, whichever takes the edge - "
            "the pass moved, it did not stop.");
        return true;
    }

    if (g_rivalReports.load(std::memory_order_relaxed) >= kMaxRivalReports)
    {
        return false;
    }
    const int n = g_rivalReports.fetch_add(1, std::memory_order_relaxed);
    if (n >= kMaxRivalReports)
    {
        return false;
    }
    Log("identify: RIVAL velocity-format edge while the identified resource is stalled - ptr=" +
        std::to_string(reinterpret_cast<uintptr_t>(resource)) + " " + std::to_string(desc.Width) + "x" +
        std::to_string(desc.Height) + " fmt=" + std::to_string(static_cast<int>(desc.Format)) +
        " took RENDER_TARGET -> shader-resource on frame " + std::to_string(frame) + ", " +
        std::to_string(FramesSinceVelocityEdge(frame)) + " frames into the stall. The identified resource is " +
        std::to_string(g_identifiedWidth.load(std::memory_order_relaxed)) + "x" +
        std::to_string(g_identifiedHeight.load(std::memory_order_relaxed)) +
        ". If this repeats, the engine moved the velocity pass to another target rather than "
        "stopping it, and the search must reopen rather than wait.");
    if (n + 1 == kMaxRivalReports)
    {
        Log("identify: further rival-edge reports suppressed (cap " + std::to_string(kMaxRivalReports) +
            " per stall episode, re-armed when the pass resumes). Absence of more lines is the cap, not the "
            "absence of rivals.");
    }
    return false;
}

bool IdentifiedVelocityExtent(uint64_t* width, uint32_t* height)
{
    if (g_identified.load(std::memory_order_acquire) == nullptr)
    {
        return false;
    }
    *width = g_identifiedWidth.load(std::memory_order_relaxed);
    *height = g_identifiedHeight.load(std::memory_order_relaxed);
    return *width != 0 && *height != 0;
}

void DumpBarrierProfiles()
{
    if (g_profileDumped.exchange(true))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_profileMutex);
    LogTo(
        "barrier_profile",
        "# ptr width height format flags transitions framesSeen framesWithExactlyTwo "
        "alternationViolations rtToShaderResource shaderResourceToRt otherPairs uavBarriers");
    for (const auto& [resource, p] : g_profiles)
    {
        LogTo(
            "barrier_profile",
            std::to_string(reinterpret_cast<uintptr_t>(resource)) + " " + std::to_string(p.width) + " " +
                std::to_string(p.height) + " " + std::to_string(p.format) + " " + std::to_string(p.flags) + " " +
                std::to_string(p.transitions) + " " + std::to_string(p.framesSeen) + " " +
                std::to_string(p.framesWithExactlyTwo) + " " + std::to_string(p.alternationViolations) + " " +
                std::to_string(p.rtToShaderResource) + " " + std::to_string(p.shaderResourceToRt) + " " +
                std::to_string(p.otherPairs) + " " + std::to_string(p.uavBarriers));
    }
    Log("barrier survey: dumped " + std::to_string(g_profiles.size()) + " resource profiles to mv_barrier_profile.log");
    // LogTo batches; the survey is written once and never again, so without an
    // explicit flush the tail of it sits in memory until the process exits.
    FlushLogs();
}

} // namespace mv
