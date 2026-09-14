#include "api/MediaScreens.hpp"

#include "api/Camera.hpp"
#include "api/ScreenQuad.hpp"
#include "api/Props.hpp"
#include "api/WorldQuery.hpp"
#include "engine/RttiAccess.hpp"
#include "webui/WorldOverlay.hpp"
#include "world/EntityService.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RED4ext/Scripting/Functions.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/Object.hpp>

namespace op77::Api::MediaScreens
{
namespace
{
constexpr size_t kPerOwnerLimit = 16;
constexpr size_t kGlobalLimit = 64;

/// Metres of slack when deciding whether a trace hit is *in front of* the screen
/// rather than the screen's own surface. A ray aimed at the quad's centre stops
/// on the panel it is aimed at when that panel has collision, and a panel
/// mounted flush against a wall stops a few centimetres before its own centre.
/// Without the slack a screen would hide itself.
constexpr float kOcclusionSlack = 0.35F;

/// Where the screen stops being drawn, and how much of that range fades out.
/// Generous on purpose: a billboard on a rooftop is the whole point of a big
/// screen, and `WorldOverlay` already fades the far end of whatever band it is
/// given.
constexpr float kMaximumDistance = 150.0F;

/// One producer name per owner, so a screen set and anything else a resource
/// publishes cannot replace one another. Mirrors `WorldAnchors`, which namespaces
/// per owner for exactly this reason.
constexpr std::string_view kProducerPrefix = "open77_media:";

struct Entry
{
    uint64_t id{};
    std::string owner;
    Definition definition;
    bool drawn{};
    std::string reason{"not yet published"};
    float distance{};
    /// Which way round this screen's picture goes, as last decided from the
    /// projection. Held per screen, and passed back in on the next frame, because
    /// near edge-on the two candidate edges project to nearly the same place and
    /// the comparison is noise -- see `kOrientationDeadband`. Without the memory
    /// the picture would flip back and forth while a player walked past a set.
    ScreenQuad::Orientation orientation{};
    /// The facing test's last outcome: whether the record declared a front at
    /// all, and whether the eye is behind it. Reported through `SnapshotAll` so
    /// `media.list` can answer "which side is that set's picture on" and "why is
    /// that screen blank" without a screenshot.
    bool facingKnown{};
    bool facingAway{};
};

RED4ext::v1::PluginHandle s_pluginHandle;
const RED4ext::v1::Sdk* s_sdk = nullptr;
/// Recursive because the publish path takes it and then calls into
/// `WorldOverlay`, which has its own lock; nothing here calls back into this
/// module, but `Props::ProjectedEntity` is called while holding it and Props
/// takes its own, so a non-recursive lock would be a deadlock waiting for the
/// first caller that holds this one across a Props call. It is the same choice
/// `Props` itself made.
std::recursive_mutex s_mutex;
std::unordered_map<uint64_t, Entry> s_entries;
uint64_t s_nextId = 1;
bool s_running = false;

void LogInfo(const char* const aFormat, auto... aArguments)
{
    if (s_sdk != nullptr && s_sdk->logger != nullptr)
        s_sdk->logger->InfoF(s_pluginHandle, aFormat, aArguments...);
}

void LogWarn(const char* const aFormat, auto... aArguments)
{
    if (s_sdk != nullptr && s_sdk->logger != nullptr)
        s_sdk->logger->WarnF(s_pluginHandle, aFormat, aArguments...);
}

RED4ext::Vector4 Add(const RED4ext::Vector4& aLeft, const RED4ext::Vector4& aRight)
{
    return {aLeft.X + aRight.X, aLeft.Y + aRight.Y, aLeft.Z + aRight.Z, 0.0F};
}

RED4ext::Vector4 Sub(const RED4ext::Vector4& aLeft, const RED4ext::Vector4& aRight)
{
    return {aLeft.X - aRight.X, aLeft.Y - aRight.Y, aLeft.Z - aRight.Z, 0.0F};
}

RED4ext::Vector4 Scale(const RED4ext::Vector4& aValue, const float aFactor)
{
    return {aValue.X * aFactor, aValue.Y * aFactor, aValue.Z * aFactor, 0.0F};
}

float Dot(const RED4ext::Vector4& aLeft, const RED4ext::Vector4& aRight)
{
    return (aLeft.X * aRight.X) + (aLeft.Y * aRight.Y) + (aLeft.Z * aRight.Z);
}

bool Finite(const RED4ext::Vector4& aValue)
{
    return std::isfinite(aValue.X) && std::isfinite(aValue.Y) && std::isfinite(aValue.Z);
}

/// Reads the entity's own world placement. Both natives are resolved once per
/// call rather than cached: this module runs on the game thread inside the tick,
/// where a stale function pointer across a hot reload is worse than a lookup.
bool ReadPlacement(
    const RED4ext::Handle<RED4ext::game::Object>& aObject,
    RED4ext::Vector4& aPosition,
    RED4ext::Quaternion& aOrientation)
{
    auto* const getPosition = Engine::Rtti::EntityNative("GetWorldPosition");
    auto* const getOrientation = Engine::Rtti::EntityNative("GetWorldOrientation");
    if (getPosition == nullptr || getOrientation == nullptr)
    {
        return false;
    }
    if (!RED4ext::ExecuteFunction(aObject.GetPtr(), getPosition, &aPosition))
    {
        return false;
    }
    if (!RED4ext::ExecuteFunction(aObject.GetPtr(), getOrientation, &aOrientation))
    {
        return false;
    }
    return Finite(aPosition);
}

/// The four world-space corners of a screen, in `ScreenQuad::Corners`' own tagged
/// order -- two of them off `+right`, two off `-right`, two off `+up`, two off
/// `-up` -- and NOT yet in texture order.
///
/// Texture order is decided after the projection, from where those corners landed
/// on screen (`ScreenQuad::Orient`), because that is the only description of
/// "which way is the picture's left" that cannot be a half-turn out. This
/// function got that wrong once by deriving the orientation from the prop mesh's
/// own UVs, which encode the game's material texture and not this one.
///
/// The geometry itself lives in `api/ScreenQuad.hpp`, pure and tested, because a
/// screen whose picture is a half-turn out cannot fail loudly: it is the right
/// place, the right size, on the right prop, drawing every frame, and unreadable.
/// The wire rectangle as the pure mapping in `api/ScreenQuad.hpp` sees it.
///
/// The two structs are intentionally separate -- one is a scripting-layer shape
/// and one is engine-free arithmetic -- so the copy lives in one function that
/// both the corner builder and the facing test read, rather than in two places
/// that could drift.
ScreenQuad::Quad MappingFor(const Definition& aDefinition)
{
    const auto& quad = aDefinition.quad;
    ScreenQuad::Quad mapping{};
    mapping.offset[0] = quad.offset[0];
    mapping.offset[1] = quad.offset[1];
    mapping.offset[2] = quad.offset[2];
    mapping.right[0] = quad.right[0];
    mapping.right[1] = quad.right[1];
    mapping.right[2] = quad.right[2];
    mapping.up[0] = quad.up[0];
    mapping.up[1] = quad.up[1];
    mapping.up[2] = quad.up[2];
    mapping.width = quad.width;
    mapping.height = quad.height;
    mapping.faces[0] = quad.faces[0];
    mapping.faces[1] = quad.faces[1];
    mapping.faces[2] = quad.faces[2];
    return mapping;
}

bool ScreenCorners(
    const Entry& aEntry,
    const RED4ext::Vector4& aWorldPosition,
    const RED4ext::Quaternion& aOrientation,
    std::vector<RED4ext::Vector4>& aOut)
{
    const ScreenQuad::Quad mapping = MappingFor(aEntry.definition);

    const ScreenQuad::Vec3 position{aWorldPosition.X, aWorldPosition.Y, aWorldPosition.Z};
    const ScreenQuad::Rotation rotation{aOrientation.i, aOrientation.j, aOrientation.k,
                                        aOrientation.r};

    ScreenQuad::Vec3 corners[4]{};
    if (!ScreenQuad::Corners(mapping, position, rotation, corners))
    {
        return false;
    }

    aOut.clear();
    aOut.reserve(4);
    for (const auto& corner : corners)
    {
        aOut.push_back(RED4ext::Vector4{corner.x, corner.y, corner.z, 1.0F});
    }
    return true;
}

/// Whether anything solid stands between the eye and the screen's centre.
///
/// The trace is shortened by `kOcclusionSlack` so the panel's own collision
/// cannot hide it. `Raycast` reports `aHit = false` for a clear segment, which
/// is the answer wanted; a refusal from the query system is treated as *not*
/// occluded, because a screen that disappears when the physics query is
/// unavailable is a worse failure than one that draws through a wall while the
/// query is unavailable.
bool Occluded(const RED4ext::Vector4& aEye, const RED4ext::Vector4& aTarget,
              const float aDistance)
{
    const auto delta = Sub(aTarget, aEye);
    if (aDistance <= kOcclusionSlack)
    {
        return false;
    }
    const auto direction = Scale(delta, (aDistance - kOcclusionSlack) / aDistance);
    const auto end = Add(aEye, direction);
    WorldQuery::RaycastHit hit{};
    bool blocked = false;
    const WorldQuery::RaycastOptions options{};
    if (WorldQuery::Raycast(aEye, end, options, hit, blocked) != WorldQuery::Status::Ok)
    {
        return false;
    }
    return blocked;
}

/// Builds the overlay item for one screen, or explains why there is none.
bool BuildItem(
    Entry& aEntry,
    const Camera::View& aView,
    WorldOverlay::Item& aOut,
    std::string& aReason)
{
    // Cleared here rather than left over: a screen that stops being built at all
    // (its prop unstreamed, its quad refused) must not keep reporting yesterday's
    // answer about which side the eye was on.
    aEntry.facingKnown = false;
    aEntry.facingAway = false;

    // Locate the prop. `ProjectedEntity` is the props registry's own mapping
    // from the server's id to this client's entity id, which is the whole reason
    // a screen can be bound before its prop has streamed in: the binding is a
    // number, and the lookup simply fails until it can succeed.
    const uint64_t entity = Props::ProjectedEntity(aEntry.definition.prop);
    if (entity == 0)
    {
        aReason = "prop_not_projected";
        return false;
    }
    auto object = EntityService::Lock(Core::EntityId{entity});
    if (!object)
    {
        aReason = "prop_not_streamed";
        return false;
    }

    RED4ext::Vector4 position{};
    RED4ext::Quaternion orientation{};
    if (!ReadPlacement(object, position, orientation))
    {
        aReason = "placement_unavailable";
        return false;
    }

    std::vector<RED4ext::Vector4> world;
    if (!ScreenCorners(aEntry, position, orientation, world))
    {
        aReason = "quad_invalid";
        return false;
    }

    const auto centre = RED4ext::Vector4{
        (world[0].X + world[1].X + world[2].X + world[3].X) * 0.25F,
        (world[0].Y + world[1].Y + world[2].Y + world[3].Y) * 0.25F,
        (world[0].Z + world[1].Z + world[2].Z + world[3].Z) * 0.25F,
        0.0F};

    // Which side of the panel the eye is on, when the record says.
    //
    // A television seen from behind used to carry its video on the back of the
    // cabinet: the projection says where the rectangle is and nothing at all
    // about which of its two sides is the picture, and the orientation rule that
    // keeps the picture upright deliberately works from either side. Only the
    // asset knows, so the record declares it (`Quad::faces`) and this is the one
    // dot product that reads it. A record that declares nothing keeps the old
    // behaviour -- drawn from both sides -- because blanking a screen on an
    // undeclared front would turn a missing catalogue number into a missing
    // television.
    const ScreenQuad::Facing facing = ScreenQuad::Faces(
        MappingFor(aEntry.definition),
        ScreenQuad::Vec3{position.X, position.Y, position.Z},
        ScreenQuad::Rotation{orientation.i, orientation.j, orientation.k, orientation.r},
        ScreenQuad::Vec3{aView.position.X, aView.position.Y, aView.position.Z});
    aEntry.facingKnown = facing.known;
    aEntry.facingAway = facing.away;
    if (facing.known && facing.away)
    {
        aReason = "behind_panel";
        return false;
    }

    const auto relative = Sub(centre, aView.position);
    const float depth = Dot(relative, aView.forward);
    // Behind the camera, or close enough to the plane that the perspective
    // divide whips the quad across the frame. Tested before the projection
    // because it is one dot product against four matrix transforms.
    if (depth <= 0.05F)
    {
        aReason = "behind_camera";
        return false;
    }
    const float distance = std::sqrt(std::max(0.0F, Dot(relative, relative)));
    if (distance > kMaximumDistance)
    {
        aReason = "out_of_range";
        return false;
    }
    if (Occluded(aView.position, centre, distance))
    {
        aReason = "occluded";
        return false;
    }

    std::array<RED4ext::Vector4, 4> projected{};
    if (Camera::ProjectPoints(
            std::span<const RED4ext::Vector4>(world.data(), world.size()),
            std::span<RED4ext::Vector4>(projected.data(), projected.size())) != Camera::Status::Ok)
    {
        aReason = "projection_unavailable";
        return false;
    }

    aOut = WorldOverlay::Item{};
    aOut.style = WorldOverlay::Style::Screen;
    aOut.surface = aEntry.definition.surface;
    aOut.depth = depth;
    aOut.distance = distance;
    aOut.maximumDistance = kMaximumDistance;
    // The engine's projection is NDC; the overlay works in 0..1 from the
    // top-left, the same conversion every other producer applies. Collected in
    // the corners' own tagged order first, because the orientation below is a
    // question about positions and not about which one is the texture's origin.
    float screen[8]{};
    for (size_t index = 0; index < 4; ++index)
    {
        if (!std::isfinite(projected[index].X) || !std::isfinite(projected[index].Y))
        {
            aReason = "projection_not_finite";
            return false;
        }
        screen[index * 2] = (projected[index].X + 1.0F) * 0.5F;
        screen[index * 2 + 1] = (1.0F - projected[index].Y) * 0.5F;
    }
    // Which way round the picture goes, from where the panel landed. Read, not
    // assumed: this is the whole reason the panel's own axes are no longer used
    // to decide it, and the previous decision is carried in so a screen that is
    // nearly edge-on keeps the answer it had instead of flapping.
    aEntry.orientation = ScreenQuad::Orient(screen, aEntry.orientation);
    // Now in texture order -- top-left, top-right, bottom-right, bottom-left --
    // which is the order `WebUiService::DrawSurfaceQuad` reads as (0,0), (1,0),
    // (1,1), (0,1).
    aOut.corners.resize(8);
    constexpr std::array<std::array<int, 2>, 4> kTextureOrder{{{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}}};
    for (size_t slot = 0; slot < kTextureOrder.size(); ++slot)
    {
        const int source =
            ScreenQuad::CornerForTexture(aEntry.orientation, kTextureOrder[slot][0],
                                         kTextureOrder[slot][1]);
        aOut.corners[slot * 2] = screen[source * 2];
        aOut.corners[slot * 2 + 1] = screen[source * 2 + 1];
    }
    // The centre, in the same space, for the anchor-based paths that are not
    // taken for a screen but are read by the debug bridge.
    aOut.x = (aOut.corners[0] + aOut.corners[2] + aOut.corners[4] + aOut.corners[6]) * 0.25F;
    aOut.y = (aOut.corners[1] + aOut.corners[3] + aOut.corners[5] + aOut.corners[7]) * 0.25F;
    aEntry.distance = distance;
    aReason = "drawn";
    return true;
}

std::string ProducerFor(const std::string& aOwner)
{
    return std::string(kProducerPrefix) + aOwner;
}

void RetireProducer(const std::string& aOwner)
{
    if (!aOwner.empty())
    {
        WorldOverlay::Retire(ProducerFor(aOwner));
    }
}
} // namespace

const char* Describe(const Status aStatus)
{
    switch (aStatus)
    {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid_argument";
    case Status::NotFound: return "not_found";
    case Status::OwnedByAnotherResource: return "owned_by_another_resource";
    case Status::QuotaExceeded: return "quota_exceeded";
    case Status::PropNotProjected: return "prop_not_projected";
    }
    return "unknown";
}

Status Bind(const std::string_view aOwner, const Definition& aDefinition, uint64_t& aId)
{
    aId = 0;
    if (aOwner.empty() || aOwner.size() > 128 || aDefinition.label.size() > 128)
    {
        return Status::InvalidArgument;
    }
    // Zero is legal and means "a screen with no content yet" -- a television
    // that has been switched off rather than one that has been removed.
    if (aDefinition.prop == 0 && aDefinition.surface == 0)
    {
        return Status::InvalidArgument;
    }

    const std::scoped_lock lock(s_mutex);
    if (s_entries.size() >= kGlobalLimit)
    {
        LogWarn("MediaScreens::Bind refused '%s': global limit of %zu screens reached.",
                std::string(aOwner).c_str(), kGlobalLimit);
        return Status::QuotaExceeded;
    }
    const auto owned = std::ranges::count_if(
        s_entries, [aOwner](const auto& item) { return item.second.owner == aOwner; });
    if (static_cast<size_t>(owned) >= kPerOwnerLimit)
    {
        LogWarn("MediaScreens::Bind refused '%s': per-owner limit of %zu screens reached.",
                std::string(aOwner).c_str(), kPerOwnerLimit);
        return Status::QuotaExceeded;
    }

    Entry entry;
    entry.id = s_nextId++;
    entry.owner = std::string(aOwner);
    entry.definition = aDefinition;
    aId = entry.id;
    s_entries.emplace(entry.id, std::move(entry));
    LogInfo("MediaScreens: bound screen %llu of '%s' to prop=%llu surface=%llu label='%s'.",
            static_cast<unsigned long long>(aId), std::string(aOwner).c_str(),
            static_cast<unsigned long long>(aDefinition.prop),
            static_cast<unsigned long long>(aDefinition.surface),
            aDefinition.label.c_str());
    return Status::Ok;
}

Status Update(const std::string_view aOwner, const uint64_t aId, const Definition& aDefinition)
{
    if (aOwner.empty() || aId == 0)
    {
        return Status::InvalidArgument;
    }
    if (aDefinition.label.size() > 128)
    {
        return Status::InvalidArgument;
    }
    const std::scoped_lock lock(s_mutex);
    const auto found = s_entries.find(aId);
    if (found == s_entries.end())
    {
        return Status::NotFound;
    }
    if (found->second.owner != aOwner)
    {
        return Status::OwnedByAnotherResource;
    }
    found->second.definition = aDefinition;
    // The next tick republishes anyway, so nothing is marked dirty here; the
    // geometry is read fresh from the definition every frame precisely so a
    // change needs no notification.
    return Status::Ok;
}

Status Unbind(const std::string_view aOwner, const uint64_t aId)
{
    if (aOwner.empty() || aId == 0)
    {
        return Status::InvalidArgument;
    }
    const std::scoped_lock lock(s_mutex);
    const auto found = s_entries.find(aId);
    if (found == s_entries.end())
    {
        return Status::NotFound;
    }
    if (found->second.owner != aOwner)
    {
        return Status::OwnedByAnotherResource;
    }
    s_entries.erase(found);
    // Publish nothing rather than waiting for the next tick: an unbind that
    // leaves a picture on the wall until the tick loop runs is a picture that
    // outlives the screen that owned it.
    RetireProducer(std::string(aOwner));
    return Status::Ok;
}

void Release(const std::string_view aOwner)
{
    if (aOwner.empty())
    {
        return;
    }
    const std::scoped_lock lock(s_mutex);
    std::erase_if(s_entries,
                  [aOwner](const auto& item) { return item.second.owner == aOwner; });
    RetireProducer(std::string(aOwner));
}

void ReleaseAll()
{
    const std::scoped_lock lock(s_mutex);
    for (const auto& [_, entry] : s_entries)
    {
        RetireProducer(entry.owner);
    }
    s_entries.clear();
}

std::vector<Snapshot> SnapshotAll(const std::string_view aOwner)
{
    const std::scoped_lock lock(s_mutex);
    std::vector<Snapshot> result;
    result.reserve(s_entries.size());
    for (const auto& [id, entry] : s_entries)
    {
        if (!aOwner.empty() && entry.owner != aOwner)
        {
            continue;
        }
        Snapshot snapshot;
        snapshot.id = id;
        snapshot.owner = entry.owner;
        snapshot.definition = entry.definition;
        snapshot.drawn = entry.drawn;
        snapshot.reason = entry.reason;
        snapshot.distance = entry.distance;
        snapshot.facingKnown = entry.facingKnown;
        snapshot.facingAway = entry.facingAway;
        result.push_back(std::move(snapshot));
    }
    std::ranges::sort(result, [](const Snapshot& left, const Snapshot& right) {
        return left.id < right.id;
    });
    return result;
}

size_t Count()
{
    const std::scoped_lock lock(s_mutex);
    return s_entries.size();
}

size_t PerOwnerLimit() { return kPerOwnerLimit; }
size_t GlobalLimit() { return kGlobalLimit; }

void Initialize(const RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* const aSdk)
{
    const std::scoped_lock lock(s_mutex);
    s_pluginHandle = aHandle;
    s_sdk = aSdk;
}

void OnRunningEnter()
{
    const std::scoped_lock lock(s_mutex);
    s_running = true;
}

void OnRunningUpdate()
{
    Camera::View view{};
    // One view read for the whole tick, not one per screen: every screen is
    // projected against the same camera, and reading it per screen would put N
    // extra RTTI calls per frame on the thread that must not stall.
    const bool haveView = Camera::Describe(view) == Camera::Status::Ok;

    // Every owner with a live screen gets a frame this tick, even when that
    // frame is empty. Publishing nothing at all for an owner that had a screen a
    // moment ago would leave its last picture on the wall until the overlay's
    // staleness guard blanked it -- and would log "producer stopped publishing"
    // about a producer that simply had nothing to say, which is a false alarm
    // that costs the next person an evening.
    std::unordered_map<std::string, std::vector<WorldOverlay::Item>> frames;
    std::unordered_set<std::string> owners;
    {
        const std::scoped_lock lock(s_mutex);
        if (s_entries.empty() || !s_running)
        {
            return;
        }
        for (auto& [_, entry] : s_entries)
        {
            owners.insert(entry.owner);
            if (!haveView)
            {
                entry.drawn = false;
                entry.reason = "camera_unavailable";
                continue;
            }
            if (entry.definition.surface == 0)
            {
                // A screen with no surface is a deliberate state, not a fault:
                // the binding is kept so the television can be switched back on
                // without re-binding, and nothing is published while it is off.
                entry.drawn = false;
                entry.reason = "no_surface";
                continue;
            }
            WorldOverlay::Item item;
            std::string reason;
            if (BuildItem(entry, view, item, reason))
            {
                entry.drawn = true;
                entry.reason = reason;
                frames[ProducerFor(entry.owner)].push_back(std::move(item));
            }
            else
            {
                entry.drawn = false;
                entry.reason = reason;
            }
        }
    }
    // Published outside the entry lock: a publish takes `WorldOverlay`'s mutex,
    // and holding both would order two locks that have no reason to be ordered.
    for (const auto& owner : owners)
    {
        const auto producer = ProducerFor(owner);
        auto found = frames.find(producer);
        WorldOverlay::Publish(
            producer, found == frames.end() ? std::vector<WorldOverlay::Item>{}
                                            : std::move(found->second));
    }
}

void OnRunningExit()
{
    const std::scoped_lock lock(s_mutex);
    s_running = false;
    for (const auto& [_, entry] : s_entries)
    {
        RetireProducer(entry.owner);
    }
    s_entries.clear();
}
} // namespace op77::Api::MediaScreens
