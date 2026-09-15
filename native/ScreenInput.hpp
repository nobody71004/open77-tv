#pragma once

#include <cstdint>

// Handing the pointer and the keyboard to a screen that lives in the world.
//
// ---------------------------------------------------------------------------
// WHY THIS IS ITS OWN FILE, WITH NO PLATFORM HEADERS IN IT
// ---------------------------------------------------------------------------
//
// Two modules need the same fact -- which television, if any, currently owns the
// mouse and the keyboard, and where on its page the pointer is:
//
//   * `WebUiService` owns the session. It is the module that already routes raw
//     input to a focused surface, swallows `WM_INPUT` so the game does not also
//     act on the keys, and sends the mouse, key and character messages over the
//     pipe. It is also the module that keeps `Windows.h` and `d3d12.h` in its
//     header.
//   * `Api::MediaScreens` draws the pointer, because the pointer belongs on the
//     panel: CEF rasterises a page into a texture and the *host* paints the
//     cursor, so a page on a world quad has no cursor unless something put one
//     there. That module runs on the game thread and must not pull in a window
//     header and a graphics API to read one enum -- see the note at the top of
//     `WorldOverlay.hpp`.
//
// So the state lives here: a plain struct, four functions, no includes beyond a
// fixed-width integer. `WebUiService.cpp` defines them; everything else declares
// against it.
namespace op77::WebUiService::ScreenInput
{
/// The session, as anything outside the WebUI service sees it.
struct State
{
    /// True while a screen owns the pointer and the keyboard.
    bool active{};
    /// The WebUI surface that owns them. Zero when nothing does.
    uint64_t surface{};
    /// Where the pointer is on that page, as a fraction of its viewport: 0..1
    /// with the origin at the top-left, the same space the page's own CSS uses.
    /// A fraction rather than a pixel because the two things that consume it do
    /// not agree on units -- the input path works in the surface's view pixels,
    /// and the panel draws in world metres -- and the conversion belongs at each
    /// end rather than in the middle.
    float u{0.5F};
    float v{0.5F};
    /// Where the pointer is to be DRAWN, as a fraction of the WebUI viewport --
    /// the space the overlay's own cursor is positioned in, which is NOT the page
    /// fraction above. The page is composited onto a panel somewhere in the
    /// world, so the two agree only when the panel happens to fill the viewport;
    /// everywhere else the raw pointer's viewport position is not over the panel
    /// at all, and a cursor drawn there would point at the world rather than at
    /// the page the click will land on.
    ///
    /// `pointerOnScreen` is false when the screen that owns the session had no
    /// position to publish this tick -- its prop unstreamed, its quad refused,
    /// the camera behind it -- in which case the cursor stays where the raw
    /// pointer is, exactly as it does with no session open.
    bool pointerOnScreen{};
    float screenU{0.5F};
    float screenV{0.5F};
};

[[nodiscard]] State Get();

/// Takes the pointer and the keyboard for a surface, seeding the pointer at
/// `aU`/`aV`. False when there is no such surface, or when it cannot be given
/// input -- a page that is not presenting has nothing to click on.
///
/// Focus is what does the work: the WebUI service already suppresses the game's
/// raw input while a surface holds keyboard or cursor focus, and already routes
/// mouse, key and character messages to whichever surface that is. Taking a
/// screen over is therefore not a second input path -- it is asking for the focus
/// the path was built around.
[[nodiscard]] bool Take(uint64_t aSurface, float aU, float aV);

/// Gives it back, and hides the pointer. Safe to call when no session is open.
void Release();

/// Moves the pointer on the page. Called by the input path with every motion
/// sample and before every button, so a click lands where the pointer is drawn.
void SetPointer(float aU, float aV);

/// Publishes where the pointer is to be DRAWN, as a fraction of the viewport.
///
/// Called once a tick by the module that knows where the panel's quad landed in
/// front of the camera -- the same arithmetic that decides where the page itself
/// is composited, so the ONE pointer a player sees is the one a click lands
/// under. Withdraw it with `ClearPointerOnScreen` on a tick that has nothing to
/// publish, or the cursor keeps last tick's place on a screen that has moved,
/// unstreamed or been turned away.
void PublishPointerOnScreen(float aU, float aV);
void ClearPointerOnScreen();
} // namespace op77::WebUiService::ScreenInput
