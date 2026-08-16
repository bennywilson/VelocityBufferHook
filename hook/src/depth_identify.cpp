#include "depth_identify.h"

#include "logging.h"
#include "velocity_identify.h"

#include <windows.h>

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace mv
{
namespace
{

// ---------------------------------------------------------------------------
// Structure: what UE5 itself will accept as scene depth.
//
// GPixelFormats[PF_DepthStencil].PlatformFormat (D3D12RHI.cpp:117/127) is one
// of exactly two things, chosen by the r.D3D12.UseD24 cvar:
//
//     UseD24 != 0 : DXGI_FORMAT_R24G8_TYPELESS      (44)
//     UseD24 == 0 : DXGI_FORMAT_R32G8X24_TYPELESS   (19)
//
// Both are TYPELESS (UE puts a typed view on top) and MULTI-PLANE (depth in
// plane 0, stencil in plane 1) - capture.cpp already derives plane/subresource
// counts from the desc rather than assuming one.
//
// Fully-typed and single-plane variants are listed below but unused.
struct FormatEntry
{
    DXGI_FORMAT format;
    int score;
    const char* name;
};

constexpr FormatEntry kDepthFormats[] = {
    {DXGI_FORMAT_R24G8_TYPELESS, 4, "R24G8_TYPELESS (PF_DepthStencil, r.D3D12.UseD24=1)"},
    {DXGI_FORMAT_R32G8X24_TYPELESS, 4, "R32G8X24_TYPELESS (PF_DepthStencil, r.D3D12.UseD24=0)"},
    {DXGI_FORMAT_D24_UNORM_S8_UINT, 2, "D24_UNORM_S8_UINT (typed - not how UE creates it)"},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 2, "D32_FLOAT_S8X24_UINT (typed - not how UE creates it)"},
    {DXGI_FORMAT_R32_TYPELESS, 1, "R32_TYPELESS (depth without stencil)"},
    {DXGI_FORMAT_D32_FLOAT, 1, "D32_FLOAT (depth without stencil, typed)"},
};

// TexCreate_DepthStencilTargetable | TexCreate_ShaderResource maps to
// ALLOW_DEPTH_STENCIL with no DENY_SHADER_RESOURCE; requiring the deny bit
// absent separates scene depth from shadow-map atlases (TargetableOnly).
constexpr UINT kRequiredFlags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
constexpr UINT kForbiddenFlags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

constexpr uint64_t kMinFramesObserved = 30;
constexpr size_t kMaxShortlisted = 32;

struct DepthProfile
{
    uint64_t framesSeen = 0;
    uint64_t lastFrame = ~0ull;
    uint64_t depthWriteToReadable = 0;
    uint64_t transitions = 0;
};

std::mutex g_mutex;
std::unordered_map<ID3D12Resource*, DepthProfile> g_profiles;
std::vector<ComPtr<ID3D12Resource>> g_shortlist;

std::atomic<ID3D12Resource*> g_identified{nullptr};
std::atomic<bool> g_decisionMade{false};
std::atomic<uint64_t> g_decideAtFrame{0};
D3D12_RESOURCE_DESC g_identifiedDesc{};

// How long to watch before deciding, measured from when velocity produces
// an extent (depth identification can't start before that), not frame 0.
constexpr uint64_t kSettleFrames = 60;

std::string DescLine(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc)
{
    return std::to_string(reinterpret_cast<uintptr_t>(resource)) + " " + std::to_string(desc.Width) + "x" +
           std::to_string(desc.Height) + " fmt=" + std::to_string(static_cast<int>(desc.Format)) +
           " mips=" + std::to_string(desc.MipLevels) + " arraySize=" + std::to_string(desc.DepthOrArraySize) +
           " samples=" + std::to_string(desc.SampleDesc.Count) +
           " flags=" + std::to_string(static_cast<unsigned>(desc.Flags)) +
           " layout=" + std::to_string(static_cast<int>(desc.Layout));
}

int FormatScore(DXGI_FORMAT format)
{
    for (const FormatEntry& entry : kDepthFormats)
    {
        if (entry.format == format)
        {
            return entry.score;
        }
    }
    return 0;
}

// The structural gate, minus the extent test - the extent is only known once
// velocity has been identified, and resources are shortlisted before that.
bool StructurallyPlausible(const D3D12_RESOURCE_DESC& desc, const char** rejection)
{
    *rejection = "";
    if (FormatScore(desc.Format) == 0)
    {
        *rejection = "not a depth format UE5's PF_DepthStencil maps to";
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
        // Excludes shadow atlases and the mobile/multiview depth arrays.
        *rejection = "arraySize != 1";
        return false;
    }

    if (desc.SampleDesc.Count != 1)
    {
        *rejection = "sampleCount != 1 (MSAA depth cannot be copied as-is)";
        return false;
    }

    if (desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN)
    {
        *rejection = "layout != UNKNOWN";
        return false;
    }

    if ((static_cast<UINT>(desc.Flags) & kRequiredFlags) != kRequiredFlags)
    {
        *rejection = "flags lack ALLOW_DEPTH_STENCIL";
        return false;
    }

    if ((static_cast<UINT>(desc.Flags) & kForbiddenFlags) != 0)
    {
        *rejection = "DENY_SHADER_RESOURCE is set (a shadow map, not scene depth)";
        return false;
    }
    return true;
}

void Decide(uint64_t frame, uint64_t velocityWidth, uint32_t velocityHeight)
{
    std::vector<ComPtr<ID3D12Resource>> shortlist;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        shortlist = g_shortlist;
    }

    struct Survivor
    {
        ID3D12Resource* resource;
        D3D12_RESOURCE_DESC desc;
        DepthProfile profile;
        int score;
    };
    std::vector<Survivor> survivors;
    for (const ComPtr<ID3D12Resource>& held : shortlist)
    {
        ID3D12Resource* resource = held.Get();
        DepthProfile profile;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto it = g_profiles.find(resource);
            if (it == g_profiles.end())
            {
                continue;
            }
            profile = it->second;
        }
        const D3D12_RESOURCE_DESC desc = resource->GetDesc(); // safe: shortlist holds a reference
        const char* rejection = "";
        if (!StructurallyPlausible(desc, &rejection))
        {
            continue;
        }
        if (desc.Width != velocityWidth || desc.Height != velocityHeight)
        {
            Log("depth: rejected " + DescLine(resource, desc) + " - not at the velocity texture's extent (" +
                std::to_string(velocityWidth) + "x" + std::to_string(velocityHeight) + ")");
            continue;
        }

        // Behavioural gate, deliberately weak (like velocity's): "written as a
        // depth target and read, most frames." Every stronger property measured
        // so far turned out to describe one game's frame graph, not the general
        // case.
        if (profile.framesSeen < kMinFramesObserved || profile.depthWriteToReadable * 2 < profile.framesSeen)
        {
            Log("depth: rejected " + DescLine(resource, desc) + " - only " +
                std::to_string(profile.depthWriteToReadable) + " DEPTH_WRITE->readable edges over " +
                std::to_string(profile.framesSeen) + " frames");
            continue;
        }
        survivors.push_back(Survivor{resource, desc, profile, FormatScore(desc.Format)});
    }

    if (survivors.empty())
    {
        Log("depth: NO candidate after " + std::to_string(frame) + " frames. Structural shortlist had " +
            std::to_string(shortlist.size()) +
            " entries. Capture continues WITHOUT depth - velocity is unaffected; the analytical "
            "reprojection ground truth is what will be missing. Retrying with a longer window.");
        g_decideAtFrame.store(frame + kSettleFrames * 2, std::memory_order_relaxed);
        return;
    }

    // Scene depth is written and read repeatedly per frame (4 DEPTH_WRITE ->
    // readable edges measured); CustomDepth renders once, only when requested.
    // Scored evidence, not a gate - both candidates' numbers go in the log.
    std::sort(
        survivors.begin(),
        survivors.end(),
        [](const Survivor& a, const Survivor& b)
        {
            if (a.score != b.score)
            {
                return a.score > b.score;
            }
            if (a.profile.depthWriteToReadable != b.profile.depthWriteToReadable)
            {
                return a.profile.depthWriteToReadable > b.profile.depthWriteToReadable;
            }
            return a.profile.transitions > b.profile.transitions;
        });
    Log("depth: " + std::to_string(survivors.size()) + " resource(s) passed the structural gate:");

    for (const Survivor& s : survivors)
    {
        const double perFrame =
            s.profile.framesSeen > 0 ? static_cast<double>(s.profile.depthWriteToReadable) / s.profile.framesSeen : 0.0;
        Log("depth:   score=" + std::to_string(s.score) + " " + DescLine(s.resource, s.desc) +
            " framesSeen=" + std::to_string(s.profile.framesSeen) +
            " depthWrite->readable=" + std::to_string(s.profile.depthWriteToReadable) + " (" +
            std::to_string(perFrame) + " per frame) transitions=" + std::to_string(s.profile.transitions));
    }
    if (survivors.size() > 1)
    {
        const Survivor& a = survivors[0];
        const Survivor& b = survivors[1];
        // A clear margin: 1.5x as many produce-then-consume edges per frame is
        // the difference between a depth buffer the frame is built around and
        // one that's occasionally rendered.
        const bool separated =
            a.score > b.score || a.profile.depthWriteToReadable > b.profile.depthWriteToReadable * 3 / 2;
        if (!separated)
        {
            Log("depth: AMBIGUOUS - the top two candidates are at the velocity extent with equally "
                "engine-plausible depth formats AND comparable per-frame usage, so neither structure nor "
                "behaviour separates them. Refusing to pick. Capture continues without depth rather than "
                "copying back a buffer that might be CustomDepth or a downsampled target, which would decode "
                "to a plausible-looking depth field and produce a wrong reprojection with nothing in the data "
                "to say so.");
            g_decisionMade.store(true, std::memory_order_relaxed);
            return;
        }
        Log("depth: picking the top candidate on usage - " + std::to_string(a.profile.depthWriteToReadable) +
            " DEPTH_WRITE->readable edges against " + std::to_string(b.profile.depthWriteToReadable) +
            " for the runner-up over the same window. This is a ranked choice among structurally "
            "indistinguishable candidates, not a unique match.");
    }

    const Survivor& winner = survivors.front();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_identifiedDesc = winner.desc;
    }

    g_identified.store(winner.resource, std::memory_order_release);
    g_decisionMade.store(true, std::memory_order_relaxed);
    // "Unique" means no other resource in the process could have been scene
    // depth; "ranked" means several could and one was chosen on evidence. Not
    // the same claim - the log should distinguish them.
    if (survivors.size() == 1)
    {
        Log("depth: SELECTED " + DescLine(winner.resource, winner.desc) +
            " - the ONLY ALLOW_DEPTH_STENCIL texture at the velocity texture's extent that is written as a "
            "depth target and then read each frame.");
    }
    else
    {
        Log("depth: SELECTED " + DescLine(winner.resource, winner.desc) + " - the top of " +
            std::to_string(survivors.size()) +
            " structurally indistinguishable candidates, chosen on per-frame usage. NOT a unique match: see the "
            "edge counts above, and treat the offline reprojection's per-frame agreement as the check on "
            "whether this was the right one.");
    }
}

} // namespace

void NoteResourceDescForDepth(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc)
{
    const char* rejection = "";
    if (!StructurallyPlausible(desc, &rejection))
    {
        if (FormatScore(desc.Format) > 0 && (static_cast<UINT>(desc.Flags) & kRequiredFlags) == kRequiredFlags)
        {
            // A depth-shaped resource that failed something else - worth a line
            // each, since if the search comes up empty these are what to check
            // first.
            Log(std::string("depth: near-miss ") + DescLine(resource, desc) + " - rejected: " + rejection);
        }
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shortlist.size() >= kMaxShortlisted)
    {
        return;
    }
    if (g_profiles.find(resource) != g_profiles.end())
    {
        return;
    }
    g_profiles.emplace(resource, DepthProfile{});
    g_shortlist.emplace_back(resource); // ComPtr ctor AddRefs
    Log("depth: shortlisted (structure) " + DescLine(resource, desc) + " - now watching its barriers");
}

void NoteDepthTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_profiles.find(resource);
    if (it == g_profiles.end())
    {
        return;
    }
    DepthProfile& p = it->second;
    if (p.lastFrame != frame)
    {
        if (p.lastFrame != ~0ull)
        {
            ++p.framesSeen;
        }
        p.lastFrame = frame;
    }
    ++p.transitions;
    if (stateBefore == D3D12_RESOURCE_STATE_DEPTH_WRITE && IsDepthReadableState(stateAfter))
    {
        ++p.depthWriteToReadable;
    }
}

void OnFrameForDepthIdentification(uint64_t frame)
{
    if (g_decisionMade.load(std::memory_order_relaxed))
    {
        return;
    }
    uint64_t width = 0;
    uint32_t height = 0;
    if (!IdentifiedVelocityExtent(&width, &height))
    {
        // No extent to test against until velocity is identified. Behaviour
        // profiles accumulate since the first barrier regardless, so only the
        // final extent test needs velocity - no separate settling window after.
        return;
    }
    if (frame >= g_decideAtFrame.load(std::memory_order_relaxed))
    {
        // Decide on the very frame the extent becomes available, not
        // kSettleFrames after - profiles accumulate from the first barrier,
        // not from when velocity resolves, so waiting again here just delays
        // the decision past most of a capture burst.
        Decide(frame, width, height);
    }
}

void ReopenDepthIdentification(const char* reason)
{
    Log(std::string("depth: REOPENING the search - ") + reason);
    g_identified.store(nullptr, std::memory_order_release);
    g_decisionMade.store(false, std::memory_order_relaxed);
    g_decideAtFrame.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_shortlist.clear();
    g_profiles.clear();
}

ID3D12Resource* IdentifiedDepthResource()
{
    return g_identified.load(std::memory_order_acquire);
}

bool IsDepthCandidate(ID3D12Resource* resource)
{
    return resource != nullptr && resource == g_identified.load(std::memory_order_acquire);
}

bool ValidateDepthCandidate(ID3D12Resource* resource)
{
    if (!IsDepthCandidate(resource))
    {
        return false;
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    std::lock_guard<std::mutex> lock(g_mutex);
    const D3D12_RESOURCE_DESC& accepted = g_identifiedDesc;
    return desc.Width == accepted.Width && desc.Height == accepted.Height && desc.Format == accepted.Format &&
           desc.MipLevels == accepted.MipLevels && desc.DepthOrArraySize == accepted.DepthOrArraySize &&
           desc.SampleDesc.Count == accepted.SampleDesc.Count && desc.Layout == accepted.Layout;
}

} // namespace mv
