# The television picture fixes, as a patch series for open77-base

Six patches, in order, for `git am` on open77-base `main` (36772b3b). They are the
pull request `fix/tv-picture-in-perspective` (#66) on open77-base -- `0005` and
`0006` join it once they have been tested in game -- and what
`docs/depth-and-occlusion.md` describes.

| Patch | What |
|---|---|
| `0001-tv-picture-in-perspective` | the picture in perspective, not bent along a diagonal |
| `0002-tv-picture-in-the-games-depth` | the picture behind whatever the game drew in front of it |
| `0003-tv-depth-shaders-out-of-fxc` | the depth pass's shaders kept out of Visual Studio's shader compiler |
| `0004-tv-depth-with-frame-generation` | the depth test at every present, with frame generation |
| `0005-tv-picture-cut-at-the-camera` | a screen with part of it behind the camera: the part in front, exactly |
| `0006-tv-clip-test-without-minmax-macros` | `0005`'s test target built without Windows' `min`/`max` macros |

    git -C <open77-base> checkout -b fix/tv-picture-in-perspective origin/main
    git -C <open77-base> am patches/open77-base/000*.patch

The files they add are mirrored in `native/` and `tests/` here, as the series
leaves them.
