# GHWT:DE Ultrawide Fix (v0.1 beta)

A ReShade add-on for **Guitar Hero World Tour: Definitive Edition** (32-bit, Direct3D 9) that draws the game's
2D layer (HUD, menus, text) as the closest **16:9 picture, centred**, on ultrawide screens, instead of stretching
it sideways. The 3D scene keeps the full width. By **WitchDoctoR**.

**Beta, not a final build.** **Download:** [`releases/GHWTDE-Ultrawide-Fix_0.1-beta.zip`](releases/GHWTDE-Ultrawide-Fix_0.1-beta.zip). Install
instructions are in the zip's README.

## How it works

The game renders the 3D scene off-screen and copies it to the back buffer with one full-screen draw (the
"composite", recognised by its pixel shader hash). Everything drawn into the back buffer after it is 2D UI, and the
engine builds those vertices on the CPU in screen space, so the **viewport** decides where they land. For each UI
draw the add-on maps the viewport, and the scissor rectangle that window elements clip with, into the centred 16:9
area:

```
k  = (16/9 * height) / width        ox = (width - 16/9 * height) / 2
x' = ox + x * k                     width' = width * k
```

Nothing changes at 16:9 or narrower, and frames without the composite (some loading screens) are left alone.

## Build

Needs Visual Studio 2022+ (C++ x86 tools, Windows SDK). Headers are vendored under `deps/`: the ReShade 6.x API
(`deps/reshade/include`, BSD-3) and Dear ImGui 1.92.5 (`deps/imgui`, MIT).

```
build.bat          -> build\ghwtde_ultrawide.addon32
python package.py  -> dist\ and releases\GHWTDE-Ultrawide-Fix_<version>.zip
```

## Known limits

- Menu backgrounds and full-screen overlays are drawn at 16:9 as well (planned: full-width backgrounds and
  gradients, left-anchored panels pinned to the screen edge).
- Some loading screens stay stretched.

Related: [GH5 / WoR HUD mod](https://github.com/jgoa156/ghwtde-gh5wor-hud-mod).
