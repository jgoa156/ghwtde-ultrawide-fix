GHWT:DE Ultrawide Fix  (v0.1 BETA)
=============================
By WitchDoctoR

On screens wider than 16:9 (2560x1080, 3440x1440, ...) Guitar Hero World Tour: Definitive Edition stretches its
whole 2D layer sideways: HUD, menus, text. This ReShade add-on draws that layer as the closest 16:9 picture, centred
(1920x1080 inside 2560x1080), unstretched. The 3D scene (venue, band, highway) keeps the full width.

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

KNOWN LIMITS (v0.1 beta, not a final build)
  - Menu backgrounds and full-screen overlays are drawn at 16:9 too, so menus show the 3D scene at the sides.
  - Some loading screens are left stretched (they don't use the scene composite the add-on keys on).
  - At 16:9 or narrower the add-on does nothing.

SETTINGS (ReShade.ini, section [GHWTDE_ULTRAWIDE])
  Enabled=1
  CompositePS=DE796938   (pixel shader hash of the game's scene composite; only change if a DE update breaks it)

CREDITS
  ReShade by crosire (BSD-3-Clause, license included). Dear ImGui by Omar Cornut (MIT).
