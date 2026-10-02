# The picture: in perspective, behind what is in front of it, cut at the camera

A television's page is composited by the client's overlay after the game has
finished its frame: the producer (`native/MediaScreens.cpp`) projects the screen's
corners on the game thread and `WorldOverlay` hands them to `WebUiService`, which
queues the page's newest frame into the overlay's ImGui draw list. Three things
were wrong with the picture that path drew, and the patch series in
`patches/open77-base/` (pull request #66 on open77-base,
`fix/tv-picture-in-perspective`) fixes them. This is what each part does and why.

| Patch | What |
|---|---|
| `0001` | the picture in perspective, not bent along a diagonal |
| `0002` | the picture behind whatever the game drew in front of it |
| `0003` | the depth pass's shaders kept out of Visual Studio's shader compiler |
| `0004` | the depth test at every present, with frame generation |
| `0005` | a screen with part of it behind the camera: the part in front, exactly |
| `0006` | `0005`'s test target built without Windows' `min`/`max` macros |

They apply with `git am` on open77-base `main` (36772b3b).

## In perspective (`0001`)

ImGui interpolates a texture linearly in screen space. One image quad over a
screen's four projected corners is therefore exact at the corners and bent
everywhere between them, along the diagonal where its two triangles meet, and
the bend changes as the player walks round the set: "the screen moves when it is
looked at from a different angle". Head-on it is exact, which is how a set is
usually checked; a television turned 50 degrees at 1.5 m was 102 px out, the 150 ft
cinema seen at 35 degrees 306 px.

`ScreenTessellation.hpp` turns the four corners into the plane's own map (a
homography), bounds how far linear interpolation errs over a cell from the map's
second derivatives, and picks the smallest grid whose bound is under 0.75 px
(at most 48 x 48). `PerspectiveImage::Add` draws that grid with exact vertices,
one draw command, cells off the view skipped. A set seen square is still the one
quad it always was. Measured on the same two cases: 0.43 px and 0.51 px.

## Behind what is in front of it (`0002` to `0004`)

The overlay has no depth of its own, so the picture was painted over V's hands
and weapon, over people walking past, over the door frame it was seen through.
The game's own depth buffer knows what is in front, so the fix uses it:

- **Finding it** (`SceneDepth.cpp`, `SceneDepthPolicy.hpp`). From plugin load a
  hook on `D3D12CreateDevice` (through RED4ext) watches the game's device, and on
  the runtime's own objects notes which depth buffer each command list binds,
  how much it draws into it, its state changes and clears, and the order lists
  run in. The buffer that takes the scene's work, summed over the last several
  presents, is the scene's. Nothing the game does is changed.
- **Copying it** at present into a texture of the overlay's own, on the overlay's
  own command list, once that frame's depth is finished, and putting the game's
  buffer back in the state it was in. The copy is reused at every present until
  a newer one replaces it, which is what keeps the test on with frame generation
  (`0004`: four presents to a rendered frame, and the small pass's buffer of the
  same size no longer taken for the scene's).
- **What a value means.** Device depth is `A + B / z` for every perspective
  projection; the two numbers are measured from the screens themselves (the
  producer knows each screen's view depth, and the buffer has the set's own glass
  in it) and locked once enough looks agree.
- **Drawing through it** (`ScreenDepthPass`, `ScreenDepthComposite.hlsl`,
  `ScreenDepth.hlsli`). One draw a screen; the pixel shader works out each pixel's
  exact texture coordinate and view depth from the screen's map, compares it with
  the game's depth with a few centimetres of allowance for the set's glass and a
  soft edge, and hides what is behind. The arithmetic is one `.hlsli` that the C++
  tests compile too; the shaders are compiled at start-up from text embedded in
  `ScreenDepthShaderSource.hpp` (`0003` keeps the files out of the build's own
  shader compiler), and a test fails when that text and the files disagree.

Until it has found the buffer, a screen is drawn exactly as `0001` draws it.
In game: Ctrl+Shift+F10 shows the depth image in use, Ctrl+Shift+F11 turns the
test off and on, Ctrl+Shift+F9 tries the next candidate buffer.
`OP77_TV_DEPTH=0` turns all of it off. Everything is logged as `TV depth:`.

## Cut at the camera (`0005`)

Reported from a recording on 2026-10-01: walking along the 150 ft cinema with its
near end behind the camera, the picture became rays of colour bars from the
middle of the view and half of it went missing. A corner behind the eye has no
projection: the engine refuses the point and `Camera::ProjectPoints` writes it as
zero, the centre of the view. `ScreenTessellation` refused that quad, and the
refusal fell back to the single image quad over it.

`ScreenClip.hpp` draws such a screen from its plane instead:

- A screen is flat, so its view depth is affine in texture space. The part at
  least 0.2 m in front of the camera is the unit square cut by one line: three,
  four or five corners.
- Four points inside that part are projected by the engine like any corner. Each
  row of the map from texture space to the view is affine in (u, v) -- position
  times depth, and depth -- so the map is a least-squares fit through them, well
  conditioned however small or oblique the screen looks. Points that are not one
  flat screen seen through one camera (the engine refused one) are refused: the
  producer reports `clip_unsolved` for that tick rather than drawing it wrong.
- Which way round the picture goes is `ScreenQuad::Orient`'s own rule, asked
  through the map where the screen is in front of the camera.
- The overlay item carries the map (`Item::clipped`, nine floats) and is blended
  between game ticks like corners are; the producer reports `drawn_clipped`.
- The depth pass draws the part in front as a fan (its geometry gained a fourth
  register; a whole screen is the same two triangles as before) and the pixel
  shader needs nothing new: the map's inverse is its three rows, and its third row
  is already 1 / depth.
- Without the depth pass, `PerspectiveImage::AddClipped` cuts the screen to the
  near plane and the view and halves its texture space, quadrant by quadrant,
  until every cell is within the tolerance. A uniform grid cannot: the curvature
  grows as the cube of nearness, so cells a few metres away must be hundreds of
  times smaller than those at the far end of the cinema.

## Known: the HUD under a television

Where the game's HUD overlaps a television, the picture is drawn over it: the
overlay composites after the whole frame, HUD included. The plan is to find the
pass in which the game draws its HUD, the way the depth buffer is found (it is
the last work into the image, with no depth), and draw the screens before it; or,
if the HUD is composited from a target of its own, keep a copy of it and draw it
back over the screens.

## Where the code is

`native/`: `ScreenTessellation.hpp`, `PerspectiveImage.hpp`, `ScreenClip.hpp`,
`ScreenDepth.hpp`, `ScreenDepth.hlsli`, `ScreenDepthComposite.hlsl`,
`ScreenDepthShaderSource.hpp`, `ScreenDepthPass.hpp|cpp`, `SceneDepth.hpp|cpp`,
`SceneDepthPolicy.hpp`, and `MediaScreens.cpp` and `ScreenMotion.hpp` as the
series leaves them. The seams (`WebUiService`, `WorldOverlay`, `client/CMakeLists.txt`)
are in the patches.

`tests/`: `ScreenTessellationTests.cpp`, `PerspectiveImageTests.cpp` (a real
ImGui draw list read back), `ScreenDepthTests.cpp` (the shader's arithmetic
against a ray-traced scene: a hand, a pillar, a wall, three depth conventions,
upscaled and dynamic-resolution depth), `SceneDepthPolicyTests.cpp` (which buffer,
what its values mean, when to copy), `ScreenDepthShaderSourceTests.cpp`, and
`ScreenClipTests.cpp` (the cut, the map against a ray-traced camera -- to a
millionth of a pixel, and within 0.02 px through float32 arithmetic 2 km from
the origin -- the refusals, the depth pass from the map pixel by pixel, the fan's
coverage, the orientation in nine poses, the curvature bound).
