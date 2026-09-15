#include "api/MediaScreens.hpp"

#include "api/Camera.hpp"
#include "api/ScreenQuad.hpp"
#include "api/Props.hpp"
#include "api/WorldQuery.hpp"
#include "engine/RttiAccess.hpp"
#include "webui/ScreenInput.hpp"
#include "webui/WorldOverlay.hpp"
#include "world/EntityService.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <mutex>
#include <optional>
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

    // Where the pointer is DRAWN, when this is the screen the player has taken
    // over.
    //
    // CEF rasterises a page into a texture; the HOST paints the cursor. On a
    // world quad there is no window to paint into, so the position is published
    // for the overlay's cursor instead of being drawn as a second marker -- and
    // it is computed from the SAME four world corners the page is composited
    // with, interpolated in texture space (the orientation is the one `Orient`
    // just decided, so a mirrored or turned-over panel is not special-cased),
    // which is the only way the one pointer a player sees and the picture it
    // points at cannot disagree about where the click will land.
    const WebUiService::ScreenInput::State input = WebUiService::ScreenInput::Get();
    if (input.active && input.surface == aEntry.definition.surface)
    {
        const auto at = [&](const int aU, const int aV) -> const RED4ext::Vector4& {
            return world[ScreenQuad::CornerForTexture(aEntry.orientation, aU, aV)];
        };
        const auto& topLeft = at(0, 0);
        const auto& topRight = at(1, 0);
        const auto& bottomRight = at(1, 1);
        const auto& bottomLeft = at(0, 1);
        const float u = std::clamp(input.u, 0.0F, 1.0F);
        const float v = std::clamp(input.v, 0.0F, 1.0F);
        const auto lerp = [](const RED4ext::Vector4& aA, const RED4ext::Vector4& aB,
                             const float aT) {
            return RED4ext::Vector4{aA.X + (aB.X - aA.X) * aT, aA.Y + (aB.Y - aA.Y) * aT,
                                    aA.Z + (aB.Z - aA.Z) * aT, 0.0F};
        };
        const auto top = lerp(topLeft, topRight, u);
        const auto bottom = lerp(bottomLeft, bottomRight, u);
        const auto point = lerp(top, bottom, v);

        RED4ext::Vector4 projectedCursor{};
        if (Camera::ProjectPoint(point, projectedCursor) == Camera::Status::Ok &&
            std::isfinite(projectedCursor.X) && std::isfinite(projectedCursor.Y))
        {
            const auto relativeCursor = Sub(point, aView.position);
            const float cursorDepth = Dot(relativeCursor, aView.forward);
            if (cursorDepth > 0.05F)
            {
                WebUiService::ScreenInput::PublishPointerOnScreen(
                    (projectedCursor.X + 1.0F) * 0.5F, (1.0F - projectedCursor.Y) * 0.5F);
            }
        }
    }
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

namespace
{
/// Ends the pointer/keyboard session when the screen it was taken for is going
/// away.
///
/// A session is a hold on the focus, and the focus is what suppresses the game's
/// own input and clips the system cursor. Left behind by a screen that was just
/// despawned or released, it is a player who cannot look around, with no
/// television on screen to explain why -- so the session ends with the screen
/// that owns it. Handed the surfaces that were released rather than searching
/// the entries, because those are already gone by the time this runs, and called
/// never under `s_mutex`: `ScreenInput::Get` reads the focus mutex, and taking two
/// locks to answer one question is how lock-order bugs start.
void EndSessionFor(const std::vector<uint64_t>& aSurfaces)
{
    if (aSurfaces.empty()) return;
    const WebUiService::ScreenInput::State session = WebUiService::ScreenInput::Get();
    if (!session.active) return;
    bool owned = false;
    for (const uint64_t surface : aSurfaces)
    {
        if (surface == session.surface)
        {
            owned = true;
            break;
        }
    }
    if (!owned) return;
    WebUiService::ScreenInput::Release();
    LogInfo("screen control released: the screen it was taken for was unbound");
}
} // namespace

void Release(const std::string_view aOwner)
{
    if (aOwner.empty())
    {
        return;
    }
    std::vector<uint64_t> released;
    {
        const std::scoped_lock lock(s_mutex);
        for (const auto& [_, entry] : s_entries)
        {
            if (entry.owner == aOwner) released.push_back(entry.definition.surface);
        }
        std::erase_if(s_entries,
                      [aOwner](const auto& item) { return item.second.owner == aOwner; });
    }
    RetireProducer(std::string(aOwner));
    EndSessionFor(released);
}

void ReleaseAll()
{
    std::vector<uint64_t> released;
    {
        const std::scoped_lock lock(s_mutex);
        for (const auto& [_, entry] : s_entries)
        {
            released.push_back(entry.definition.surface);
            RetireProducer(entry.owner);
        }
        s_entries.clear();
    }
    EndSessionFor(released);
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

namespace
{
/// The key that takes a screen, and gives it back.
///
/// **F8, and not the F7 this first used.** `ClientResourceHost` binds F7 to the
/// client's own perspective toggle (`kPerspectiveKeyVirtualKey`, which exists
/// precisely because `open77_perspective` already claims F6). The two polls are
/// on different gates -- the perspective one is gated on the web focus, this one
/// is what TAKES that focus -- so the take press would fall through both: the
/// camera would flip a frame before the screen took the keyboard. A key that
/// does two things by construction is a key the player reports as broken, and
/// the fix is the key, not a gate ordering that happens to work.
///
/// F8 is unbound in the base game's control map and unbound by this plugin;
/// F1/F7/F10 are taken, and F6 belongs to the perspective resource. It is read
/// here rather than routed through the window hook for a specific reason -- a
/// GAME-THREAD tick is the only place that can answer "which panel is under the
/// crosshair" and "is a session already open" without marshalling state across
/// threads, and every other consumer of a key in this plugin reads it exactly
/// this way (`Api::VehicleFlight::ReadInput`).
constexpr int kControlKey = VK_F8;
bool s_controlKeyHeld{};

/// Nearest screen the ray crosses, and the point on it.
///
/// The plane comes from the quad's own four corners, in `ScreenQuad`'s tagged
/// order, so this agrees with the draw path by construction: right is the mean of
/// the two `+right` edges, up is the mean of the two `+up` edges, and the hit is
/// expressed in that frame. A ray that crosses the *bounding* plane outside the
/// rectangle is not a hit -- a screen is taken by looking at it, not at the wall
/// it hangs on.

/// The world point on a screen's plane, hit by a ray, as a fraction of the
/// panel, or `std::nullopt` when the ray misses it.
struct PlaneHit
{
    float u{};
    float v{};
    float distance{};
};

std::optional<PlaneHit> Intersect(
    const std::vector<RED4ext::Vector4>& aWorld,
    const float aWidth,
    const float aHeight,
    const RED4ext::Vector4& aEye,
    const RED4ext::Vector4& aDirection)
{
    if (aWorld.size() < 4 || aWidth <= 0.0F || aHeight <= 0.0F) return std::nullopt;
    const auto& pp = aWorld[ScreenQuad::CornerIndex(1, 1)];
    const auto& np = aWorld[ScreenQuad::CornerIndex(-1, 1)];
    const auto& nn = aWorld[ScreenQuad::CornerIndex(-1, -1)];
    const auto& pn = aWorld[ScreenQuad::CornerIndex(1, -1)];

    const auto centre = RED4ext::Vector4{
        (pp.X + np.X + nn.X + pn.X) * 0.25F,
        (pp.Y + np.Y + nn.Y + pn.Y) * 0.25F,
        (pp.Z + np.Z + nn.Z + pn.Z) * 0.25F,
        0.0F};

    ScreenQuad::Vec3 rightAxis{(pp.X - np.X + (pn.X - nn.X)) * 0.5F,
                              (pp.Y - np.Y + (pn.Y - nn.Y)) * 0.5F,
                              (pp.Z - np.Z + (pn.Z - nn.Z)) * 0.5F};
    ScreenQuad::Vec3 upAxis{(pp.X - pn.X + (np.X - nn.X)) * 0.5F,
                            (pp.Y - pn.Y + (np.Y - nn.Y)) * 0.5F,
                            (pp.Z - pn.Z + (np.Z - nn.Z)) * 0.5F};
    if (!ScreenQuad::Normalise(rightAxis) || !ScreenQuad::Normalise(upAxis)) return std::nullopt;

    // The quad's own normal, from the two edge midlines. `ScreenQuad` keeps only
    // the arithmetic a screen needs to be drawn (`Rotate`, `Normalise`, `Dot`),
    // so the cross product is spelled out here rather than added to a module the
    // pure tests cover.
    ScreenQuad::Vec3 normal{rightAxis.y * upAxis.z - rightAxis.z * upAxis.y,
                            rightAxis.z * upAxis.x - rightAxis.x * upAxis.z,
                            rightAxis.x * upAxis.y - rightAxis.y * upAxis.x};
    if (!ScreenQuad::Normalise(normal)) return std::nullopt;

    const auto direction = RED4ext::Vector4{aDirection.X, aDirection.Y, aDirection.Z, 0.0F};
    const float denominator = normal.x * direction.X + normal.y * direction.Y +
                              normal.z * direction.Z;
    if (std::abs(denominator) < 1.0e-6F) return std::nullopt;
    const auto toCentre = RED4ext::Vector4{centre.X - aEye.X, centre.Y - aEye.Y,
                                           centre.Z - aEye.Z, 0.0F};
    const float numerator = normal.x * toCentre.X + normal.y * toCentre.Y +
                            normal.z * toCentre.Z;
    const float t = numerator / denominator;
    if (t <= 0.0F) return std::nullopt;

    const auto hit = RED4ext::Vector4{aEye.X + direction.X * t, aEye.Y + direction.Y * t,
                                      aEye.Z + direction.Z * t, 0.0F};
    const ScreenQuad::Vec3 local{hit.X - centre.X, hit.Y - centre.Y, hit.Z - centre.Z};
    const float alongRight = ScreenQuad::Dot(local, rightAxis);
    const float alongUp = ScreenQuad::Dot(local, upAxis);
    const float u = alongRight / aWidth + 0.5F;
    const float v = alongUp / aHeight + 0.5F;
    if (u < 0.0F || u > 1.0F || v < 0.0F || v > 1.0F) return std::nullopt;

    PlaneHit result{};
    result.u = u;
    // The page's own v runs downwards from the top, the panel's up axis runs
    // upwards: the pointer is seeded in page coordinates, so the flip is here.
    result.v = 1.0F - v;
    result.distance = t;
    return result;
}
} // namespace

Status HitTest(
    const RED4ext::Vector4& aEye,
    const RED4ext::Vector4& aDirection,
    Hit& aOut)
{
    const std::scoped_lock lock(s_mutex);
    if (!s_running) return Status::NotFound;

    bool found = false;
    float nearest = 0.0F;
    Hit best{};
    for (const auto& [id, entry] : s_entries)
    {
        if (entry.definition.surface == 0) continue;
        // A panel the eye is behind is not clickable: taking a screen whose
        // picture is not even drawn would hand the mouse to something the player
        // cannot see, which is indistinguishable from the feature not working.
        if (entry.facingKnown && entry.facingAway) continue;
        const uint64_t entity = Props::ProjectedEntity(entry.definition.prop);
        if (entity == 0) continue;
        auto object = EntityService::Lock(Core::EntityId{entity});
        if (!object) continue;
        RED4ext::Vector4 position{};
        RED4ext::Quaternion orientation{};
        if (!ReadPlacement(object, position, orientation)) continue;
        std::vector<RED4ext::Vector4> world;
        if (!ScreenCorners(entry, position, orientation, world)) continue;
        const ScreenQuad::Quad mapping = MappingFor(entry.definition);
        const auto hit = Intersect(world, mapping.width, mapping.height, aEye, aDirection);
        if (!hit) continue;
        if (found && hit->distance >= nearest) continue;
        found = true;
        nearest = hit->distance;
        best.id = id;
        best.surface = entry.definition.surface;
        best.u = hit->u;
        best.v = hit->v;
        best.distance = hit->distance;
    }
    if (!found) return Status::NotFound;
    aOut = best;
    return Status::Ok;
}

bool ToggleControl()
{
    const bool held = (GetAsyncKeyState(kControlKey) & 0x8000) != 0;
    const bool pressed = held && !s_controlKeyHeld;
    s_controlKeyHeld = held;
    if (!pressed) return false;

    if (WebUiService::ScreenInput::Get().active)
    {
        WebUiService::ScreenInput::Release();
        LogInfo("screen control released (F8)");
        return true;
    }

    Camera::View view{};
    if (Camera::Describe(view) != Camera::Status::Ok)
    {
        LogInfo("screen control refused: no camera to aim from");
        return false;
    }
    Hit hit{};
    const Status status = HitTest(view.position, view.forward, hit);
    if (status != Status::Ok)
    {
        LogInfo("screen control refused: nothing under the crosshair (%s)", Describe(status));
        return false;
    }
    if (!WebUiService::ScreenInput::Take(hit.surface, hit.u, hit.v))
    {
        LogInfo("screen control refused: surface %llu is not presenting",
                static_cast<unsigned long long>(hit.surface));
        return false;
    }
    LogInfo("screen control taken: surface %llu at %.2f m (F8 releases, mouse and keyboard go to the page)",
            static_cast<unsigned long long>(hit.surface), static_cast<double>(hit.distance));
    return true;
}

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
    // The pointer/keyboard handoff, before anything is projected: taking a screen
    // is a state change, and the pointer it seeds has to be drawn by the frame
    // this same pass publishes.
    static_cast<void>(ToggleControl());

    // One clear for the whole tick, before anything below may publish: where the
    // pointer is drawn is a per-tick answer, and a screen that stops being built
    // -- its prop unstreamed, its quad refused, the camera behind it -- must not
    // keep last tick's place on the wall. Whoever can publish does so below.
    WebUiService::ScreenInput::ClearPointerOnScreen();

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
    // The world is going away, so no screen can be under the crosshair any more.
    // Released explicitly rather than left to the WebUI teardown: the focus it
    // holds suppresses the game's own raw input, and a client that kept that
    // across a world change would come back with no mouse look.
    WebUiService::ScreenInput::Release();
    const std::scoped_lock lock(s_mutex);
    s_running = false;
    for (const auto& [_, entry] : s_entries)
    {
        RetireProducer(entry.owner);
    }
    s_entries.clear();
}
} // namespace op77::Api::MediaScreens
