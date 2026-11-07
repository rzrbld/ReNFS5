# Known issues

renfs5 is experimental. Complete handheld gameplay (career, every track and
mode) remains **UNVERIFIED**. On H700 the garage reaches 60 FPS and races mostly
run above **30 FPS**; the heaviest scenes (chase camera with the whole pack in
view) dip into the high 20s.

- Supported translation inputs are the v3.2 executable and renderer pair.
  Other game revisions are unverified.
- CD images containing only a Windows installer need PC-side installation first.
- The FPS cap is a pacing target, not a guarantee that every scene reaches it.
- Detail: LOW uses sharp nearest-pixel textures, MEDIUM the game's own
  filtering, HIGH adds mipmapped trilinear/anisotropic textures and 4x
  anti-aliasing (costs GPU time; distant see-through details such as fences
  can look thinner). The game's in-game graphics options still apply.
  Higher resolutions are unverified on handhelds.
- Some original keyboard graphics/HUD shortcuts are not implemented.

Back up saves/ and configs/ before upgrading. For errors, retain log.txt and
configs/launcher-worker.log.
