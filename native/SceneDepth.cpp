// See SceneDepth.hpp.

#include "SceneDepth.hpp"

#include "SceneDepthPolicy.hpp"
#include "ScreenDepth.hpp"
#include "ScreenDepthPass.hpp"
#include "ScreenTessellation.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace op77::WebUI::SceneDepth
{
namespace
{
namespace Policy = op77::WebUI::SceneDepthPolicy;
namespace SD = op77::WorldOverlay::ScreenDepth;
namespace Tess = op77::WorldOverlay::ScreenTessellation;
namespace Clip = op77::WorldOverlay::ScreenClip;

static_assert(Policy::Dxgi::kR32G8X24Typeless == static_cast<uint32_t>(DXGI_FORMAT_R32G8X24_TYPELESS));
static_assert(Policy::Dxgi::kD32FloatS8X24Uint == static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT_S8X24_UINT));
static_assert(Policy::Dxgi::kR32FloatX8X24Typeless == static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS));
static_assert(Policy::Dxgi::kX32TypelessG8X24Uint == static_cast<uint32_t>(DXGI_FORMAT_X32_TYPELESS_G8X24_UINT));
static_assert(Policy::Dxgi::kR32Typeless == static_cast<uint32_t>(DXGI_FORMAT_R32_TYPELESS));
static_assert(Policy::Dxgi::kD32Float == static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT));
static_assert(Policy::Dxgi::kR32Float == static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT));
static_assert(Policy::Dxgi::kR24G8Typeless == static_cast<uint32_t>(DXGI_FORMAT_R24G8_TYPELESS));
static_assert(Policy::Dxgi::kD24UnormS8Uint == static_cast<uint32_t>(DXGI_FORMAT_D24_UNORM_S8_UINT));
static_assert(Policy::Dxgi::kR24UnormX8Typeless == static_cast<uint32_t>(DXGI_FORMAT_R24_UNORM_X8_TYPELESS));
static_assert(Policy::Dxgi::kX24TypelessG8Uint == static_cast<uint32_t>(DXGI_FORMAT_X24_TYPELESS_G8_UINT));
static_assert(Policy::Dxgi::kR16Typeless == static_cast<uint32_t>(DXGI_FORMAT_R16_TYPELESS));
static_assert(Policy::Dxgi::kD16Unorm == static_cast<uint32_t>(DXGI_FORMAT_D16_UNORM));
static_assert(Policy::Dxgi::kR16Unorm == static_cast<uint32_t>(DXGI_FORMAT_R16_UNORM));

// ---------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------

LogFunction s_log{};

void Log(const LogLevel aLevel, const std::string& aMessage)
{
    if (s_log != nullptr)
    {
        s_log(aLevel, "TV depth: " + aMessage);
    }
}

/// Logs a warning the first time `aFlag` is raised.
void WarnOnce(std::atomic_bool& aFlag, const std::string& aMessage)
{
    if (!aFlag.exchange(true, std::memory_order_relaxed))
    {
        Log(LogLevel::Warning, aMessage);
    }
}

std::string Hex(const void* const aPointer)
{
    return std::format("0x{:X}", reinterpret_cast<uintptr_t>(aPointer));
}

std::string StateName(const D3D12_RESOURCE_STATES aState)
{
    if (aState == D3D12_RESOURCE_STATE_COMMON)
    {
        return "COMMON";
    }
    static const std::pair<D3D12_RESOURCE_STATES, const char*> kNames[] = {
        {D3D12_RESOURCE_STATE_DEPTH_WRITE, "DEPTH_WRITE"},
        {D3D12_RESOURCE_STATE_DEPTH_READ, "DEPTH_READ"},
        {D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "PIXEL_SHADER_RESOURCE"},
        {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "NON_PIXEL_SHADER_RESOURCE"},
        {D3D12_RESOURCE_STATE_COPY_SOURCE, "COPY_SOURCE"},
        {D3D12_RESOURCE_STATE_COPY_DEST, "COPY_DEST"},
        {D3D12_RESOURCE_STATE_RESOLVE_SOURCE, "RESOLVE_SOURCE"},
        {D3D12_RESOURCE_STATE_RESOLVE_DEST, "RESOLVE_DEST"},
        {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "UNORDERED_ACCESS"},
        {D3D12_RESOURCE_STATE_RENDER_TARGET, "RENDER_TARGET"},
    };
    std::string text;
    auto rest = static_cast<uint32_t>(aState);
    for (const auto& [bit, name] : kNames)
    {
        if ((rest & static_cast<uint32_t>(bit)) != 0)
        {
            if (!text.empty())
            {
                text += '|';
            }
            text += name;
            rest &= ~static_cast<uint32_t>(bit);
        }
    }
    if (rest != 0)
    {
        text += std::format("{}0x{:X}", text.empty() ? "" : "|", rest);
    }
    return text;
}

// ---------------------------------------------------------------------------------
// What the hooks see
// ---------------------------------------------------------------------------------

constexpr uint32_t kDepthStates =
    static_cast<uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE) | static_cast<uint32_t>(D3D12_RESOURCE_STATE_DEPTH_READ);
constexpr uint32_t kShaderReadStates = static_cast<uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                                       static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
constexpr uint32_t kWriteStates =
    static_cast<uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE) | static_cast<uint32_t>(D3D12_RESOURCE_STATE_RENDER_TARGET) |
    static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) | static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_DEST) |
    static_cast<uint32_t>(D3D12_RESOURCE_STATE_RESOLVE_DEST);

/// A state nothing writes the buffer in: its depth is finished for now. (COMMON is
/// not counted: it says nothing about what comes next.)
constexpr bool IsReadOnly(const D3D12_RESOURCE_STATES aState)
{
    const auto bits = static_cast<uint32_t>(aState);
    return bits != 0 && (bits & kWriteStates) == 0;
}

/// What a depth-stencil view points at, from CreateDepthStencilView.
struct DsvTarget
{
    ID3D12Resource* resource{}; ///< identity only: never called through after the view was made
    D3D12_RESOURCE_DESC desc{};
    bool usable{}; ///< a 2-D view of the first mip and slice
};

std::shared_mutex s_dsvMutex;
std::unordered_map<SIZE_T, DsvTarget> s_dsvs;
std::atomic_uint64_t s_dsvsCreated{};

/// Depth buffers whose every state change is worth recording: checked on every
/// barrier the game records, so a small lock-free open-addressed set. A pointer
/// stays after its resource is gone; that only costs a few recorded transitions
/// of whatever reuses the address.
constexpr size_t kKnownCapacity = 256;
constexpr size_t kKnownProbes = 4;
std::array<std::atomic<ID3D12Resource*>, kKnownCapacity> s_known{};

size_t KnownSlot(const ID3D12Resource* const aResource)
{
    auto value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(aResource));
    value ^= value >> 33U;
    value *= 0xFF51AFD7ED558CCDULL;
    value ^= value >> 33U;
    return static_cast<size_t>(value) & (kKnownCapacity - 1);
}

bool IsKnown(const ID3D12Resource* const aResource)
{
    if (aResource == nullptr)
    {
        return false;
    }
    const size_t first = KnownSlot(aResource);
    for (size_t probe = 0; probe < kKnownProbes; ++probe)
    {
        if (s_known[(first + probe) & (kKnownCapacity - 1)].load(std::memory_order_relaxed) == aResource)
        {
            return true;
        }
    }
    return false;
}

void Remember(ID3D12Resource* const aResource)
{
    if (aResource == nullptr || IsKnown(aResource))
    {
        return;
    }
    const size_t first = KnownSlot(aResource);
    for (size_t probe = 0; probe < kKnownProbes; ++probe)
    {
        ID3D12Resource* expected = nullptr;
        if (s_known[(first + probe) & (kKnownCapacity - 1)].compare_exchange_strong(expected, aResource,
                                                                                      std::memory_order_relaxed))
        {
            return;
        }
    }
    // All probed slots taken: the newest wins the first of them.
    s_known[first].store(aResource, std::memory_order_relaxed);
}

/// One depth buffer bound by one command list while it was recorded.
struct Use
{
    SIZE_T dsv{};
    uint32_t draws{};
    uint32_t indirect{};
    uint64_t vertices{};
    float viewportWidth{};
    float viewportHeight{};
    bool cleared{};
    float clearDepth{};
};

struct StateChange
{
    enum class Kind : uint8_t
    {
        Transition,
        AliasedAway, ///< an aliasing barrier gave its memory to another resource
        AliasedIn,   ///< an aliasing barrier gave it its memory back
        Cleared,     ///< the whole depth plane of the view `dsv` was cleared (a new frame's depth begins)
    };
    ID3D12Resource* resource{};
    UINT subresource{};
    D3D12_RESOURCE_STATES after{};
    Kind kind{Kind::Transition};
    SIZE_T dsv{};
};

/// Everything one command list recorded that matters, since its last Reset.
/// Written only by the thread recording the list; read when it is executed.
struct ListState
{
    static constexpr size_t kUses = 16;
    std::array<Use, kUses> uses{};
    size_t useCount{};
    Use* current{};
    float viewportWidth{};
    float viewportHeight{};
    std::vector<StateChange> transitions;

    void Clear()
    {
        useCount = 0;
        current = nullptr;
        viewportWidth = 0.0F;
        viewportHeight = 0.0F;
        transitions.clear();
    }

    /// The use for `aDsv`, added if new; null when there is no room.
    Use* UseOf(const SIZE_T aDsv)
    {
        for (size_t index = 0; index < useCount; ++index)
        {
            if (uses[index].dsv == aDsv)
            {
                return &uses[index];
            }
        }
        if (useCount == kUses)
        {
            return nullptr;
        }
        Use& use = uses[useCount++];
        use = Use{};
        use.dsv = aDsv;
        return &use;
    }

    void Bind(const SIZE_T aDsv) { current = aDsv == 0 ? nullptr : UseOf(aDsv); }
};

std::shared_mutex s_listsMutex;
std::unordered_map<const void*, std::unique_ptr<ListState>> s_lists;
constexpr size_t kMaximumLists = 8192;

struct ListCache
{
    const void* list{};
    ListState* state{};
};
thread_local ListCache t_listCache;

ListState* StateOf(const void* const aList)
{
    if (t_listCache.list == aList)
    {
        return t_listCache.state;
    }
    ListState* state = nullptr;
    {
        const std::shared_lock lock(s_listsMutex);
        if (const auto found = s_lists.find(aList); found != s_lists.end())
        {
            state = found->second.get();
        }
    }
    if (state == nullptr)
    {
        const std::unique_lock lock(s_listsMutex);
        auto& slot = s_lists[aList];
        if (!slot)
        {
            if (s_lists.size() > kMaximumLists)
            {
                s_lists.erase(aList);
                return nullptr;
            }
            slot = std::make_unique<ListState>();
            slot->transitions.reserve(32);
        }
        state = slot.get();
    }
    t_listCache = {aList, state};
    return state;
}

ListState* PeekState(const void* const aList)
{
    const std::shared_lock lock(s_listsMutex);
    const auto found = s_lists.find(aList);
    return found == s_lists.end() ? nullptr : found->second.get();
}

/// A depth buffer across frames: its last known state, per plane, in the order the
/// GPU runs the game's lists.
struct Track
{
    std::array<D3D12_RESOURCE_STATES, 2> state{};
    std::array<bool, 2> known{};
    bool initialKnown{}; ///< from its creation, not from a change seen since
    bool cleared{};
    float clearDepth{};  ///< the optimised clear value it was made with, or the last clear seen
    uint64_t writes{};   ///< draws into it and clears of it, ever: changes whenever its content may have
    bool settledSinceClear{}; ///< moved into a read-only state since its last clear: this frame's depth is done
    bool everSettled{};  ///< ever moved into a read-only state: the game reads it once it is done
};

/// A depth buffer in the frame being accumulated.
struct FrameUse
{
    D3D12_RESOURCE_DESC desc{};
    bool haveDesc{};
    uint32_t draws{};
    uint32_t indirect{};
    uint64_t vertices{};
    float viewportWidth{};
    float viewportHeight{};
    bool cleared{};
    float clearDepth{};
    bool readByShaders{};
    bool aliasedAway{};
};

struct FrameRecord
{
    std::unordered_map<ID3D12Resource*, FrameUse> uses;
    uint64_t unresolvedDraws{}; ///< draws into a view made before the hooks could see it
    uint64_t lists{};
};

std::mutex s_frameMutex;
FrameRecord s_frame;
std::unordered_map<ID3D12Resource*, Track> s_tracks;

void Merge(ListState& aList)
{
    // Caller holds s_frameMutex.
    ++s_frame.lists;
    for (size_t index = 0; index < aList.useCount; ++index)
    {
        const Use& use = aList.uses[index];
        if (use.draws == 0 && !use.cleared)
        {
            continue;
        }
        DsvTarget target;
        {
            const std::shared_lock lock(s_dsvMutex);
            const auto found = s_dsvs.find(use.dsv);
            if (found == s_dsvs.end())
            {
                s_frame.unresolvedDraws += use.draws;
                continue;
            }
            target = found->second;
        }
        if (!target.usable || target.resource == nullptr)
        {
            continue;
        }
        FrameUse& frame = s_frame.uses[target.resource];
        frame.desc = target.desc;
        frame.haveDesc = true;
        frame.draws += use.draws;
        frame.indirect += use.indirect;
        frame.vertices += use.vertices;
        frame.viewportWidth = std::max(frame.viewportWidth, use.viewportWidth);
        frame.viewportHeight = std::max(frame.viewportHeight, use.viewportHeight);
        Track& track = s_tracks[target.resource];
        track.writes += use.draws + (use.cleared ? 1U : 0U);
        if (use.cleared)
        {
            frame.cleared = true;
            frame.clearDepth = use.clearDepth;
            track.cleared = true;
            track.clearDepth = use.clearDepth;
        }
    }
    // State changes and clears in the order the list recorded them, so that "done
    // with since its last clear" means the same as it does on the GPU.
    for (const StateChange& change : aList.transitions)
    {
        if (change.kind == StateChange::Kind::Cleared)
        {
            ID3D12Resource* cleared = nullptr;
            {
                const std::shared_lock lock(s_dsvMutex);
                if (const auto found = s_dsvs.find(change.dsv); found != s_dsvs.end() && found->second.usable)
                {
                    cleared = found->second.resource;
                }
            }
            if (cleared != nullptr)
            {
                s_tracks[cleared].settledSinceClear = false;
            }
            continue;
        }
        Track& track = s_tracks[change.resource];
        FrameUse& frame = s_frame.uses[change.resource]; // the game used it this present, whatever the change
        if (change.kind != StateChange::Kind::Transition)
        {
            // In the order the GPU runs them: what counts is whether the memory is
            // the buffer's when the frame ends.
            frame.aliasedAway = change.kind == StateChange::Kind::AliasedAway;
            continue;
        }
        if (change.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
        {
            track.state = {change.after, change.after};
            track.known = {true, true};
        }
        else if (change.subresource < 2)
        {
            track.state[change.subresource] = change.after;
            track.known[change.subresource] = true;
        }
        if ((static_cast<uint32_t>(change.after) & kShaderReadStates) != 0)
        {
            frame.readByShaders = true;
        }
        // The depth plane moved to where nothing writes it: this frame's depth is done.
        if ((change.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || change.subresource == 0) &&
            IsReadOnly(change.after))
        {
            track.settledSinceClear = true;
            track.everSettled = true;
        }
    }
    aList.useCount = 0;
    aList.current = nullptr;
    aList.transitions.clear();
}

// ---------------------------------------------------------------------------------
// The hooks: entries of the runtime's own function tables
// ---------------------------------------------------------------------------------

// Slots in the tables, in d3d12.h's declaration order (checked against the C
// interface's Vtbl structs: tests/SceneDepth harness).
namespace Slot
{
constexpr size_t kDeviceCreateDepthStencilView = 21;
constexpr size_t kDeviceCopyDescriptors = 23;
constexpr size_t kDeviceCopyDescriptorsSimple = 24;
constexpr size_t kDeviceCreateCommittedResource = 27;
constexpr size_t kDeviceCreatePlacedResource = 29;
constexpr size_t kQueueExecuteCommandLists = 10;
constexpr size_t kListReset = 10;
constexpr size_t kListDrawInstanced = 12;
constexpr size_t kListDrawIndexedInstanced = 13;
constexpr size_t kListRSSetViewports = 21;
constexpr size_t kListResourceBarrier = 26;
constexpr size_t kListOMSetRenderTargets = 46;
constexpr size_t kListClearDepthStencilView = 47;
constexpr size_t kListExecuteIndirect = 59;
constexpr size_t kListBeginRenderPass = 68; // ID3D12GraphicsCommandList4
constexpr size_t kListEndRenderPass = 69;
constexpr size_t kCount = 70;
} // namespace Slot

enum class TableKind : uint8_t
{
    Device,
    Queue,
    List,
};

struct HookedTable
{
    void** table{};
    TableKind kind{};
    std::array<void*, Slot::kCount> originals{};
    std::array<void*, Slot::kCount> detours{};
};

constexpr size_t kMaximumTables = 8;
std::array<HookedTable, kMaximumTables> s_tables{};
std::atomic_size_t s_tableCount{};
std::mutex s_patchMutex;

/// The original entry for `aSlot` of the table `aSelf` was called through.
template <typename Function>
Function OriginalOf(const void* const aSelf, const size_t aSlot, const TableKind aKind)
{
    void** const table = *static_cast<void** const*>(aSelf);
    const size_t count = s_tableCount.load(std::memory_order_acquire);
    for (size_t index = 0; index < count; ++index)
    {
        if (s_tables[index].table == table && s_tables[index].originals[aSlot] != nullptr)
        {
            return reinterpret_cast<Function>(s_tables[index].originals[aSlot]);
        }
    }
    // Reached through a table this did not patch (another hook copied the entry):
    // every table of a kind holds the same runtime functions.
    for (size_t index = 0; index < count; ++index)
    {
        if (s_tables[index].kind == aKind && s_tables[index].originals[aSlot] != nullptr)
        {
            return reinterpret_cast<Function>(s_tables[index].originals[aSlot]);
        }
    }
    return nullptr;
}

std::wstring ModuleNameOf(const void* const aAddress)
{
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(aAddress), &module) == 0)
    {
        return L"";
    }
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    std::wstring name(path.data(), length);
    if (const auto slash = name.find_last_of(L"\\/"); slash != std::wstring::npos)
    {
        name.erase(0, slash + 1);
    }
    for (auto& character : name)
    {
        character = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(character)));
    }
    return name;
}

std::string Narrow(const std::wstring& aText)
{
    std::string out;
    out.reserve(aText.size());
    for (const wchar_t character : aText)
    {
        out += (character >= 0x20 && character < 0x7F) ? static_cast<char>(character) : '?';
    }
    return out;
}

/// The Direct3D 12 runtime's own objects, not a wrapper around them: every call a
/// wrapper makes ends up in the runtime's, so hooking both would count twice.
/// (Microsoft's runtime and vkd3d-proton live in d3d12core.dll or d3d12.dll; Wine's
/// own vkd3d in wined3d.dll.)
bool IsRuntimeTable(void** const aTable)
{
    const std::wstring name = ModuleNameOf(aTable);
    return name == L"d3d12core.dll" || name == L"d3d12.dll" || name == L"wined3d.dll";
}

bool IsHooked(void** const aTable)
{
    const size_t count = s_tableCount.load(std::memory_order_acquire);
    for (size_t index = 0; index < count; ++index)
    {
        if (s_tables[index].table == aTable)
        {
            return true;
        }
    }
    return false;
}

void WriteEntry(void** const aEntry, void* const aValue)
{
    DWORD previous = 0;
    if (VirtualProtect(aEntry, sizeof(void*), PAGE_READWRITE, &previous) == 0)
    {
        return;
    }
    InterlockedExchangePointer(aEntry, aValue);
    DWORD ignored = 0;
    VirtualProtect(aEntry, sizeof(void*), previous, &ignored);
}

std::atomic_bool s_warnedTables{};

/// Points `aEntries` of `aTable` at the detours, keeping the originals. A table is
/// patched once; its record is published before any entry changes, so a detour
/// always finds the original it chains to.
bool PatchTable(void** const aTable, const TableKind aKind, std::span<const std::pair<size_t, void*>> aEntries,
                const char* const aWhat)
{
    if (aTable == nullptr)
    {
        return false;
    }
    const std::scoped_lock lock(s_patchMutex);
    const size_t count = s_tableCount.load(std::memory_order_relaxed);
    for (size_t index = 0; index < count; ++index)
    {
        HookedTable& existing = s_tables[index];
        if (existing.table != aTable)
        {
            continue;
        }
        // Patched before and put back by Uninstall: on again.
        for (const auto& [slot, detour] : aEntries)
        {
            if (existing.detours[slot] == detour && aTable[slot] == existing.originals[slot])
            {
                WriteEntry(&aTable[slot], detour);
            }
        }
        return true;
    }
    if (count == kMaximumTables)
    {
        WarnOnce(s_warnedTables, "too many Direct3D 12 function tables; a further one is not watched.");
        return false;
    }
    HookedTable& record = s_tables[count];
    record = HookedTable{};
    record.table = aTable;
    record.kind = aKind;
    for (const auto& [slot, detour] : aEntries)
    {
        record.originals[slot] = aTable[slot];
        record.detours[slot] = detour;
    }
    s_tableCount.store(count + 1, std::memory_order_release);
    for (const auto& [slot, detour] : aEntries)
    {
        WriteEntry(&aTable[slot], detour);
    }
    Log(LogLevel::Info, std::format("watching the {} table at {} ({}).", aWhat, Hex(aTable), Narrow(ModuleNameOf(aTable))));
    return true;
}

// -- device -------------------------------------------------------------------------

using CreateDepthStencilViewFunction = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
                                                                 const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                                                 D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDescriptorsFunction = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                                          const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                                          const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyDescriptorsSimpleFunction = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                                D3D12_CPU_DESCRIPTOR_HANDLE,
                                                                D3D12_DESCRIPTOR_HEAP_TYPE);
using CreateCommittedResourceFunction = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                                     D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                                     D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                                                     REFIID, void**);
using CreatePlacedResourceFunction = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
                                                                  const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                                  const D3D12_CLEAR_VALUE*, REFIID, void**);

void NoteDsv(const SIZE_T aDescriptor, ID3D12Resource* const aResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* const aDesc)
{
    if (aResource == nullptr)
    {
        const std::unique_lock lock(s_dsvMutex);
        s_dsvs.erase(aDescriptor);
        return;
    }
    DsvTarget target;
    target.resource = aResource;
    target.desc = aResource->GetDesc();
    target.usable = target.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    if (aDesc != nullptr)
    {
        target.usable = target.usable && aDesc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D &&
                        aDesc->Texture2D.MipSlice == 0;
    }
    Remember(aResource);
    s_dsvsCreated.fetch_add(1, std::memory_order_relaxed);
    const std::unique_lock lock(s_dsvMutex);
    s_dsvs[aDescriptor] = target;
}

void STDMETHODCALLTYPE DetourCreateDepthStencilView(ID3D12Device* const aSelf, ID3D12Resource* const aResource,
                                                    const D3D12_DEPTH_STENCIL_VIEW_DESC* const aDesc,
                                                    const D3D12_CPU_DESCRIPTOR_HANDLE aDescriptor)
{
    if (const auto original = OriginalOf<CreateDepthStencilViewFunction>(aSelf, Slot::kDeviceCreateDepthStencilView,
                                                                        TableKind::Device))
    {
        original(aSelf, aResource, aDesc, aDescriptor);
    }
    NoteDsv(aDescriptor.ptr, aResource, aDesc);
}

void CopyDsv(const SIZE_T aDestination, const SIZE_T aSource)
{
    const std::unique_lock lock(s_dsvMutex);
    if (const auto found = s_dsvs.find(aSource); found != s_dsvs.end())
    {
        const DsvTarget copy = found->second;
        s_dsvs[aDestination] = copy;
    }
    else
    {
        s_dsvs.erase(aDestination);
    }
}

void STDMETHODCALLTYPE DetourCopyDescriptorsSimple(ID3D12Device* const aSelf, const UINT aCount,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE aDestination,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE aSource,
                                                   const D3D12_DESCRIPTOR_HEAP_TYPE aType)
{
    if (const auto original = OriginalOf<CopyDescriptorsSimpleFunction>(aSelf, Slot::kDeviceCopyDescriptorsSimple,
                                                                       TableKind::Device))
    {
        original(aSelf, aCount, aDestination, aSource, aType);
    }
    if (aType != D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
    {
        return;
    }
    const SIZE_T step = aSelf->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (UINT index = 0; index < aCount; ++index)
    {
        CopyDsv(aDestination.ptr + (index * step), aSource.ptr + (index * step));
    }
}

void STDMETHODCALLTYPE DetourCopyDescriptors(ID3D12Device* const aSelf, const UINT aDestinationRanges,
                                             const D3D12_CPU_DESCRIPTOR_HANDLE* const aDestinationStarts,
                                             const UINT* const aDestinationSizes, const UINT aSourceRanges,
                                             const D3D12_CPU_DESCRIPTOR_HANDLE* const aSourceStarts,
                                             const UINT* const aSourceSizes, const D3D12_DESCRIPTOR_HEAP_TYPE aType)
{
    if (const auto original =
            OriginalOf<CopyDescriptorsFunction>(aSelf, Slot::kDeviceCopyDescriptors, TableKind::Device))
    {
        original(aSelf, aDestinationRanges, aDestinationStarts, aDestinationSizes, aSourceRanges, aSourceStarts,
                 aSourceSizes, aType);
    }
    if (aType != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || aDestinationStarts == nullptr || aSourceStarts == nullptr)
    {
        return;
    }
    // Walk both sets of ranges in step, one descriptor at a time (a null size array
    // means every range is one descriptor long).
    const SIZE_T step = aSelf->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    UINT destinationRange = 0;
    UINT destinationOffset = 0;
    UINT sourceRange = 0;
    UINT sourceOffset = 0;
    const auto sizeOf = [](const UINT* const aSizes, const UINT aRange) { return aSizes == nullptr ? 1U : aSizes[aRange]; };
    while (destinationRange < aDestinationRanges && sourceRange < aSourceRanges)
    {
        if (destinationOffset >= sizeOf(aDestinationSizes, destinationRange))
        {
            ++destinationRange;
            destinationOffset = 0;
            continue;
        }
        if (sourceOffset >= sizeOf(aSourceSizes, sourceRange))
        {
            ++sourceRange;
            sourceOffset = 0;
            continue;
        }
        CopyDsv(aDestinationStarts[destinationRange].ptr + (destinationOffset * step),
                aSourceStarts[sourceRange].ptr + (sourceOffset * step));
        ++destinationOffset;
        ++sourceOffset;
    }
}

/// A depth buffer just made: its first state and the clear value it was made for.
void NoteCreated(const D3D12_RESOURCE_DESC* const aDesc, const D3D12_RESOURCE_STATES aInitial,
                 const D3D12_CLEAR_VALUE* const aClear, REFIID aInterface, void* const aObject)
{
    if (aDesc == nullptr || aObject == nullptr ||
        (aDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0)
    {
        return;
    }
    // The object is the interface asked for; the resource's identity is its
    // ID3D12Resource pointer, which a barrier carries.
    ID3D12Resource* resource = nullptr;
    auto* const unknown = static_cast<IUnknown*>(aObject);
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&resource))) || resource == nullptr)
    {
        return;
    }
    static_cast<void>(aInterface);
    {
        const std::scoped_lock lock(s_frameMutex);
        Track& track = s_tracks[resource];
        track = Track{};
        track.state = {aInitial, aInitial};
        track.known = {true, true};
        track.initialKnown = true;
        if (aClear != nullptr)
        {
            track.cleared = true;
            track.clearDepth = aClear->DepthStencil.Depth;
        }
    }
    Remember(resource);
    resource->Release();
}

HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device* const aSelf,
                                                        const D3D12_HEAP_PROPERTIES* const aHeap,
                                                        const D3D12_HEAP_FLAGS aHeapFlags,
                                                        const D3D12_RESOURCE_DESC* const aDesc,
                                                        const D3D12_RESOURCE_STATES aInitial,
                                                        const D3D12_CLEAR_VALUE* const aClear, REFIID aInterface,
                                                        void** const aObject)
{
    const auto original = OriginalOf<CreateCommittedResourceFunction>(aSelf, Slot::kDeviceCreateCommittedResource,
                                                                      TableKind::Device);
    if (original == nullptr)
    {
        return E_FAIL;
    }
    const HRESULT result = original(aSelf, aHeap, aHeapFlags, aDesc, aInitial, aClear, aInterface, aObject);
    if (SUCCEEDED(result) && aObject != nullptr)
    {
        NoteCreated(aDesc, aInitial, aClear, aInterface, *aObject);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE DetourCreatePlacedResource(ID3D12Device* const aSelf, ID3D12Heap* const aHeap,
                                                     const UINT64 aOffset, const D3D12_RESOURCE_DESC* const aDesc,
                                                     const D3D12_RESOURCE_STATES aInitial,
                                                     const D3D12_CLEAR_VALUE* const aClear, REFIID aInterface,
                                                     void** const aObject)
{
    const auto original = OriginalOf<CreatePlacedResourceFunction>(aSelf, Slot::kDeviceCreatePlacedResource,
                                                                   TableKind::Device);
    if (original == nullptr)
    {
        return E_FAIL;
    }
    const HRESULT result = original(aSelf, aHeap, aOffset, aDesc, aInitial, aClear, aInterface, aObject);
    if (SUCCEEDED(result) && aObject != nullptr)
    {
        NoteCreated(aDesc, aInitial, aClear, aInterface, *aObject);
    }
    return result;
}

// -- command list -------------------------------------------------------------------

using ResetFunction = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*,
                                                   ID3D12PipelineState*);
using DrawInstancedFunction = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexedInstancedFunction = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT,
                                                               UINT);
using RSSetViewportsFunction = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using ResourceBarrierFunction = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                          const D3D12_RESOURCE_BARRIER*);
using OMSetRenderTargetsFunction = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                             const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                             const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ClearDepthStencilViewFunction = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                                D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT,
                                                                const D3D12_RECT*);
using ExecuteIndirectFunction = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT,
                                                          ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);

HRESULT STDMETHODCALLTYPE DetourReset(ID3D12GraphicsCommandList* const aSelf,
                                      ID3D12CommandAllocator* const aAllocator,
                                      ID3D12PipelineState* const aPipeline)
{
    const auto original = OriginalOf<ResetFunction>(aSelf, Slot::kListReset, TableKind::List);
    if (original == nullptr)
    {
        return E_FAIL;
    }
    const HRESULT result = original(aSelf, aAllocator, aPipeline);
    if (SUCCEEDED(result))
    {
        if (ListState* const state = StateOf(aSelf))
        {
            state->Clear();
        }
    }
    return result;
}

void STDMETHODCALLTYPE DetourDrawInstanced(ID3D12GraphicsCommandList* const aSelf, const UINT aVertices,
                                           const UINT aInstances, const UINT aFirstVertex, const UINT aFirstInstance)
{
    if (const auto original = OriginalOf<DrawInstancedFunction>(aSelf, Slot::kListDrawInstanced, TableKind::List))
    {
        original(aSelf, aVertices, aInstances, aFirstVertex, aFirstInstance);
    }
    if (ListState* const state = StateOf(aSelf); state != nullptr && state->current != nullptr)
    {
        Use& use = *state->current;
        ++use.draws;
        use.vertices += static_cast<uint64_t>(aVertices) * aInstances;
        use.viewportWidth = std::max(use.viewportWidth, state->viewportWidth);
        use.viewportHeight = std::max(use.viewportHeight, state->viewportHeight);
    }
}

void STDMETHODCALLTYPE DetourDrawIndexedInstanced(ID3D12GraphicsCommandList* const aSelf, const UINT aIndices,
                                                  const UINT aInstances, const UINT aFirstIndex,
                                                  const INT aBaseVertex, const UINT aFirstInstance)
{
    if (const auto original =
            OriginalOf<DrawIndexedInstancedFunction>(aSelf, Slot::kListDrawIndexedInstanced, TableKind::List))
    {
        original(aSelf, aIndices, aInstances, aFirstIndex, aBaseVertex, aFirstInstance);
    }
    if (ListState* const state = StateOf(aSelf); state != nullptr && state->current != nullptr)
    {
        Use& use = *state->current;
        ++use.draws;
        use.vertices += static_cast<uint64_t>(aIndices) * aInstances;
        use.viewportWidth = std::max(use.viewportWidth, state->viewportWidth);
        use.viewportHeight = std::max(use.viewportHeight, state->viewportHeight);
    }
}

void STDMETHODCALLTYPE DetourExecuteIndirect(ID3D12GraphicsCommandList* const aSelf,
                                             ID3D12CommandSignature* const aSignature, const UINT aMaximum,
                                             ID3D12Resource* const aArguments, const UINT64 aArgumentOffset,
                                             ID3D12Resource* const aCount, const UINT64 aCountOffset)
{
    if (const auto original = OriginalOf<ExecuteIndirectFunction>(aSelf, Slot::kListExecuteIndirect, TableKind::List))
    {
        original(aSelf, aSignature, aMaximum, aArguments, aArgumentOffset, aCount, aCountOffset);
    }
    if (ListState* const state = StateOf(aSelf); state != nullptr && state->current != nullptr)
    {
        Use& use = *state->current;
        const UINT draws = std::min<UINT>(aMaximum, 65536U);
        use.draws += draws;
        use.indirect += draws;
        use.viewportWidth = std::max(use.viewportWidth, state->viewportWidth);
        use.viewportHeight = std::max(use.viewportHeight, state->viewportHeight);
    }
}

void STDMETHODCALLTYPE DetourRSSetViewports(ID3D12GraphicsCommandList* const aSelf, const UINT aCount,
                                            const D3D12_VIEWPORT* const aViewports)
{
    if (const auto original = OriginalOf<RSSetViewportsFunction>(aSelf, Slot::kListRSSetViewports, TableKind::List))
    {
        original(aSelf, aCount, aViewports);
    }
    if (aCount > 0 && aViewports != nullptr)
    {
        if (ListState* const state = StateOf(aSelf))
        {
            state->viewportWidth = aViewports[0].TopLeftX + aViewports[0].Width;
            state->viewportHeight = aViewports[0].TopLeftY + aViewports[0].Height;
        }
    }
}

void STDMETHODCALLTYPE DetourResourceBarrier(ID3D12GraphicsCommandList* const aSelf, const UINT aCount,
                                             const D3D12_RESOURCE_BARRIER* const aBarriers)
{
    if (aBarriers != nullptr && aCount > 0)
    {
        ListState* state = nullptr;
        for (UINT index = 0; index < aCount; ++index)
        {
            const D3D12_RESOURCE_BARRIER& barrier = aBarriers[index];
            if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
            {
                if ((barrier.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) != 0)
                {
                    continue; // takes effect at its END_ONLY half
                }
                ID3D12Resource* const resource = barrier.Transition.pResource;
                const uint32_t states = static_cast<uint32_t>(barrier.Transition.StateBefore) |
                                        static_cast<uint32_t>(barrier.Transition.StateAfter);
                const bool depth = (states & kDepthStates) != 0;
                if (resource == nullptr || (!depth && !IsKnown(resource)))
                {
                    continue;
                }
                if (depth)
                {
                    Remember(resource);
                }
                if (state == nullptr && (state = StateOf(aSelf)) == nullptr)
                {
                    break;
                }
                if (state->transitions.size() < 4096)
                {
                    state->transitions.push_back({resource, barrier.Transition.Subresource,
                                                  barrier.Transition.StateAfter, StateChange::Kind::Transition});
                }
            }
            else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
            {
                ID3D12Resource* const before = barrier.Aliasing.pResourceBefore;
                ID3D12Resource* const after = barrier.Aliasing.pResourceAfter;
                const bool away = before != nullptr && IsKnown(before);
                const bool back = after != nullptr && IsKnown(after);
                if (!away && !back)
                {
                    continue;
                }
                if (state == nullptr && (state = StateOf(aSelf)) == nullptr)
                {
                    break;
                }
                if (away && state->transitions.size() < 4096)
                {
                    state->transitions.push_back(
                        {before, 0, D3D12_RESOURCE_STATE_COMMON, StateChange::Kind::AliasedAway});
                }
                if (back && state->transitions.size() < 4096)
                {
                    state->transitions.push_back({after, 0, D3D12_RESOURCE_STATE_COMMON, StateChange::Kind::AliasedIn});
                }
            }
        }
    }
    if (const auto original = OriginalOf<ResourceBarrierFunction>(aSelf, Slot::kListResourceBarrier, TableKind::List))
    {
        original(aSelf, aCount, aBarriers);
    }
}

void STDMETHODCALLTYPE DetourOMSetRenderTargets(ID3D12GraphicsCommandList* const aSelf, const UINT aCount,
                                                const D3D12_CPU_DESCRIPTOR_HANDLE* const aTargets,
                                                const BOOL aSingleRange,
                                                const D3D12_CPU_DESCRIPTOR_HANDLE* const aDepthStencil)
{
    if (const auto original =
            OriginalOf<OMSetRenderTargetsFunction>(aSelf, Slot::kListOMSetRenderTargets, TableKind::List))
    {
        original(aSelf, aCount, aTargets, aSingleRange, aDepthStencil);
    }
    if (ListState* const state = StateOf(aSelf))
    {
        state->Bind(aDepthStencil != nullptr ? aDepthStencil->ptr : 0);
    }
}

void STDMETHODCALLTYPE DetourClearDepthStencilView(ID3D12GraphicsCommandList* const aSelf,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE aView,
                                                   const D3D12_CLEAR_FLAGS aFlags, const FLOAT aDepth,
                                                   const UINT8 aStencil, const UINT aRects,
                                                   const D3D12_RECT* const aRectangles)
{
    if (const auto original =
            OriginalOf<ClearDepthStencilViewFunction>(aSelf, Slot::kListClearDepthStencilView, TableKind::List))
    {
        original(aSelf, aView, aFlags, aDepth, aStencil, aRects, aRectangles);
    }
    if ((aFlags & D3D12_CLEAR_FLAG_DEPTH) == 0 || aRects != 0)
    {
        return; // a stencil clear, or part of the buffer: says nothing about which way depth runs
    }
    if (ListState* const state = StateOf(aSelf))
    {
        if (Use* const use = state->UseOf(aView.ptr))
        {
            use->cleared = true;
            use->clearDepth = aDepth;
        }
        if (state->transitions.size() < 4096)
        {
            state->transitions.push_back(
                {nullptr, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, StateChange::Kind::Cleared, aView.ptr});
        }
    }
}

#if defined(__ID3D12GraphicsCommandList4_INTERFACE_DEFINED__)
using BeginRenderPassFunction = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT,
                                                          const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                                          const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
                                                          D3D12_RENDER_PASS_FLAGS);
using EndRenderPassFunction = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);

void STDMETHODCALLTYPE DetourBeginRenderPass(ID3D12GraphicsCommandList4* const aSelf, const UINT aCount,
                                             const D3D12_RENDER_PASS_RENDER_TARGET_DESC* const aTargets,
                                             const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* const aDepthStencil,
                                             const D3D12_RENDER_PASS_FLAGS aFlags)
{
    if (const auto original = OriginalOf<BeginRenderPassFunction>(aSelf, Slot::kListBeginRenderPass, TableKind::List))
    {
        original(aSelf, aCount, aTargets, aDepthStencil, aFlags);
    }
    // The same object as the ID3D12GraphicsCommandList the other hooks see.
    if (ListState* const state = StateOf(aSelf))
    {
        state->Bind(aDepthStencil != nullptr ? aDepthStencil->cpuDescriptor.ptr : 0);
        if (aDepthStencil != nullptr && state->current != nullptr &&
            aDepthStencil->DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
        {
            state->current->cleared = true;
            state->current->clearDepth = aDepthStencil->DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth;
            if (state->transitions.size() < 4096)
            {
                state->transitions.push_back({nullptr, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                              StateChange::Kind::Cleared, aDepthStencil->cpuDescriptor.ptr});
            }
        }
    }
}

void STDMETHODCALLTYPE DetourEndRenderPass(ID3D12GraphicsCommandList4* const aSelf)
{
    if (const auto original = OriginalOf<EndRenderPassFunction>(aSelf, Slot::kListEndRenderPass, TableKind::List))
    {
        original(aSelf);
    }
    if (ListState* const state = StateOf(aSelf))
    {
        state->Bind(0);
    }
}
#endif

// -- queue --------------------------------------------------------------------------

using ExecuteCommandListsFunction = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

void HookListTable(ID3D12GraphicsCommandList* aList);

void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* const aSelf, const UINT aCount,
                                                 ID3D12CommandList* const* const aLists)
{
    if (aLists != nullptr && aCount > 0 && aCount <= 4096)
    {
        // The hooks key a list by its ID3D12GraphicsCommandList pointer, which is the
        // object the other entries are called on.
        thread_local std::vector<ListState*> states;
        states.assign(aCount, nullptr);
        for (UINT index = 0; index < aCount; ++index)
        {
            ID3D12GraphicsCommandList* graphics = nullptr;
            if (aLists[index] == nullptr || aLists[index]->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
                FAILED(aLists[index]->QueryInterface(IID_PPV_ARGS(&graphics))) || graphics == nullptr)
            {
                continue;
            }
            // A list of a kind not seen yet (made before the hooks, or a class of its
            // own): watched from its next recording on.
            if (!IsHooked(*reinterpret_cast<void** const*>(graphics)))
            {
                HookListTable(graphics);
            }
            states[index] = PeekState(graphics);
            graphics->Release();
        }
        const std::scoped_lock lock(s_frameMutex);
        for (UINT index = 0; index < aCount; ++index)
        {
            if (states[index] != nullptr)
            {
                Merge(*states[index]);
            }
        }
    }
    if (const auto original =
            OriginalOf<ExecuteCommandListsFunction>(aSelf, Slot::kQueueExecuteCommandLists, TableKind::Queue))
    {
        original(aSelf, aCount, aLists);
    }
}

// -- putting them on ------------------------------------------------------------------

std::atomic_bool s_enabled{};
std::atomic_bool s_deviceSeen{};
std::atomic_bool s_warnedNotRuntime{};

void HookListTable(ID3D12GraphicsCommandList* const aList)
{
    if (aList == nullptr)
    {
        return;
    }
    void** const table = *reinterpret_cast<void** const*>(aList);
    if (IsHooked(table))
    {
        return;
    }
    if (!IsRuntimeTable(table))
    {
        WarnOnce(s_warnedNotRuntime, std::format("a command list's table lives in {}, not in the Direct3D 12 runtime; "
                                                 "it is not watched.",
                                                 Narrow(ModuleNameOf(table))));
        return;
    }
    std::vector<std::pair<size_t, void*>> entries = {
        {Slot::kListReset, reinterpret_cast<void*>(&DetourReset)},
        {Slot::kListDrawInstanced, reinterpret_cast<void*>(&DetourDrawInstanced)},
        {Slot::kListDrawIndexedInstanced, reinterpret_cast<void*>(&DetourDrawIndexedInstanced)},
        {Slot::kListRSSetViewports, reinterpret_cast<void*>(&DetourRSSetViewports)},
        {Slot::kListResourceBarrier, reinterpret_cast<void*>(&DetourResourceBarrier)},
        {Slot::kListOMSetRenderTargets, reinterpret_cast<void*>(&DetourOMSetRenderTargets)},
        {Slot::kListClearDepthStencilView, reinterpret_cast<void*>(&DetourClearDepthStencilView)},
        {Slot::kListExecuteIndirect, reinterpret_cast<void*>(&DetourExecuteIndirect)},
    };
#if defined(__ID3D12GraphicsCommandList4_INTERFACE_DEFINED__)
    ID3D12GraphicsCommandList4* passes = nullptr;
    if (SUCCEEDED(aList->QueryInterface(IID_PPV_ARGS(&passes))) && passes != nullptr)
    {
        if (*reinterpret_cast<void** const*>(passes) == table)
        {
            entries.emplace_back(Slot::kListBeginRenderPass, reinterpret_cast<void*>(&DetourBeginRenderPass));
            entries.emplace_back(Slot::kListEndRenderPass, reinterpret_cast<void*>(&DetourEndRenderPass));
        }
        passes->Release();
    }
#endif
    static_cast<void>(PatchTable(table, TableKind::List, entries, "command list"));
}

void HookQueueTable(ID3D12CommandQueue* const aQueue)
{
    if (aQueue == nullptr)
    {
        return;
    }
    void** const table = *reinterpret_cast<void** const*>(aQueue);
    if (IsHooked(table))
    {
        return;
    }
    if (!IsRuntimeTable(table))
    {
        WarnOnce(s_warnedNotRuntime, std::format("a command queue's table lives in {}, not in the Direct3D 12 "
                                                 "runtime; it is not watched.",
                                                 Narrow(ModuleNameOf(table))));
        return;
    }
    const std::pair<size_t, void*> entries[] = {
        {Slot::kQueueExecuteCommandLists, reinterpret_cast<void*>(&DetourExecuteCommandLists)},
    };
    static_cast<void>(PatchTable(table, TableKind::Queue, entries, "command queue"));
}

/// Hooks a device and, through objects made on it for a moment, the runtime's
/// queue and command list tables.
void HookDevice(ID3D12Device* const aDevice, const char* const aWhen)
{
    if (aDevice == nullptr || !s_enabled.load(std::memory_order_acquire))
    {
        return;
    }
    void** const table = *reinterpret_cast<void** const*>(aDevice);
    if (!IsHooked(table))
    {
        if (!IsRuntimeTable(table))
        {
            WarnOnce(s_warnedNotRuntime, std::format("the device's table lives in {}, not in the Direct3D 12 "
                                                     "runtime; depth buffers are not watched.",
                                                     Narrow(ModuleNameOf(table))));
            return;
        }
        const std::pair<size_t, void*> entries[] = {
            {Slot::kDeviceCreateDepthStencilView, reinterpret_cast<void*>(&DetourCreateDepthStencilView)},
            {Slot::kDeviceCopyDescriptors, reinterpret_cast<void*>(&DetourCopyDescriptors)},
            {Slot::kDeviceCopyDescriptorsSimple, reinterpret_cast<void*>(&DetourCopyDescriptorsSimple)},
            {Slot::kDeviceCreateCommittedResource, reinterpret_cast<void*>(&DetourCreateCommittedResource)},
            {Slot::kDeviceCreatePlacedResource, reinterpret_cast<void*>(&DetourCreatePlacedResource)},
        };
        if (!PatchTable(table, TableKind::Device, entries, "device"))
        {
            return;
        }
        Log(LogLevel::Info, std::format("watching the game's Direct3D 12 device {} ({}).", Hex(aDevice), aWhen));
    }
    s_deviceSeen.store(true, std::memory_order_release);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    if (SUCCEEDED(aDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) && queue != nullptr)
    {
        HookQueueTable(queue);
        queue->Release();
    }
    ID3D12CommandAllocator* allocator = nullptr;
    if (SUCCEEDED(aDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
        allocator != nullptr)
    {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(aDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                                 IID_PPV_ARGS(&list))) &&
            list != nullptr)
        {
            list->Close();
            HookListTable(list);
            list->Release();
        }
        allocator->Release();
    }
}

using CreateDeviceFunction = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
CreateDeviceFunction s_createDevice{};

HRESULT WINAPI DetourCreateDevice(IUnknown* const aAdapter, const D3D_FEATURE_LEVEL aLevel, REFIID aInterface,
                                  void** const aDevice)
{
    if (s_createDevice == nullptr)
    {
        return E_FAIL;
    }
    const HRESULT result = s_createDevice(aAdapter, aLevel, aInterface, aDevice);
    if (SUCCEEDED(result) && aDevice != nullptr && *aDevice != nullptr)
    {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*aDevice)->QueryInterface(IID_PPV_ARGS(&device))) && device != nullptr)
        {
            HookDevice(device, "as the game created it");
            device->Release();
        }
    }
    return result;
}

// ---------------------------------------------------------------------------------
// The overlay's side: everything below runs on the thread that renders the overlay
// ---------------------------------------------------------------------------------

AttachFunction s_attach{};

// How the test is judged in the game. The prop's own glass (or the panel just
// behind it) is in the depth buffer within a few centimetres of the picture's
// plane; a hand or a door frame is far further in front. B is used 1.5 % large,
// which can only make the game's geometry look a little further away: a measured B
// a touch small would otherwise hide the picture behind its own television.
constexpr float kToleranceMetres = 0.05F;
constexpr float kRelativeTolerance = 0.015F;
constexpr float kSoftnessMetres = 0.03F;
constexpr double kModelBias = 1.015;
// Until B is measured, the debug view shades depth as if it were this.
constexpr double kDisplayB = 0.05;
// A copy is made only while a screen was drawn this recently (presents), or the
// debug view is on: a few seconds, so that a screen turned back to is tested from
// its first present.
constexpr uint64_t kCopyWhileScreensWithin = 600;
// A change of buffer is logged at most this often; the status line counts them all.
constexpr auto kSwitchLogEvery = std::chrono::seconds(5);
constexpr size_t kReadbackSlots = 4;
constexpr size_t kSamplesPerSlot = Policy::kSamplesPerScreen * 4;
constexpr UINT64 kSampleStride = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

ID3D12Device* s_device{};
D3D12_CPU_DESCRIPTOR_HANDLE s_viewCpu{};
D3D12_GPU_DESCRIPTOR_HANDLE s_viewGpu{};
SD::Pass s_pass;
bool s_graphicsReady{};

/// The overlay's copy of the game's depth.
struct Copy
{
    ID3D12Resource* texture{};
    D3D12_RESOURCE_DESC desc{};
    Policy::Family family{Policy::Family::None};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COPY_DEST};
    DXGI_FORMAT footprintFormat{DXGI_FORMAT_UNKNOWN};
    uint64_t lastFence{};
};
Copy s_copy;

struct Retired
{
    ID3D12Resource* texture{};
    uint64_t lastFence{};
};
std::vector<Retired> s_retired;

/// The buffer chosen at this present (from the work of the last several), and what
/// the game did with it since the previous one.
struct Selection
{
    ID3D12Resource* resource{};
    D3D12_RESOURCE_DESC desc{};
    Policy::Family family{Policy::Family::None};
    float extent[2]{};
    uint32_t draws{};   ///< summed over recent presents (Policy::History)
    uint32_t indirect{};
    uint64_t vertices{};
    bool readByShaders{};
    std::array<D3D12_RESOURCE_STATES, 2> state{};
    bool stateKnown{};
    bool initialKnown{};
    Policy::Convention convention{Policy::Convention::Unknown};
    float clearDepth{};
    bool cleared{};
    bool seen{};        ///< used by the lists run since the previous present
    bool aliasedAway{}; ///< ...and its memory given to another resource at their end
    uint64_t writes{};
    bool settledSinceClear{};
    bool everSettled{};
};
Selection s_selection;
std::string s_candidatesText;
Policy::History s_history;
/// The description of every buffer the history remembers, by its id.
std::unordered_map<uint64_t, D3D12_RESOURCE_DESC> s_descs;
std::chrono::steady_clock::time_point s_lastSwitchLog{};

ID3D12GraphicsCommandList* s_frameList{};
float s_overlayWidth{1.0F};
float s_overlayHeight{1.0F};
uint64_t s_present{};
uint64_t s_lastScreenAt{};
uint64_t s_lastFence{};
bool s_copied{}; ///< the copy was made at this present

/// What the overlay's copy holds: which buffer it came from, at which of its
/// writes, at which present, and the part of it the game drew. Screens are tested
/// against it at every present until a newer copy is made, for at most
/// Policy::kCopyFreshForPresents: a game with frame generation presents several
/// images per frame it renders.
ID3D12Resource* s_copyOf{};
uint64_t s_copyWrites{};
uint64_t s_copyPresent{};
uint32_t s_waitsSinceCopy{};
float s_copyExtent[2]{};
bool s_copyUsable{}; ///< for this present: there is a copy, and it is fresh

bool s_testOn{true};
bool s_debugView{};
int s_override{-1};
std::array<bool, 3> s_keysDown{};

Policy::Calibration s_calibration;
bool s_wasLocked{};
double s_lockedB{};

/// One present's calibration points on their way back from the GPU. Each point's
/// texel is copied on its own into `s_readback` (one 512-byte cell each), or, when
/// the copy is a depth buffer and so can only be copied whole, the depth plane is
/// copied whole into `s_planeReadback` and the points are read out of it.
struct ReadbackSlot
{
    bool filling{};
    bool pending{};
    bool wholePlane{};
    uint64_t present{};
    uint64_t fence{};
    size_t count{};
    size_t groups{};
    std::array<Policy::Sample, kSamplesPerSlot> samples{};
    std::array<UINT64, kSamplesPerSlot> offset{};
    std::array<uint8_t, kSamplesPerSlot> group{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
};
ID3D12Resource* s_readback{};
std::array<ReadbackSlot, kReadbackSlots> s_slots{};
ID3D12Resource* s_planeReadback{};
D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_planeLayout{};
UINT64 s_planeBytes{};

// What the status line reports.
struct Counters
{
    uint64_t presents{};
    uint64_t copies{};
    uint64_t waits{}; ///< presents at which the chosen buffer was mid-frame, so the last copy was kept
    uint64_t screens{};
    uint64_t screensTested{};
    uint64_t switches{};
    uint64_t noCandidate{};
    uint64_t aliased{};
    uint64_t unresolvedDraws{};
    uint64_t groupsUsed{};
};
Counters s_counters;
Counters s_countersAtStatus;
std::chrono::steady_clock::time_point s_lastStatus{};

std::atomic_bool s_warnedState{};
std::atomic_bool s_warnedAliased{};
std::atomic_bool s_warnedCopy{};
std::atomic_bool s_warnedFormat{};
std::atomic_bool s_warnedUnresolved{};
std::atomic_bool s_warnedConvention{};

template <typename T>
void SafeRelease(T*& aObject)
{
    if (aObject != nullptr)
    {
        aObject->Release();
        aObject = nullptr;
    }
}

std::string DescribeResource(const D3D12_RESOURCE_DESC& aDesc, const float aExtentWidth, const float aExtentHeight)
{
    std::string text = std::format("{}x{} {}", aDesc.Width, aDesc.Height,
                                   Policy::Describe(Policy::FamilyOf(static_cast<uint32_t>(aDesc.Format))));
    if (aExtentWidth > 0.0F && (static_cast<UINT64>(aExtentWidth) != aDesc.Width ||
                                static_cast<UINT>(aExtentHeight) != aDesc.Height))
    {
        text += std::format(" (drawn {}x{})", static_cast<int>(aExtentWidth), static_cast<int>(aExtentHeight));
    }
    return text;
}

void ReleaseRetired(const uint64_t aCompletedFence, const bool aAll)
{
    std::erase_if(s_retired, [&](Retired& aRetired) {
        if (aAll || aRetired.lastFence <= aCompletedFence)
        {
            SafeRelease(aRetired.texture);
            return true;
        }
        return false;
    });
}

/// The shape a copy texture could not be made in, so it is not tried every frame.
struct Refused
{
    UINT64 width{};
    UINT height{};
    Policy::Family family{Policy::Family::None};
};
Refused s_refused;

/// A copy texture shaped like `aDesc`, and the view of it in the overlay's heap.
bool EnsureCopy(const D3D12_RESOURCE_DESC& aDesc, const Policy::Family aFamily)
{
    if (s_copy.texture != nullptr && s_copy.desc.Width == aDesc.Width && s_copy.desc.Height == aDesc.Height &&
        s_copy.family == aFamily)
    {
        return true;
    }
    if (s_refused.width == aDesc.Width && s_refused.height == aDesc.Height && s_refused.family == aFamily)
    {
        return false;
    }
    if (s_copy.texture != nullptr)
    {
        // Frames still in flight may read it; the view is rewritten below, which a
        // frame being drawn as the game changes resolution can see. It is gone a
        // frame later.
        s_retired.push_back({s_copy.texture, s_lastFence});
        s_copy.texture = nullptr;
    }
    s_copy = Copy{};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = aDesc.Width;
    desc.Height = aDesc.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = static_cast<DXGI_FORMAT>(Policy::CopyFormat(aFamily));
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(s_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&s_copy.texture))) ||
        s_copy.texture == nullptr)
    {
        // Some runtimes (Wine's) only make the two-plane formats as depth buffers. A
        // copy made as one still copies and still reads, but its texels cannot be
        // copied out one at a time: the calibration then copies the plane whole.
        s_copy.texture = nullptr;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (FAILED(s_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&s_copy.texture))) ||
            s_copy.texture == nullptr)
        {
            WarnOnce(s_warnedCopy, std::format("could not make a {}x{} {} texture to copy the depth into.",
                                               desc.Width, desc.Height, Policy::Describe(aFamily)));
            s_copy = Copy{};
            s_refused = Refused{aDesc.Width, aDesc.Height, aFamily};
            return false;
        }
        WarnOnce(s_warnedCopy, std::format("the {} copy had to be made as a depth buffer, so its points are read "
                                           "back from whole copies of it.",
                                           Policy::Describe(aFamily)));
    }
    s_copy.texture->SetName(L"Open77 TV depth copy");
    s_copy.desc = desc;
    s_copy.family = aFamily;
    s_copy.state = D3D12_RESOURCE_STATE_COPY_DEST;

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = static_cast<DXGI_FORMAT>(Policy::ReadFormat(aFamily));
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    view.Texture2D.PlaneSlice = 0;
    s_device->CreateShaderResourceView(s_copy.texture, &view, s_viewCpu);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 total = 0;
    s_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &rowBytes, &total);
    s_copy.footprintFormat = layout.Footprint.Format;
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 &&
        (s_planeReadback == nullptr || s_planeBytes < total))
    {
        SafeRelease(s_planeReadback);
        D3D12_HEAP_PROPERTIES readback{};
        readback.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&s_planeReadback))))
        {
            s_planeReadback = nullptr;
        }
        s_planeBytes = s_planeReadback != nullptr ? total : 0;
    }
    s_planeLayout = layout;
    Log(LogLevel::Info, std::format("copying the depth into a {}x{} {} texture (read as format {}, plane 0 laid out "
                                    "as format {}).",
                                    desc.Width, desc.Height, Policy::Describe(aFamily),
                                    static_cast<int>(view.Format), static_cast<int>(layout.Footprint.Format)));
    return true;
}

void Transition(ID3D12GraphicsCommandList* const aList, ID3D12Resource* const aResource, const UINT aSubresource,
                const D3D12_RESOURCE_STATES aBefore, const D3D12_RESOURCE_STATES aAfter)
{
    if (aBefore == aAfter)
    {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = aResource;
    barrier.Transition.Subresource = aSubresource;
    barrier.Transition.StateBefore = aBefore;
    barrier.Transition.StateAfter = aAfter;
    aList->ResourceBarrier(1, &barrier);
}

/// Copies the selected buffer into the overlay's texture, leaving the buffer in
/// exactly the state(s) the game left it in.
bool RecordCopy(ID3D12GraphicsCommandList* const aList)
{
    const Selection& chosen = s_selection;
    const uint32_t planes = Policy::PlaneCount(chosen.family);
    const auto readable = [](const D3D12_RESOURCE_STATES aState) {
        return (static_cast<uint32_t>(aState) & static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE)) != 0;
    };
    std::array<D3D12_RESOURCE_STATES, 2> before = chosen.state;
    if (planes == 1)
    {
        before[1] = before[0];
    }
    const bool same = planes == 1 || before[0] == before[1];
    if (same)
    {
        if (!readable(before[0]))
        {
            Transition(aList, chosen.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before[0],
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
    }
    else
    {
        for (UINT plane = 0; plane < planes; ++plane)
        {
            if (!readable(before[plane]))
            {
                Transition(aList, chosen.resource, plane, before[plane], D3D12_RESOURCE_STATE_COPY_SOURCE);
            }
        }
    }
    Transition(aList, s_copy.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, s_copy.state,
               D3D12_RESOURCE_STATE_COPY_DEST);
    aList->CopyResource(s_copy.texture, chosen.resource);
    if (same)
    {
        if (!readable(before[0]))
        {
            Transition(aList, chosen.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_COPY_SOURCE, before[0]);
        }
    }
    else
    {
        for (UINT plane = 0; plane < planes; ++plane)
        {
            if (!readable(before[plane]))
            {
                Transition(aList, chosen.resource, plane, D3D12_RESOURCE_STATE_COPY_SOURCE, before[plane]);
            }
        }
    }
    Transition(aList, s_copy.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s_copy.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return true;
}

// -- calibration readback -------------------------------------------------------------

ReadbackSlot* FillingSlot()
{
    for (auto& slot : s_slots)
    {
        if (slot.filling && slot.present == s_present)
        {
            return &slot;
        }
    }
    for (auto& slot : s_slots)
    {
        if (!slot.filling && !slot.pending)
        {
            slot = ReadbackSlot{};
            slot.filling = true;
            slot.present = s_present;
            slot.format = s_copy.footprintFormat;
            return &slot;
        }
    }
    return nullptr;
}

void ProcessReadbacks(const uint64_t aCompletedFence)
{
    for (size_t index = 0; index < s_slots.size(); ++index)
    {
        ReadbackSlot& slot = s_slots[index];
        if (slot.filling && slot.present != s_present)
        {
            slot = ReadbackSlot{}; // its present was never submitted
            continue;
        }
        if (!slot.pending || slot.fence > aCompletedFence)
        {
            continue;
        }
        ID3D12Resource* const buffer = slot.wholePlane ? s_planeReadback : s_readback;
        if (buffer == nullptr || slot.count == 0)
        {
            slot = ReadbackSlot{};
            continue;
        }
        UINT64 first = slot.offset[0];
        UINT64 last = slot.offset[0];
        for (size_t sample = 0; sample < slot.count; ++sample)
        {
            first = std::min(first, slot.offset[sample]);
            last = std::max(last, slot.offset[sample]);
        }
        const D3D12_RANGE range{static_cast<SIZE_T>(first), static_cast<SIZE_T>(last + 8)};
        uint8_t* mapped = nullptr;
        if (SUCCEEDED(buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) && mapped != nullptr)
        {
            for (size_t sample = 0; sample < slot.count; ++sample)
            {
                slot.samples[sample].device =
                    Policy::DecodeTexel(static_cast<uint32_t>(slot.format), mapped + slot.offset[sample]);
            }
            const D3D12_RANGE written{0, 0};
            buffer->Unmap(0, &written);
            size_t begin = 0;
            while (begin < slot.count)
            {
                size_t end = begin;
                while (end < slot.count && slot.group[end] == slot.group[begin])
                {
                    ++end;
                }
                if (s_calibration.AddGroup(std::span<const Policy::Sample>(&slot.samples[begin], end - begin)))
                {
                    ++s_counters.groupsUsed;
                }
                begin = end;
            }
        }
        slot = ReadbackSlot{};
    }
    if (s_calibration.Locked() && (!s_wasLocked || s_calibration.B() != s_lockedB))
    {
        Log(LogLevel::Info,
            std::format("{} what the depth means: device = {} + {:.5f} / metres ({} depth; {} screen samples, spread "
                        "{:.1f} %). The test is on.",
                        s_wasLocked ? "measured again" : "measured", s_calibration.A(), s_calibration.B(),
                        Policy::Describe(s_calibration.Direction()), s_calibration.Groups(),
                        s_calibration.LastSpread() * 100.0));
        s_wasLocked = true;
        s_lockedB = s_calibration.B();
    }
}

/// Bytes per texel of a copy footprint's format (the depth plane as a copy lays it
/// out), 0 for one this does not read.
UINT FootprintTexelBytes(const DXGI_FORMAT aFormat)
{
    switch (aFormat)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return 8;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return 4;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM: return 2;
    default: return 0;
    }
}

/// Copies the depth under a few points in the middle of a screen to a readback
/// buffer, with the screen's own inverse view depth at each, for the calibration.
/// Only at a present that copied the depth: then the screen's corners and the depth
/// are of the same moment, which at the presents in between (frame generation's)
/// they are not quite. A screen with part of it behind the camera passes its map
/// (`aClipped`) instead of usable corners; its samples behind the near plane are
/// skipped like samples off the edge of the view.
void RecordSamples(ID3D12GraphicsCommandList* const aList, const SD::Constants& aConstants,
                   const float (&aCorners)[8], const Clip::Map* const aClipped = nullptr)
{
    if (s_copy.texture == nullptr || !s_copied || s_calibration.Direction() == Policy::Convention::Unknown)
    {
        return;
    }
    const bool wholePlane = (s_copy.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    const UINT texelBytes = FootprintTexelBytes(s_copy.footprintFormat);
    if (texelBytes == 0 ||
        Policy::DecodeTexel(static_cast<uint32_t>(s_copy.footprintFormat), std::array<uint8_t, 8>{}.data()) < 0.0)
    {
        WarnOnce(s_warnedFormat, std::format("the depth copy's plane is laid out as format {}, which this does not "
                                             "read; the test cannot be calibrated.",
                                             static_cast<int>(s_copy.footprintFormat)));
        return;
    }
    if ((wholePlane ? s_planeReadback : s_readback) == nullptr)
    {
        return;
    }
    ReadbackSlot* const slot = FillingSlot();
    if (slot == nullptr || slot->count + Policy::kSamplesPerScreen > kSamplesPerSlot || slot->groups >= 255)
    {
        return;
    }
    if (wholePlane && !slot->wholePlane)
    {
        // One whole-plane copy in flight at a time: there is one buffer for it.
        for (const auto& other : s_slots)
        {
            if (&other != slot && other.wholePlane && (other.pending || other.filling))
            {
                return;
            }
        }
    }
    Tess::Homography map;
    if (aClipped == nullptr && Tess::Solve(aCorners, map) != Tess::Status::Ok)
    {
        return;
    }
    // Where a sample lands: through the corners' homography, or the cut screen's own
    // map -- off the view (and so skipped below) where that is behind the near plane.
    const auto place = [&map, aClipped](const float aU, const float aV) {
        if (aClipped == nullptr)
        {
            return map.Apply(aU, aV);
        }
        if (!(aClipped->Depth(aU, aV) > Clip::kNearDepth))
        {
            return Tess::Point{-1.0F, -1.0F};
        }
        const std::array<double, 2> at = aClipped->Apply(aU, aV);
        return Tess::Point{static_cast<float>(at[0]), static_cast<float>(at[1])};
    };
    const float scaleX = s_copyExtent[0] / s_overlayWidth;
    const float scaleY = s_copyExtent[1] / s_overlayHeight;
    const size_t index = static_cast<size_t>(slot - s_slots.data());
    const UINT64 base = static_cast<UINT64>(index) * kSamplesPerSlot * kSampleStride;
    Transition(aList, s_copy.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, s_copy.state,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = s_copy.texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    if (wholePlane && !slot->wholePlane)
    {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = s_planeReadback;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = s_planeLayout;
        aList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        slot->wholePlane = true;
    }
    for (const float v : Policy::kSampleCoordinates)
    {
        for (const float u : Policy::kSampleCoordinates)
        {
            const Tess::Point pixel = place(u, v);
            const double inverse = (static_cast<double>(aConstants.plane[0]) * pixel.x) +
                                   (static_cast<double>(aConstants.plane[1]) * pixel.y) + aConstants.plane[2];
            const float texelX = std::floor(pixel.x * scaleX);
            const float texelY = std::floor(pixel.y * scaleY);
            if (!(texelX >= 0.0F) || !(texelY >= 0.0F) || texelX >= s_copyExtent[0] ||
                texelY >= s_copyExtent[1] || !(inverse > 0.0))
            {
                continue;
            }
            const auto x = static_cast<UINT>(texelX);
            const auto y = static_cast<UINT>(texelY);
            if (wholePlane)
            {
                slot->offset[slot->count] = s_planeLayout.Offset +
                                            (static_cast<UINT64>(y) * s_planeLayout.Footprint.RowPitch) +
                                            (static_cast<UINT64>(x) * texelBytes);
            }
            else
            {
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = s_readback;
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint.Offset = base + (slot->count * kSampleStride);
                destination.PlacedFootprint.Footprint.Format = s_copy.footprintFormat;
                destination.PlacedFootprint.Footprint.Width = 1;
                destination.PlacedFootprint.Footprint.Height = 1;
                destination.PlacedFootprint.Footprint.Depth = 1;
                destination.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
                const D3D12_BOX box{x, y, 0, x + 1, y + 1, 1};
                aList->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
                slot->offset[slot->count] = destination.PlacedFootprint.Offset;
            }
            slot->samples[slot->count] = Policy::Sample{inverse, -1.0};
            slot->group[slot->count] = static_cast<uint8_t>(slot->groups);
            ++slot->count;
        }
    }
    ++slot->groups;
    Transition(aList, s_copy.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE,
               s_copy.state);
}

// -- drawing ----------------------------------------------------------------------------

struct ScreenDraw
{
    float corners[8]{};
    float centreDepth{};
    float opacity{};
    UINT64 picture{};
    bool debug{};
    /// A screen with part of it behind the camera (ScreenClip.hpp): drawn from its
    /// map, over the part in front of the near plane -- `polygon`, `count` corners in
    /// overlay pixels -- and `corners`/`centreDepth` unused.
    bool clipped{};
    int count{};
    float polygon[2 * Clip::kMaximumCorners]{};
    double map[3][3]{};
};

/// This present's sizes and model, as the pass's constants want them.
struct Frame
{
    float overlay[2]{};
    float extent[2]{};
    float texture[2]{};
    SD::DepthModel model;
    bool depth{};
    bool test{};
};

Frame CurrentFrame()
{
    Frame frame;
    frame.overlay[0] = s_overlayWidth;
    frame.overlay[1] = s_overlayHeight;
    frame.depth = s_copyUsable && s_copy.texture != nullptr;
    if (frame.depth)
    {
        frame.extent[0] = s_copyExtent[0];
        frame.extent[1] = s_copyExtent[1];
        frame.texture[0] = static_cast<float>(s_copy.desc.Width);
        frame.texture[1] = static_cast<float>(s_copy.desc.Height);
    }
    else
    {
        for (int axis = 0; axis < 2; ++axis)
        {
            frame.extent[axis] = frame.overlay[axis];
            frame.texture[axis] = frame.overlay[axis];
        }
    }
    const bool locked = s_calibration.Locked();
    frame.model.a = s_calibration.A();
    frame.model.b = locked ? s_calibration.B() * kModelBias
                           : (s_calibration.Direction() == Policy::Convention::Conventional ? -kDisplayB : kDisplayB);
    frame.test = frame.depth && locked && s_testOn;
    return frame;
}

void ScreenCallback(const ImDrawList*, const ImDrawCmd* const aCommand)
{
    ID3D12GraphicsCommandList* const list = s_frameList;
    if (list == nullptr || !s_pass.Ready() || aCommand == nullptr || aCommand->UserCallbackData == nullptr)
    {
        return;
    }
    ScreenDraw draw;
    std::memcpy(&draw, aCommand->UserCallbackData, sizeof(draw));
    const Frame frame = CurrentFrame();
    SD::Tuning tuning;
    tuning.toleranceMetres = kToleranceMetres;
    tuning.relativeTolerance = kRelativeTolerance;
    tuning.softnessMetres = kSoftnessMetres;
    tuning.opacity = draw.opacity;
    SD::Constants constants;
    const bool useDepth = draw.debug ? frame.depth : frame.test;
    Clip::Map map;
    if (draw.clipped)
    {
        std::memcpy(map.m, draw.map, sizeof(map.m));
        if (SD::BuildFromMap(map, frame.model, frame.overlay, frame.extent, frame.texture, tuning, useDepth,
                             constants) != SD::Status::Ok)
        {
            return;
        }
    }
    else if (SD::Build(draw.corners, draw.centreDepth, frame.model, frame.overlay, frame.extent, frame.texture,
                       tuning, useDepth, constants) != SD::Status::Ok)
    {
        return;
    }
    if (draw.debug)
    {
        if (!frame.depth)
        {
            return;
        }
        constants.look[3] = 1.0F; // the depth itself, not the picture
    }
    else if (frame.depth)
    {
        RecordSamples(list, constants, draw.corners, draw.clipped ? &map : nullptr);
    }
    const D3D12_RECT clip{static_cast<LONG>(aCommand->ClipRect.x), static_cast<LONG>(aCommand->ClipRect.y),
                          static_cast<LONG>(aCommand->ClipRect.z), static_cast<LONG>(aCommand->ClipRect.w)};
    list->RSSetScissorRects(1, &clip);
    D3D12_GPU_DESCRIPTOR_HANDLE picture{};
    picture.ptr = draw.picture;
    const D3D12_GPU_DESCRIPTOR_HANDLE depth = frame.depth ? s_viewGpu : picture;
    const SD::Geometry geometry = draw.clipped
                                      ? SD::MakePolygonGeometry(draw.polygon, draw.count, frame.overlay[0],
                                                                frame.overlay[1])
                                      : SD::MakeGeometry(draw.corners, frame.overlay[0], frame.overlay[1]);
    s_pass.Draw(list, constants, geometry, draw.debug ? depth : picture, depth);
    if (!draw.debug)
    {
        ++s_counters.screens;
        if (frame.test)
        {
            ++s_counters.screensTested;
        }
    }
}

// -- keys, status -------------------------------------------------------------------------

bool GameIsForeground()
{
    const HWND window = GetForegroundWindow();
    DWORD process = 0;
    return window != nullptr && GetWindowThreadProcessId(window, &process) != 0 && process == GetCurrentProcessId();
}

void PollKeys()
{
    const bool chord = GameIsForeground() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                       (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const int keys[3] = {VK_F9, VK_F10, VK_F11};
    for (size_t index = 0; index < 3; ++index)
    {
        const bool down = chord && (GetAsyncKeyState(keys[index]) & 0x8000) != 0;
        const bool pressed = down && !s_keysDown[index];
        s_keysDown[index] = down;
        if (!pressed)
        {
            continue;
        }
        if (index == 0)
        {
            s_override = s_override >= 7 ? -1 : s_override + 1;
            Log(LogLevel::Info, s_override < 0 ? std::string("candidate: automatic again.")
                                               : std::format("candidate: trying number {} of the ranked list.",
                                                             s_override + 1));
        }
        else if (index == 1)
        {
            s_debugView = !s_debugView;
            Log(LogLevel::Info, s_debugView ? "debug view on." : "debug view off.");
        }
        else
        {
            s_testOn = !s_testOn;
            Log(LogLevel::Info, s_testOn ? "test on: screens are hidden behind what is in front of them."
                                         : "test off: screens are drawn over everything, as before.");
        }
    }
}

void LogStatus()
{
    const auto now = std::chrono::steady_clock::now();
    if (s_lastStatus != std::chrono::steady_clock::time_point{} && now - s_lastStatus < std::chrono::seconds(30))
    {
        return;
    }
    const Counters& was = s_countersAtStatus;
    const Counters& is = s_counters;
    if (s_lastStatus != std::chrono::steady_clock::time_point{} && is.screens == was.screens && !s_debugView)
    {
        s_lastStatus = now;
        return; // no screen drawn since the last line: nothing to say
    }
    std::string chosen = "none";
    if (s_selection.resource != nullptr)
    {
        chosen = std::format("{} {} state {}{} draws {} vertices {} indirect {} {}",
                             Hex(s_selection.resource),
                             DescribeResource(s_selection.desc, s_selection.extent[0], s_selection.extent[1]),
                             StateName(s_selection.state[0]), s_selection.stateKnown ? "" : " (assumed)",
                             s_selection.draws, s_selection.vertices, s_selection.indirect,
                             Policy::Describe(s_selection.convention));
    }
    Log(LogLevel::Info,
        std::format("status: buffer {}; model {}; test {}; last 30 s: {} presents, {} copies, {} waits for a "
                    "finished frame, {} screens drawn, {} through the test, {} samples used, {} switches, {} presents "
                    "without a buffer, {} draws into views made before the hooks, {} aliased.",
                    chosen,
                    s_calibration.Locked()
                        ? std::format("device = {} + {:.5f} / z", s_calibration.A(), s_calibration.B())
                        : std::format("measuring ({} samples, spread {:.1f} %)", s_calibration.Groups(),
                                      s_calibration.LastSpread() * 100.0),
                    s_testOn ? "on" : "off", is.presents - was.presents, is.copies - was.copies,
                    is.waits - was.waits, is.screens - was.screens, is.screensTested - was.screensTested,
                    is.groupsUsed - was.groupsUsed, is.switches - was.switches, is.noCandidate - was.noCandidate,
                    is.unresolvedDraws - was.unresolvedDraws, is.aliased - was.aliased));
    s_countersAtStatus = s_counters;
    s_lastStatus = now;
}

// -- choosing ---------------------------------------------------------------------------

void Choose(FrameRecord& aFrame)
{
    // This present's work, per buffer.
    std::vector<Policy::Candidate> present;
    for (const auto& [resource, use] : aFrame.uses)
    {
        if (!use.haveDesc)
        {
            continue;
        }
        Policy::Candidate candidate;
        candidate.id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(resource));
        candidate.width = static_cast<uint32_t>(std::min<UINT64>(use.desc.Width, 0xFFFFFFFFULL));
        candidate.height = use.desc.Height;
        candidate.format = static_cast<uint32_t>(use.desc.Format);
        candidate.samples = use.desc.SampleDesc.Count;
        candidate.mipLevels = use.desc.MipLevels;
        candidate.arraySize = use.desc.DepthOrArraySize;
        candidate.depthStencil = (use.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
        candidate.twoDimensional = use.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        candidate.draws = use.draws;
        candidate.indirectDraws = use.indirect;
        candidate.vertices = use.vertices;
        candidate.viewportWidth = use.viewportWidth;
        candidate.viewportHeight = use.viewportHeight;
        candidate.readByShaders = use.readByShaders;
        present.push_back(candidate);
        s_descs[candidate.id] = use.desc;
    }
    // Chosen by the work of the last several presents, not this one's: with frame
    // generation the game's frame spans several, the scene's buffer drawn in one
    // of them and small passes' buffers in others.
    s_history.Add(present);
    const std::vector<Policy::Candidate>& summed = s_history.Candidates();
    std::erase_if(s_descs, [&](const auto& aEntry) {
        return std::none_of(summed.begin(), summed.end(),
                            [&](const Policy::Candidate& aCandidate) { return aCandidate.id == aEntry.first; });
    });
    const std::vector<size_t> ranked = Policy::Rank(summed, s_overlayWidth, s_overlayHeight);
    const size_t chosen = Policy::Choose(summed, ranked,
                                         static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s_selection.resource)),
                                         s_override);
    // For the debug view: the top of the list.
    s_candidatesText.clear();
    for (size_t place = 0; s_debugView && place < ranked.size() && place < 4; ++place)
    {
        const auto& candidate = summed[ranked[place]];
        const Policy::Extent extent = Policy::DrawnExtent(candidate);
        const auto desc = s_descs.find(candidate.id);
        s_candidatesText += std::format(
            "{}{} {}: {} draws, {} vertices{}\n", ranked[place] == chosen ? "> " : "  ", place + 1,
            desc != s_descs.end() ? DescribeResource(desc->second, extent.width, extent.height) : std::string("?"),
            candidate.draws, candidate.vertices, candidate.readByShaders ? ", sampled" : "");
    }
    const auto desc = chosen < summed.size() ? s_descs.find(summed[chosen].id) : s_descs.end();
    if (desc == s_descs.end())
    {
        ++s_counters.noCandidate;
        s_selection.seen = false; // nothing of the game's to touch at this present
        return;
    }
    const Policy::Candidate& candidate = summed[chosen];
    auto* const resource = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(candidate.id));
    Track track;
    {
        const std::scoped_lock lock(s_frameMutex);
        if (const auto found = s_tracks.find(resource); found != s_tracks.end())
        {
            track = found->second;
        }
    }
    const auto use = aFrame.uses.find(resource);
    const Policy::Extent extent = Policy::DrawnExtent(candidate);
    Selection selection;
    selection.resource = resource;
    selection.desc = desc->second;
    selection.family = Policy::FamilyOf(candidate.format);
    selection.extent[0] = extent.width;
    selection.extent[1] = extent.height;
    selection.draws = candidate.draws;
    selection.indirect = candidate.indirectDraws;
    selection.vertices = candidate.vertices;
    selection.readByShaders = candidate.readByShaders;
    selection.stateKnown = track.known[0];
    selection.initialKnown = track.initialKnown;
    selection.state = track.known[0] ? track.state : std::array<D3D12_RESOURCE_STATES, 2>{
                                                         D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                         D3D12_RESOURCE_STATE_DEPTH_WRITE};
    if (track.known[0] && !track.known[1])
    {
        selection.state[1] = selection.state[0];
    }
    selection.cleared = track.cleared;
    selection.clearDepth = track.clearDepth;
    selection.convention = selection.cleared ? Policy::FromClear(selection.clearDepth) : Policy::Convention::Unknown;
    if (selection.convention == Policy::Convention::Unknown)
    {
        WarnOnce(s_warnedConvention,
                 selection.cleared
                     ? std::format("the depth buffer is cleared to {}, neither 0 nor 1; taken as reversed (near is 1).",
                                   selection.clearDepth)
                     : std::string("no clear of the depth buffer was seen; taken as reversed (near is 1)."));
        selection.convention = Policy::Convention::Reversed;
    }
    if (!selection.stateKnown)
    {
        WarnOnce(s_warnedState, "no state change of the depth buffer has been seen; it is taken to be in DEPTH_WRITE "
                                "at present.");
    }
    selection.seen = use != aFrame.uses.end();
    selection.aliasedAway = selection.seen && use->second.aliasedAway;
    selection.writes = track.writes;
    selection.settledSinceClear = track.settledSinceClear;
    selection.everSettled = track.everSettled;
    if (resource != s_selection.resource)
    {
        ++s_counters.switches;
        const auto now = std::chrono::steady_clock::now();
        if (s_lastSwitchLog == std::chrono::steady_clock::time_point{} || now - s_lastSwitchLog >= kSwitchLogEvery)
        {
            s_lastSwitchLog = now;
            Log(LogLevel::Info,
                std::format("the scene's depth buffer is {} {}: {} draws, {} vertices, {} indirect over the last few "
                            "presents{}; cleared to {}; state at present {}{}; the best of {}.",
                            Hex(resource), DescribeResource(selection.desc, extent.width, extent.height),
                            candidate.draws, candidate.vertices, candidate.indirectDraws,
                            candidate.readByShaders ? ", also sampled by the game" : "",
                            selection.cleared ? std::format("{}", selection.clearDepth) : std::string("(not seen)"),
                            StateName(selection.state[0]),
                            selection.stateKnown ? (selection.initialKnown ? " (from its creation and changes)" : "")
                                                 : " (assumed)",
                            ranked.size()));
        }
    }
    s_selection = selection;
    s_calibration.SetConvention(selection.convention);
    if (selection.aliasedAway)
    {
        ++s_counters.aliased;
        WarnOnce(s_warnedAliased, "the game gave the depth buffer's memory to another resource after drawing with "
                                  "it, so at present it does not hold the scene: the last copy is kept.");
    }
}
} // namespace

// ---------------------------------------------------------------------------------
// The public side
// ---------------------------------------------------------------------------------

void Install(const AttachFunction aAttach, const LogFunction aLog)
{
    s_log = aLog;
    s_attach = aAttach;
    std::array<wchar_t, 8> value{};
    const DWORD length = GetEnvironmentVariableW(L"OP77_TV_DEPTH", value.data(), static_cast<DWORD>(value.size()));
    if (length == 1 && value[0] == L'0')
    {
        Log(LogLevel::Info, "off (OP77_TV_DEPTH=0): screens are drawn over everything, as before, and nothing is "
                            "hooked.");
        return;
    }
    s_enabled.store(true, std::memory_order_release);
    HMODULE runtime = GetModuleHandleW(L"d3d12.dll");
    if (runtime == nullptr)
    {
        runtime = LoadLibraryW(L"d3d12.dll");
    }
    void* const target =
        runtime != nullptr ? reinterpret_cast<void*>(GetProcAddress(runtime, "D3D12CreateDevice")) : nullptr;
    if (target != nullptr && aAttach != nullptr &&
        aAttach(target, reinterpret_cast<void*>(&DetourCreateDevice), reinterpret_cast<void**>(&s_createDevice)))
    {
        Log(LogLevel::Info, "waiting for the game's Direct3D 12 device (D3D12CreateDevice hooked).");
    }
    else
    {
        s_createDevice = nullptr;
        Log(LogLevel::Warning, "could not hook D3D12CreateDevice; the device is watched from when the overlay starts, "
                               "and depth buffers made before then are found by their state changes only.");
    }
}

void Uninstall()
{
    s_enabled.store(false, std::memory_order_release);
    const std::scoped_lock lock(s_patchMutex);
    const size_t count = s_tableCount.load(std::memory_order_acquire);
    for (size_t index = 0; index < count; ++index)
    {
        HookedTable& record = s_tables[index];
        for (size_t slot = 0; slot < Slot::kCount; ++slot)
        {
            if (record.detours[slot] != nullptr && record.table[slot] == record.detours[slot])
            {
                WriteEntry(&record.table[slot], record.originals[slot]);
            }
        }
    }
}

void OnGraphicsReady(ID3D12Device* const aDevice, const D3D12_CPU_DESCRIPTOR_HANDLE aCpuSlot,
                     const D3D12_GPU_DESCRIPTOR_HANDLE aGpuSlot, const DXGI_FORMAT aRenderTargetFormat)
{
    OnGraphicsShutdown();
    if (!s_enabled.load(std::memory_order_acquire) || aDevice == nullptr || aCpuSlot.ptr == 0 || aGpuSlot.ptr == 0)
    {
        return;
    }
    if (!IsHooked(*reinterpret_cast<void** const*>(aDevice)))
    {
        HookDevice(aDevice, s_deviceSeen.load(std::memory_order_acquire) ? "the overlay's device"
                                                                         : "when the overlay started");
    }
    s_device = aDevice;
    s_device->AddRef();
    s_viewCpu = aCpuSlot;
    s_viewGpu = aGpuSlot;
    std::string error;
    if (!s_pass.Create(aDevice, aRenderTargetFormat, error))
    {
        Log(LogLevel::Warning, "the depth-tested drawing could not be built (" + error +
                                   "); screens are drawn as before.");
        SafeRelease(s_device);
        return;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = kReadbackSlots * kSamplesPerSlot * kSampleStride;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(aDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&s_readback))))
    {
        s_readback = nullptr;
        Log(LogLevel::Warning, "no readback buffer: the depth test cannot be calibrated.");
    }
    s_graphicsReady = true;
    Log(LogLevel::Info, std::format("ready to draw screens in the game's depth ({}). Keys: Ctrl+Shift+F10 shows the "
                                    "depth, Ctrl+Shift+F11 turns the test off/on, Ctrl+Shift+F9 tries the next "
                                    "buffer.",
                                    s_deviceSeen.load() ? "the game's device is watched" : "the device is not watched"));
}

void OnGraphicsShutdown()
{
    s_graphicsReady = false;
    s_frameList = nullptr;
    s_copied = false;
    s_pass.Release();
    SafeRelease(s_copy.texture);
    s_copy = Copy{};
    s_refused = Refused{};
    ReleaseRetired(0, true);
    SafeRelease(s_readback);
    SafeRelease(s_planeReadback);
    s_planeBytes = 0;
    s_slots = {};
    s_selection = Selection{};
    s_history.Clear();
    s_descs.clear();
    s_lastSwitchLog = {};
    s_copyOf = nullptr;
    s_copyWrites = 0;
    s_copyPresent = 0;
    s_waitsSinceCopy = 0;
    s_copyExtent[0] = 0.0F;
    s_copyExtent[1] = 0.0F;
    s_copyUsable = false;
    SafeRelease(s_device);
    s_viewCpu = {};
    s_viewGpu = {};
}

void PrepareFrame(ID3D12GraphicsCommandList* const aCommands, const uint64_t aCompletedFence,
                  const float aOverlayWidth, const float aOverlayHeight)
{
    ++s_present;
    s_frameList = aCommands;
    s_copied = false;
    s_copyUsable = false;
    s_overlayWidth = std::max(1.0F, aOverlayWidth);
    s_overlayHeight = std::max(1.0F, aOverlayHeight);
    FrameRecord frame;
    {
        const std::scoped_lock lock(s_frameMutex);
        frame = std::move(s_frame);
        s_frame = FrameRecord{};
    }
    if (!s_graphicsReady || aCommands == nullptr)
    {
        return;
    }
    ++s_counters.presents;
    s_counters.unresolvedDraws += frame.unresolvedDraws;
    if (frame.unresolvedDraws > 0 && frame.uses.empty())
    {
        WarnOnce(s_warnedUnresolved, "the game draws into depth views made before they could be seen; the depth "
                                     "buffer cannot be told apart this way.");
    }
    ReleaseRetired(aCompletedFence, false);
    ProcessReadbacks(aCompletedFence);
    PollKeys();
    Choose(frame);
    LogStatus();
    const bool wanted = s_debugView || (s_lastScreenAt != 0 && s_present <= s_lastScreenAt + kCopyWhileScreensWithin);
    if (wanted && s_selection.resource != nullptr)
    {
        Policy::CopyInputs inputs;
        inputs.seenThisPresent = s_selection.seen;
        inputs.writtenSinceCopy = s_copyOf != s_selection.resource || s_copyWrites != s_selection.writes;
        inputs.aliasedAway = s_selection.aliasedAway;
        inputs.everSettled = s_selection.everSettled;
        inputs.settledSinceClear = s_selection.settledSinceClear;
        inputs.readOnlyNow = s_selection.stateKnown && IsReadOnly(s_selection.state[0]);
        inputs.waits = s_waitsSinceCopy;
        if (Policy::ShouldCopy(inputs))
        {
            if (EnsureCopy(s_selection.desc, s_selection.family) && RecordCopy(aCommands))
            {
                s_copied = true;
                ++s_counters.copies;
                s_copyOf = s_selection.resource;
                s_copyWrites = s_selection.writes;
                s_copyPresent = s_present;
                s_waitsSinceCopy = 0;
                s_copyExtent[0] = s_selection.extent[0];
                s_copyExtent[1] = s_selection.extent[1];
            }
        }
        else if (inputs.seenThisPresent && inputs.writtenSinceCopy && !inputs.aliasedAway)
        {
            ++s_counters.waits; // mid-frame: the last finished frame's copy is kept
            ++s_waitsSinceCopy;
        }
    }
    // The copy is used at every present until a newer one replaces it: with frame
    // generation most presents show an image the game did not render, and no depth
    // is drawn for them.
    s_copyUsable = s_copy.texture != nullptr && s_copyOf != nullptr &&
                   s_present - s_copyPresent <= Policy::kCopyFreshForPresents;
}

void FrameSubmitted(const uint64_t aFenceValue)
{
    s_frameList = nullptr; // closed and submitted: nothing more is recorded into it
    if (aFenceValue == 0)
    {
        return;
    }
    s_lastFence = aFenceValue;
    if (s_copied || s_copyUsable)
    {
        s_copy.lastFence = aFenceValue; // written or read by this frame
    }
    for (auto& slot : s_slots)
    {
        if (slot.filling && slot.present == s_present)
        {
            slot.filling = false;
            slot.pending = slot.count > 0;
            slot.fence = aFenceValue;
            if (!slot.pending)
            {
                slot = ReadbackSlot{};
            }
        }
    }
}

bool DrawScreen(ImDrawList& aDraw, const D3D12_GPU_DESCRIPTOR_HANDLE aPicture, const float (&aCorners)[8],
                const float aCentreDepth, const float aOpacity)
{
    if (!s_graphicsReady || !s_pass.Ready() || aPicture.ptr == 0 || !(aCentreDepth > 0.0F))
    {
        return false;
    }
    // Remembered before anything can refuse, so the next present copies the depth.
    s_lastScreenAt = s_present + 1;
    // Only while there is a recent copy of the game's depth: otherwise this is the
    // old drawing with more steps, and the old drawing is the one known to work.
    if (!s_copyUsable)
    {
        return false;
    }
    // Refused here exactly as the callback would refuse it, so a screen the pass
    // cannot draw is drawn the old way instead of not at all.
    const float size[2]{s_overlayWidth, s_overlayHeight};
    SD::Constants probe;
    if (SD::Build(aCorners, aCentreDepth, SD::DepthModel{}, size, size, size, SD::Tuning{}, false, probe) !=
        SD::Status::Ok)
    {
        return false;
    }
    ScreenDraw draw;
    std::copy(std::begin(aCorners), std::end(aCorners), std::begin(draw.corners));
    draw.centreDepth = aCentreDepth;
    draw.opacity = std::clamp(aOpacity, 0.0F, 1.0F);
    draw.picture = aPicture.ptr;
    aDraw.AddCallback(&ScreenCallback, &draw, sizeof(draw));
    aDraw.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return true;
}

bool DrawClippedScreen(ImDrawList& aDraw, const D3D12_GPU_DESCRIPTOR_HANDLE aPicture, const Clip::Map& aMap,
                       const float aOpacity)
{
    if (!s_graphicsReady || !s_pass.Ready() || aPicture.ptr == 0 || !aMap.Finite())
    {
        return false;
    }
    // Remembered before anything can refuse, so the next present copies the depth.
    s_lastScreenAt = s_present + 1;
    if (!s_copyUsable)
    {
        return false;
    }
    // The part in front of the near plane: three to five corners, which the pass
    // draws as a fan. The pixel shader works every pixel's texture coordinate and
    // depth out from the map, so the corners only say which pixels to shade.
    const Clip::Polygon region = Clip::VisibleRegion(aMap);
    if (region.count < 3 || region.count > Clip::kMaximumCorners)
    {
        return false;
    }
    ScreenDraw draw;
    for (int i = 0; i < region.count; ++i)
    {
        const auto& corner = region.at[static_cast<size_t>(i)];
        const std::array<double, 2> at = aMap.Apply(corner[0], corner[1]);
        // Within a million pixels, as `ScreenTessellation::Solve` holds corners: the
        // near plane is close enough to the eye that a corner of a large screen on it
        // can land a long way off the view, but not that far.
        if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || std::abs(at[0]) > Tess::kMaximumCoordinate ||
            std::abs(at[1]) > Tess::kMaximumCoordinate)
        {
            return false;
        }
        draw.polygon[i * 2] = static_cast<float>(at[0]);
        draw.polygon[(i * 2) + 1] = static_cast<float>(at[1]);
    }
    // Refused here exactly as the callback would refuse it, so a screen the pass
    // cannot draw is drawn the other way instead of not at all.
    const float size[2]{s_overlayWidth, s_overlayHeight};
    SD::Constants probe;
    if (SD::BuildFromMap(aMap, SD::DepthModel{}, size, size, size, SD::Tuning{}, false, probe) != SD::Status::Ok)
    {
        return false;
    }
    draw.clipped = true;
    draw.count = region.count;
    std::memcpy(draw.map, aMap.m, sizeof(draw.map));
    draw.centreDepth = 1.0F;
    draw.opacity = std::clamp(aOpacity, 0.0F, 1.0F);
    draw.picture = aPicture.ptr;
    aDraw.AddCallback(&ScreenCallback, &draw, sizeof(draw));
    aDraw.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return true;
}

void DrawOverlay(ImDrawList& aForeground, const float aOverlayWidth, const float aOverlayHeight)
{
    if (!s_debugView || !s_graphicsReady || !s_pass.Ready())
    {
        return;
    }
    const float margin = 16.0F;
    std::string text = "TV depth (Ctrl+Shift: F10 view, F11 test, F9 next buffer)\n";
    if (s_copyUsable)
    {
        const float width = std::max(64.0F, aOverlayWidth * 0.3F);
        const float height = width * (s_copyExtent[1] / std::max(1.0F, s_copyExtent[0]));
        ScreenDraw draw;
        const float left = margin;
        const float top = margin;
        const float corners[8] = {left, top, left + width, top, left + width, top + height, left, top + height};
        std::copy(std::begin(corners), std::end(corners), std::begin(draw.corners));
        draw.centreDepth = 1.0F;
        draw.opacity = 1.0F;
        draw.picture = s_viewGpu.ptr;
        draw.debug = true;
        aForeground.AddRectFilled(ImVec2(left - 2.0F, top - 2.0F), ImVec2(left + width + 2.0F, top + height + 2.0F),
                                  IM_COL32(0, 0, 0, 200));
        aForeground.AddCallback(&ScreenCallback, &draw, sizeof(draw));
        aForeground.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        text += std::format("{} {}  {}  test {}\n",
                            DescribeResource(s_selection.desc, s_selection.extent[0], s_selection.extent[1]),
                            Policy::Describe(s_selection.convention),
                            s_calibration.Locked()
                                ? std::format("B {:.4f} m", s_calibration.B())
                                : std::format("measuring B ({} samples)", s_calibration.Groups()),
                            s_testOn ? "on" : "OFF");
        text += s_candidatesText;
        aForeground.AddText(ImVec2(left, top + height + 6.0F), IM_COL32(255, 255, 255, 255), text.c_str());
    }
    else
    {
        text += "no depth buffer found yet";
        aForeground.AddText(ImVec2(margin, margin), IM_COL32(255, 220, 120, 255), text.c_str());
    }
    static_cast<void>(aOverlayHeight);
}

std::string Describe()
{
    if (!s_enabled.load(std::memory_order_acquire))
    {
        return "off";
    }
    return std::format("device {}, buffer {}, model {}, test {}", s_deviceSeen.load() ? "watched" : "not seen",
                       s_selection.resource != nullptr
                           ? DescribeResource(s_selection.desc, s_selection.extent[0], s_selection.extent[1])
                           : std::string("none"),
                       s_calibration.Locked() ? std::format("B {:.5f}", s_calibration.B()) : std::string("measuring"),
                       s_testOn ? "on" : "off");
}
} // namespace op77::WebUI::SceneDepth
