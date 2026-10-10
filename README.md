# GHWT:DE Ultrawide Fix (v0.2 beta)

A ReShade add-on for **Guitar Hero World Tour: Definitive Edition** (32-bit, Direct3D 9) that draws the game's
2D layer (HUD, menus, text) as the closest **16:9 picture, centred**, on ultrawide screens, instead of stretching
it sideways. The 3D scene keeps the full width. By **WitchDoctoR**.

**Beta, not a final build.** **Download:** [`releases/GHWTDE-Ultrawide-Fix_0.2-beta.zip`](releases/GHWTDE-Ultrawide-Fix_0.2-beta.zip). Install
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

Nothing changes at 16:9 or narrower, and frames without the composite are left alone.

### Per-element modes (0.2)

The engine draws its 2D list one object at a time (`NxXenon::sSprite` / `SText`, cloned for the renderer every
frame). The add-on wraps their draw methods (vtable slots) and the element transform update, so every UI draw knows
its screen element, ancestors and texture checksum. Rules then pick a mode per draw:

- **centre** (default), **left** / **right** (group pinned to a screen edge), **stretch** (full-screen gradients),
- **cover** / **tile** / **slice** / **span** / **fill-left/right** (sprite vertices edited: backgrounds fill the
  screen without distortion), **cover** for whole layouts (YOU ROCK screens), **bars** (black sides).
- Text glyphs are queued by the engine and drawn later, so each text is flushed inside its wrapper to draw with its
  group's mode. The loading screen's movie (a `DrawPrimitiveUP` outside the sprite system) is covered through a hook
  on the device's `DrawPrimitiveUP`.

Every new element is logged once to `ghwtde_ultrawide_catalog.txt` (ids, textures, rects) for writing new rules.

## Build

Needs Visual Studio 2022+ (C++ x86 tools, Windows SDK). Headers are vendored under `deps/`: the ReShade 6.x API
(`deps/reshade/include`, BSD-3) and Dear ImGui 1.92.5 (`deps/imgui`, MIT).

```
build.bat          -> build\ghwtde_ultrawide.addon32
python package.py  -> dist\ and releases\GHWTDE-Ultrawide-Fix_<version>.zip
```

## Known limits

- Face-off / battle HUDs are not adjusted yet.
- Screens without a rule stay a centred 16:9 picture.
- Also bundled with the GH5 / WoR HUD mod (optional ReShade folder).

Related: [GH5 / WoR HUD mod](https://github.com/jgoa156/ghwtde-gh5wor-hud-mod).
