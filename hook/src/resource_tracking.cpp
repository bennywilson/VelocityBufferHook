#include "resource_tracking.h"

#include "logging.h"

#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace mv {
namespace {

// shared_mutex (multiple concurrent readers, exclusive writer) since
// OMSetRenderTargets - the hot-path reader - fires far more often than
// resources/views get created.
std::shared_mutex g_resourceMutex;
std::unordered_map<ID3D12Resource*, ResourceInfo> g_resourceMap;

std::shared_mutex g_rtvMutex;
std::unordered_map<SIZE_T, ID3D12Resource*> g_rtvMap;

std::mutex g_seenMutex;
std::unordered_set<ID3D12Resource*> g_seenResources;

// Bounds worst-case exposure: GetDesc() on a resource encountered in a barrier
// is a considered bet (steady-state resource, not one being created) but not a
// proven-safe pattern, so cap how many distinct resources we are willing to try
// it on. 8000 is generous for identification, which completes in the first ~90
// frames; it is NOT generous for a session, because UE5's transient allocator
// hands out fresh ID3D12Resource objects for the whole run.
constexpr int64_t kIdentifyBudget = 8000;
std::atomic<int64_t> g_identifyBudget{kIdentifyBudget};
std::atomic<bool> g_budgetExhaustionReported{false};

// Candidates are held BY REFERENCE, not by raw pointer.
//
// The target is a transient placed resource out of UE5's
// TransientResourceAllocator - precisely the category most likely to be
// released and have its address handed straight back out for something else.
// Keyed on a bare ID3D12Resource*, a recycled address makes IsCandidate()
// answer true for a completely unrelated resource, and the consequences are
// not a wrong picture: we would issue a transition barrier asserting a
// StateBefore that resource was never in (undefined behaviour, and a debug
// layer error), then CopyTextureRegion out of it using footprints computed
// from the old resource's desc. Both fail silently on a driver that tolerates
// them. Worse, TryMarkResourceSeen guarantees a recycled address is never
// re-examined, so the mistake would be permanent for the session.
//
// An AddRef costs one leaked ID3D12Resource object per candidate for the
// process lifetime. That is cheap here (one candidate per session, measured
// across six sessions) and it does NOT pin the underlying heap memory - a
// placed resource's storage belongs to the heap, which the engine keeps
// aliasing as it likes. What it does buy is that the address cannot be
// reissued while we are still using it as an identity, which is the whole
// requirement.
struct Candidate {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_DESC desc{};
};

std::shared_mutex g_candidateMutex;
std::unordered_map<ID3D12Resource*, Candidate> g_candidateResources;

bool SameLayout(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count && a.Flags == b.Flags;
}

} // namespace

void RecordResource(ID3D12Resource* resource, const ResourceInfo& info) {
    std::unique_lock lock(g_resourceMutex);
    g_resourceMap[resource] = info;
}

std::optional<ResourceInfo> GetResourceInfo(ID3D12Resource* resource) {
    std::shared_lock lock(g_resourceMutex);
    const auto it = g_resourceMap.find(resource);
    if (it == g_resourceMap.end()) {
        return std::nullopt;
    }
    return it->second;
}

void RecordRenderTargetView(SIZE_T descriptorHandle, ID3D12Resource* resource) {
    std::unique_lock lock(g_rtvMutex);
    g_rtvMap[descriptorHandle] = resource;
}

ID3D12Resource* GetResourceForRenderTargetView(SIZE_T descriptorHandle) {
    std::shared_lock lock(g_rtvMutex);
    const auto it = g_rtvMap.find(descriptorHandle);
    return it != g_rtvMap.end() ? it->second : nullptr;
}

bool TryMarkResourceSeen(ID3D12Resource* resource) {
    std::lock_guard<std::mutex> lock(g_seenMutex);
    return g_seenResources.insert(resource).second;
}

bool TryTakeIdentifyBudget() {
    if (g_identifyBudget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        return true;
    }
    if (!g_budgetExhaustionReported.exchange(true)) {
        Log("resource-tracking: identification budget of " + std::to_string(kIdentifyBudget) +
            " distinct resources is SPENT. No resource created from here on is examined, so a "
            "velocity or depth texture the engine allocates now can never be found - and until this "
            "line existed that happened in total silence, looking exactly like a game that had "
            "stopped creating resources. Identification itself is long finished; what is lost is the "
            "ability to notice a REPLACEMENT.");
    }
    return false;
}

void ForgetAllResourcesSeen() {
    // Refill first: reopening with an empty budget clears the seen set and then
    // rejects every resource one layer down, which searches forever and finds
    // nothing while reporting that it reopened.
    g_identifyBudget.store(kIdentifyBudget, std::memory_order_relaxed);
    g_budgetExhaustionReported.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_seenMutex);
    g_seenResources.clear();
}

void MarkAsCandidate(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) {
    std::unique_lock lock(g_candidateMutex);
    auto& entry = g_candidateResources[resource];
    entry.resource = resource; // ComPtr::operator= AddRefs
    entry.desc = desc;
}

bool IsCandidate(ID3D12Resource* resource) {
    std::shared_lock lock(g_candidateMutex);
    return g_candidateResources.count(resource) != 0;
}

bool ValidateCandidate(ID3D12Resource* resource) {
    // Safe to call GetDesc() here specifically because we hold a reference -
    // this is the one place in the project where touching a resource outside
    // the call that handed it to us is defensible, and only for that reason.
    D3D12_RESOURCE_DESC recorded{};
    {
        std::shared_lock lock(g_candidateMutex);
        const auto it = g_candidateResources.find(resource);
        if (it == g_candidateResources.end()) {
            return false;
        }
        recorded = it->second.desc;
    }
    const D3D12_RESOURCE_DESC current = resource->GetDesc();
    if (SameLayout(recorded, current)) {
        return true;
    }
    // Either the address was recycled despite the reference (it cannot be, so
    // this would mean the reference was lost) or the resource was genuinely
    // recreated at a new resolution. Both mean the footprints and the assumed
    // StateBefore are stale, so stop acting on it rather than issue a barrier
    // that asserts a state it is not in.
    Log("resource-tracking: candidate " + std::to_string(reinterpret_cast<uintptr_t>(resource)) +
        " no longer matches the desc it was accepted with (" + std::to_string(recorded.Width) + "x" +
        std::to_string(recorded.Height) + " fmt=" + std::to_string(static_cast<int>(recorded.Format)) + " -> " +
        std::to_string(current.Width) + "x" + std::to_string(current.Height) + " fmt=" +
        std::to_string(static_cast<int>(current.Format)) + "); dropping it");
    {
        std::unique_lock lock(g_candidateMutex);
        g_candidateResources.erase(resource);
    }
    return false;
}

} // namespace mv
