#pragma once

// Draws a television's picture behind whatever the game drew in front of it.
//
// The overlay composites a screen's page after the game has finished its frame,
// into the presented image, with no depth of its own: so the picture used to be
// painted over V's hands and body, over people walking past and over the door
// frame it was seen through. This module finds the depth buffer the game drew the
// frame with, copies it at present into a texture of the overlay's own, and draws
// each screen with the depth-tested pass (ScreenDepthPass), which hides every pixel
// of the picture that something in the game is in front of.
//
// Finding the buffer takes watching the game draw. From plugin load it waits for
// the game's Direct3D 12 device (a hook on D3D12CreateDevice, through RED4ext), and
// then, on the runtime's own objects, notes which depth buffers each command list
// binds and how much it draws into each, every state change of a depth buffer, and
// the order the lists run in. At present the buffer that took the scene's work is
// the scene's (SceneDepthPolicy::Rank). Nothing the game does is changed: the
// copy is made on the overlay's own command list, and the buffer is put back in the
// state the game left it in.
//
// What a depth value means (device = A + B / z) is measured from the screens
// themselves (SceneDepthPolicy::Calibration); until it is, a screen is drawn in
// perspective without the test, exactly as before.
//
// In game:
//   Ctrl+Shift+F10  shows the depth buffer in use, top left (lighter is nearer)
//   Ctrl+Shift+F11  turns the depth test off and on, to compare
//   Ctrl+Shift+F9   tries the next candidate buffer (then back to automatic)
// OP77_TV_DEPTH=0 in the environment turns all of it off: nothing is hooked.
//
// Everything is logged with the prefix "TV depth:".

#include <cstdint>
#include <string>

#include <d3d12.h>

#include "ScreenClip.hpp"

struct ImDrawList;

namespace op77::WebUI::SceneDepth
{
enum class LogLevel : uint8_t
{
    Info,
    Warning,
};

using LogFunction = void (*)(LogLevel aLevel, const std::string& aMessage);
/// RED4ext's Hooking::Attach, bound to the plugin: hooks `aTarget`, returns the
/// trampoline in `aOriginal`.
using AttachFunction = bool (*)(void* aTarget, void* aDetour, void** aOriginal);

/// Plugin load. Hooks D3D12CreateDevice so the game's device is seen from its
/// first depth buffer on. Does nothing with OP77_TV_DEPTH=0.
void Install(AttachFunction aAttach, LogFunction aLog);

/// Plugin unload: puts back every table entry this changed.
void Uninstall();

/// The overlay's graphics came up on `aDevice`. If the device was created before
/// Install could see it, the hooks go on now (depth buffers made before that are
/// found by their state changes instead). `aCpuSlot`/`aGpuSlot` are one descriptor
/// of the overlay's shader-visible heap, kept for the copy's view.
void OnGraphicsReady(ID3D12Device* aDevice, D3D12_CPU_DESCRIPTOR_HANDLE aCpuSlot, D3D12_GPU_DESCRIPTOR_HANDLE aGpuSlot,
                     DXGI_FORMAT aRenderTargetFormat);

/// Before the overlay's device goes away (its frames have finished).
void OnGraphicsShutdown();

/// Once per present, on the overlay's command list, before ImGui records into it.
/// Picks the scene's depth buffer from the frame just finished and copies it.
void PrepareFrame(ID3D12GraphicsCommandList* aCommands, uint64_t aCompletedFence, float aOverlayWidth,
                  float aOverlayHeight);

/// The fence the overlay's list of this present signals.
void FrameSubmitted(uint64_t aFenceValue);

/// Draws one screen through the depth pass, into `aDraw` (ImGui callbacks, run
/// when the overlay renders). False when the pass cannot draw it: the caller then
/// draws it the old way. `aCentreDepth` is the view depth of the screen's centre.
[[nodiscard]] bool DrawScreen(ImDrawList& aDraw, D3D12_GPU_DESCRIPTOR_HANDLE aPicture, const float (&aCorners)[8],
                              float aCentreDepth, float aOpacity);

/// The same for a screen with part of it behind the camera (ScreenClip.hpp):
/// `aMap` takes texture space to overlay pixels, its third row the view depth.
/// Only the part in front of the camera's near plane is drawn. False, as above,
/// when the pass cannot draw it.
[[nodiscard]] bool DrawClippedScreen(ImDrawList& aDraw, D3D12_GPU_DESCRIPTOR_HANDLE aPicture,
                                     const op77::WorldOverlay::ScreenClip::Map& aMap, float aOpacity);

/// Once per frame while the overlay is built: the debug view, when it is on.
void DrawOverlay(ImDrawList& aForeground, float aOverlayWidth, float aOverlayHeight);

/// One line for the log and the debug bridge.
[[nodiscard]] std::string Describe();
} // namespace op77::WebUI::SceneDepth
