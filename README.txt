GHWT:DE Ultrawide Fix  (v0.3 BETA)
=============================
By WitchDoctoR

On screens wider than 16:9 (2560x1080, 3440x1440, ...) Guitar Hero World Tour: Definitive Edition stretches its
whole 2D layer sideways: HUD, menus, text. This ReShade add-on draws that layer as the closest 16:9 picture, centred
(1920x1080 inside 2560x1080), unstretched. The 3D scene (venue, band, highway) keeps the full width.

Per screen (v0.2): full-screen gradients and dimmers stretch to the full width; full-screen backgrounds fill the
screen without distortion (title screen wall, song list side art, every YOU ROCK theme); loading screens are a
16:9 picture with black side bars (sharper than zooming the art; see LoadingFill); side panels (Career / Quickplay mode select, instrument select, the song intro) pin to the screen's left edge with
their text. Everything else stays a centred 16:9 picture. Also bundled with the GH5 / WoR HUD mod.

REQUIREMENTS
  - Guitar Hero World Tour: Definitive Edition.
  - ReShade 6.x, 32-bit, Direct3D 9 (with add-on support). A copy is included (see below).

INSTALL (into the folder with GHWT_Definitive.exe)
  1. Copy ghwtde_ultrawide.addon32 next to GHWT_Definitive.exe.
  2. No ReShade yet? Also copy d3d9.dll from "ReShade (only if you do not have it)".
  In game, the ReShade overlay (Home key) > Add-ons tab shows "GHWT:DE Ultrawide Fix" with an on/off checkbox and
  the number of UI draws it adjusted in the last frame.

UNINSTALL
  Delete ghwtde_ultrawide.addon32 (and d3d9.dll if you copied it only for this).

KNOWN LIMITS (v0.3 beta, not a final build)
  - Face-off / battle HUDs are not adjusted yet (centred 16:9).
  - Screens the add-on has no rule for stay a centred 16:9 picture (the 3D scene shows at the sides).
  - The per-element modes hook GHWT_Definitive.exe; on another game build they switch off (centring still works).
  - At 16:9 or narrower the add-on does nothing.

SETTINGS (ReShade.ini, section [GHWTDE_ULTRAWIDE]; all optional)
  Enabled=1
  ElementModes=1         per-element modes below (0 = everything centred, as in v0.1)
  FullScreenImages=cover full-screen sprites with a large texture (backgrounds): cover | stretch | keep
  FullWidth=stretch      other full-width sprites (gradients, vignettes, dimmers)
  MinImageSize=512       texture size that makes a full-screen sprite an "image"
  Texture rules (hex texture checksums, comma-separated):
    Cover=  Stretch=  Keep=        keep = stays in the centred 16:9 picture
    Slice=ID:a:b                   image part a..b (0-1 of its width) stretches, the rest keeps its proportions
    Tile=ID:a:b[:f]                image part a..b repeats (mirrored) across the screen, the rest stays centred,
                                   its left edge fading in over width f (built in: title wall 5049d8f2:0.58:1:0.04)
  Element groups (hex element ids; the element and everything under it):
    Left=  Right=  Centre=  Bars=  (built in: select-mode paper 97dc36b4 + header 29aa72ea, instrument paper
                                   98fe6fdc, song intro 43823aca, title screen flash ce1ae821 -> left;
                                   title screen flash 3415d542 -> right; loading screen aed7cd2e -> bars;
                                   YOU ROCK layouts a39d36f7, c5fe03f7, 427ea435, the exploding text
                                   6139b417 and its sparks 32cc87b4, the WoR HUD mod's YOU ROCK bolts
                                   881079ee -> cover)
  Built in as well: song list side art 4b93dd1e / b19ce07d grow from the screen edges; title screen overlay
  f8d1f74f stretches.
  LoadingFill=0          1 = loading screens fill the width instead (the movie is scaled to cover the screen; the
                         GH5 / WoR HUD mod's Warriors of Rock loading picture 468df16a, wheel and plate frames cover
                         it at 16:9, anchored so the ring and the LOADING plate stay whole). Also a checkbox in the
                         add-on's overlay.
  AutoAnchor=0           experimental: pin any panel whose art touches a screen edge
  AnchorInHud=0          allow pinning inside the gameplay HUD
  Catalog=0              1 = developer log: every new element once to ghwtde_ultrawide_catalog.txt, plus one frame
                         dump per movie screen (slows the game down with moving text; leave off)
  CompositePS=DE796938   (pixel shader hash of the game's scene composite; only change if a DE update breaks it)
  The overlay button "Dump this frame's 2D draws" (or Ctrl+Shift+U) appends the frame's UI draws to
  ghwtde_ultrawide_dump.txt next to the game exe.
CREDITS
  ReShade by crosire (BSD-3-Clause, license included). Dear ImGui by Omar Cornut (MIT).
