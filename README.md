# GHWT:DE Ultrawide Fix (v1.1)

An ASI plugin for **Guitar Hero World Tour: Definitive Edition** (32-bit, Direct3D 9) for screens wider than 16:9.
By **WitchDoctoR**.

- **3D (Hor+, 1.1):** the game keeps 16:9's horizontal field of view and crops the top and bottom on wider screens
  (the highway, band and venue look zoomed in and the highway runs off the bottom). The plugin keeps 16:9's vertical
  field of view instead and shows more at the sides, so the highway is its 16:9 size and lines up with the HUD.
- **2D:** the HUD, menus and text are drawn as the closest **16:9 picture, centred**, instead of stretched sideways.
- **No ReShade needed (1.1):** it hooks the game's own Direct3D 9 device, so it works with any ReShade version or
  preset (or none), GShade and DXVK. Loaded by an ASI loader (included).

**Download:** [`releases/GHWTDE-Ultrawide-Fix_1.1.zip`](releases/GHWTDE-Ultrawide-Fix_1.1.zip) or the
[Releases page](https://github.com/jgoa156/ghwtde-ultrawide-fix/releases/latest). Install instructions are in the
zip's README. Updating from 1.0: delete `ghwtde_ultrawide.addon32`.

## 3D field of view (1.1)

The engine builds its perspective frustums (camera projection `0x70d910`, a second one at `0x710b80`) as
`width = 2 near tan(fov/2)`, `height = width / aspect`. Each `tan()` call is redirected to a thunk that scales the
result by `aspect / (16/9)` on viewports wider than 16:9 (`HorPlus=1`; the call sites are byte-checked, so another
game build leaves them alone). The 2D hooks go in at `Direct3DCreate9`, after every ASI plugin has started, so the
GH5 / WoR HUD plugin still finds the original code it checks.

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
  group's mode. Loading screens are 16:9 with black side bars by default (`LoadingFill=1`: their movie, a
  `DrawPrimitiveUP` outside the sprite system, is covered through a hook on the device's `DrawPrimitiveUP`).

With `Catalog=1` (in `ghwtde_ultrawide.ini`) (off by default: it slows the game down with moving text), every new element is logged once to
`ghwtde_ultrawide_catalog.txt` (ids, textures, rects) for writing new rules.

## Build

Needs Visual Studio 2022+ (C++ x86 tools, Windows SDK).

```
build.bat          -> build\ghwtde_ultrawide.asi
python package.py  -> dist\ and releases\GHWTDE-Ultrawide-Fix_<version>.zip
```

## Known limits

- Face-off / battle HUDs are not adjusted yet.
- Screens without a rule stay a centred 16:9 picture.
- Also bundled with the GH5 / WoR HUD mod.

Related: [GH5 / WoR HUD mod](https://github.com/jgoa156/ghwtde-gh5wor-hud-mod).
