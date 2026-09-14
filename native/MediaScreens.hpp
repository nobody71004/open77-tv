#pragma once

// A WebUI surface on a screen that lives in the world.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS
// ---------------------------------------------------------------------------
//
// A resource creates a WebUI surface -- that is existing machinery, with its
// whole lifecycle already proven -- and then says *where in the world that
// surface belongs*: on this spawned prop, at this quad, in the prop's own local
// frame. Every game tick this module reads the prop's world transform, builds
// the four corners of that quad, projects them with the engine's own camera, and
// publishes one `WorldOverlay::Style::Screen` item. `WorldOverlay::Draw` then
// hands the surface id to `WebUiService::DrawSurfaceQuad`, which composites the
// page's newest rasterised frame into the quad.
//
// The split is deliberate: the page is CEF's business, the descriptors are the
// WebUI service's business, and the only thing that has to happen on the game
// thread -- projecting a world point against the camera the frame will use -- is
// the only thing that happens here.
//
// ---------------------------------------------------------------------------
// WHY THE QUAD IS DATA, NOT GEOMETRY
// ---------------------------------------------------------------------------
//
// The obvious alternative is to read the screen rectangle out of the prop's own
// mesh: find the screen submesh, take its bounds, and drive that. It was not
// done, and the reason is a property of this engine rather than a shortcut. A
// vanilla prop `.ent` is not spawnable here and `entMeshComponent::mesh` cannot
// be redirected after the component is attached (docs/research/props-and-object-
// spawning.md), so the props this module serves are Open77-owned hosts whose
// component graph is authored at asset-build time -- the names available to a
// script are aliases into that set (`Api::Props::Catalog`). Reading a screen
// rectangle back out of one of those would mean resolving a submesh by name on
// every distinct television, and a prop that renamed that submesh would move the
// screen silently.
//
// So the quad is authored, once, per television record, in metres relative to
// the prop's origin -- the same place the model alias is authored. It is
// therefore the record catalogue that knows a 55-inch wall panel from a
// countertop monitor, which is also where a designer would look for it.
//
// ---------------------------------------------------------------------------
// OCCLUSION: WHAT IS AND IS NOT HANDLED
// ---------------------------------------------------------------------------
//
// The overlay pass this draws into has no depth buffer, so a screen drawn on a
// wall between the camera and the television would appear through it. That is
// not acceptable for a large in-world rectangle, so this module refuses to
// publish a screen whose centre is behind geometry: one `Api::WorldQuery::
// Raycast` per screen per tick from the eye to the quad's centre.
//
// Be exact about what that buys, because it is a coarse test and it should not
// be mistaken for a depth buffer:
//
//   * Fully visible and fully hidden both read correctly.
//   * A screen half behind a pillar is drawn whole, because the test is on the
//     centre. It will look wrong from exactly the angles where the pillar
//     bisects the panel.
//   * A screen whose centre is visible through a doorway is drawn even though
//     most of it is not.
//
// The cost is bounded and paid per screen, not per pixel: one trace per screen
// per game tick, skipped entirely when a screen's quad is off-view or behind the
// camera (both tested before the trace, because the projection is cheaper).
//
// ---------------------------------------------------------------------------
// THE ONE THING THAT IS NOT SETTLED BY MEASUREMENT HERE
// ---------------------------------------------------------------------------
//
// The composite is a screen-space quad with no depth test, so a screen is drawn
// over the game's world whatever stands between the camera and it. That is a
// property of the pass, not of this module, and closing it means giving the
// overlay a depth buffer, which is a different piece of work.
//
// Its RELATION TO THE INTERFACE is settled, and it took a report to settle it:
// screens are published as `WorldOverlay::Style::Screen` and the caller draws
// that layer BEFORE the CEF pages (`WorldOverlay::Layer::Screens`), so opening
// the freeroam menu covers a television instead of the television covering the
// menu. Anything that changes the order of those two calls brings that back.
//
// ---------------------------------------------------------------------------
// WHICH SIDE OF THE PANEL CARRIES THE PICTURE
// ---------------------------------------------------------------------------
//
// A quad is a rectangle, and a rectangle has two sides. Nothing in a projection
// distinguishes them, so a television seen from behind carried its video on the
// back of the cabinet as convincingly as on the glass ("tvs are playing videos
// on both sides"). The rule that puts the picture the right way up
// (`ScreenQuad::Orient`) deliberately keeps the picture readable from either
// side and therefore cannot answer this: by the time the picture is upright, the
// fact that it is on the wrong side has been erased.
//
// So the record declares it -- `Quad::faces`, a direction in the prop's own
// frame, authored beside the rectangle in the catalogue and checked against the
// asset's own measured geometry (see the note above each record). One dot
// product per screen per tick decides it, and a record that declares nothing is
// drawn from both sides exactly as before: a screen that vanishes on a guess is
// worse than a screen seen through its own cabinet.
//
// This is not the occlusion test and does not replace it. Occlusion answers "is
// something in the way"; this answers "is this the back of the thing". A set
// standing free in a room has nothing in the way from behind and needs this gate
// to stop showing its picture through the cabinet.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <RED4ext/RED4ext.hpp>

namespace op77::Api::MediaScreens
{
enum class Status : uint8_t
{
    Ok,
    InvalidArgument,
    NotFound,
    OwnedByAnotherResource,
    QuotaExceeded,
    /// The prop this screen is bound to is not projected on this client. Normal
    /// and transient while a television streams in and out of range.
    PropNotProjected,
};

[[nodiscard]] const char* Describe(Status aStatus);

/// The screen rectangle in the prop's own local frame, in metres.
///
/// `offset` is the quad's centre relative to the prop's origin; `right` and `up`
/// are the quad's axes in local space, so a wall-mounted panel and a monitor
/// tilted back on a desk are the same struct. They are not required to be
/// orthogonal or unit length -- they are normalised and the up axis is
/// orthogonalised against right before use -- which lets a record write
/// something like `right = {1, 0, 0}, up = {0, 0, 1}` for an upright screen and
/// `up = {0, -0.34, 0.94}` for one tilted back twenty degrees, without the
/// author having to do the algebra.
///
/// Both axes are rotated by the prop's *world* orientation at draw time, so a
/// television turned to face the room needs no adjustment here.
struct Quad
{
    float offset[3]{0.0F, 0.0F, 1.0F};
    float right[3]{1.0F, 0.0F, 0.0F};
    float up[3]{0.0F, 0.0F, 1.0F};
    /// Full width and height of the screen in metres, not half-extents. This is
    /// the one place the struct prefers the number a person reads off a spec
    /// sheet.
    float width{1.2F};
    float height{0.68F};
    /// Which way the display looks, as a direction in the prop's own local frame
    /// -- `{0, 1, 0}` for a television whose picture is on its +Y face. A zero
    /// vector, the default, means the record does not say and the screen draws
    /// from both sides.
    ///
    /// It is deliberately a direction in the *asset's* frame rather than a
    /// derived convention (a winding order, a cross product of `right` and `up`,
    /// or the sign of `offset`). Each of those encodes an assumption about
    /// handedness that this file has already been wrong about once -- see
    /// `ScreenQuad.hpp`. A direction written down is one number that can be
    /// checked against the mesh and corrected in one place.
    float faces[3]{0.0F, 0.0F, 0.0F};
};

/// Everything `Bind` accepts, and what `SnapshotAll` reports back.
struct Definition
{
    /// The server prop id this screen is attached to, as replicated by the props
    /// channel. Binding to an id that is not projected yet is allowed and
    /// expected -- the screen simply is not drawn until the prop streams in.
    uint64_t prop{};
    /// The WebUI surface whose frames are composited. Zero removes the screen's
    /// content but keeps its binding, which is how a television is left switched
    /// on with nothing playing.
    uint64_t surface{};
    Quad quad{};
    /// Shown by the debug bridge and in logs. Never sent to the page.
    std::string label;
};

struct Snapshot
{
    uint64_t id{};
    std::string owner;
    Definition definition;
    /// Whether the last game tick got as far as publishing an item for it.
    /// False for a prop that is not projected, a quad that is off-view, a screen
    /// behind geometry, or a screen being looked at from behind -- `reason` says
    /// which, and `behind_panel` is the facing gate.
    bool drawn{};
    std::string reason;
    float distance{};
    /// Whether the record declared a front at all, and, when it did, whether the
    /// eye was on the wrong side of it. `known = false` is a record that never
    /// said which side the picture is on -- those still draw from both sides, and
    /// saying so out loud is the difference between an unfinished catalogue entry
    /// and a mystery.
    bool facingKnown{};
    bool facingAway{};
};

/// Registers a screen. The owner string is the resource name, exactly as with
/// `Api::Props`: one resource can never unbind another's screens, and a resource
/// stop releases its own.
[[nodiscard]] Status Bind(
    std::string_view aOwner,
    const Definition& aDefinition,
    uint64_t& aId);

/// Replaces the quad and label. A change of `prop` or `surface` is legal and
/// takes effect on the next tick; there is no respawn, because nothing has been
/// spawned here -- the screen is a projection over an entity this module does
/// not own.
[[nodiscard]] Status Update(
    std::string_view aOwner,
    uint64_t aId,
    const Definition& aDefinition);

[[nodiscard]] Status Unbind(std::string_view aOwner, uint64_t aId);

/// Drops every screen owned by a resource. Called wherever its anchors and props
/// are released, so a stopped resource cannot leave a picture hanging in the
/// world.
void Release(std::string_view aOwner);
void ReleaseAll();

[[nodiscard]] std::vector<Snapshot> SnapshotAll(std::string_view aOwner = {});
[[nodiscard]] size_t Count();

/// How many screens one resource may hold, and how many this client may hold at
/// once. Screens are cheap -- no entity, one raycast per tick each -- but each
/// one is a trace against the physics world every frame, and a bound is the only
/// thing that keeps a runaway script from turning that into a frame cost.
[[nodiscard]] size_t PerOwnerLimit();
[[nodiscard]] size_t GlobalLimit();

void Initialize(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
void OnRunningEnter();
/// Game thread, once per frame. Projects and publishes every live screen.
void OnRunningUpdate();
void OnRunningExit();
} // namespace op77::Api::MediaScreens
