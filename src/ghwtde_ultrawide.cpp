// ghwtde_ultrawide: ASI plugin for Guitar Hero World Tour: Definitive Edition (32-bit, Direct3D 9).
//
// 1.1: works on the game's own IDirect3DDevice9 (hooked when the game creates it), whichever d3d9.dll provides it:
// Windows' Direct3D 9, ReShade's d3d9.dll or DXVK. Every hook runs on the game's render thread, in step with the
// sprite / text draw hooks below (ReShade's draw events don't under DXVK: it records Vulkan work on its own thread),
// so ReShade is no longer needed. Loaded by an ASI loader (dinput8.dll); settings in ghwtde_ultrawide.ini, log in
// ghwtde_ultrawide.log.
//
// On a screen wider than 16:9 the game stretches its whole 2D layer (HUD, menus, text) sideways. This add-on draws
// that layer as the closest 16:9 picture, centred (1920x1080 inside 2560x1080, for example), while the 3D scene
// keeps the full width.
//
// How: the game renders the scene off-screen and copies it to the back buffer with one full-screen draw (pixel
// shader hash CompositePS). Everything drawn into the back buffer after that draw is 2D UI, and the engine builds
// those vertices on the CPU in screen space, so the viewport decides where they land. Each UI draw gets its
// viewport (and scissor rectangle, which window elements clip with) mapped into the centred 16:9 area:
// x' = ox + x * k, width' = width * k, with k = (16/9 * height) / width and ox = (width - 16/9 * height) / 2.
// At 16:9 or narrower nothing changes. Frames without the composite (some loading screens) are left alone.
//
// Per-element modes (0.2): the engine draws its 2D list one object at a time (NxXenon::sSprite / SText, cloned for
// the renderer each frame), each sprite with one DrawPrimitiveUP from a static vertex buffer. The add-on wraps those
// draw methods, so every UI draw knows its sprite or text and, through the element transform update, the screen
// element that owns it (sprite element +0x218 -> Nx::CSprite +0x24; text element +0x214 / shadow +0x218 ->
// Nx::CXboxText +0x5c). Then:
//   - full-screen sprites (gradients, vignettes, dimmers) stretch to the full width; full-screen images (large
//     textures) cover the screen; per texture, an image can instead be sliced (one band of it absorbs the extra
//     width, the rest keeps its proportions) or tiled (a band repeats across, the rest stays centred);
//   - element groups listed by id (self + descendants) pin to the left / right screen edge (the select-mode and
//     instrument papers, the song intro), or get black side bars (the loading screen);
//   - the gameplay HUD (hud_root) is never re-anchored.
// Every new element seen is appended once to ghwtde_ultrawide_catalog.txt (ids, texture checksums, rects), and the
// overlay can dump one frame's 2D draws to ghwtde_ultrawide_dump.txt.
//
// Settings (ghwtde_ultrawide.ini next to the game, section [GHWTDE_ULTRAWIDE]; ReShade.ini's section is read as a
// fallback, as in 1.0): see README.txt.

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace
{
	char g_log_path[MAX_PATH] = {};

	void log_msg(const char *fmt, ...)
	{
		if (g_log_path[0] == 0)
			return;
		FILE *f = nullptr;
		if (fopen_s(&f, g_log_path, "a") != 0 || f == nullptr)
			return;
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(f, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
		va_list a;
		va_start(a, fmt);
		vfprintf(f, fmt, a);
		va_end(a);
		fputc('\n', f);
		fclose(f);
	}

	// ---- GHWT_Definitive.exe (image base 0x400000) ----
	constexpr uintptr_t kVtSSprite = 0xa2ecd0;       // NxXenon::sSprite vtable: dtor, clone, -, begin, draw, end
	constexpr uintptr_t kVtSText = 0xa30274;         // NxXenon::SText vtable, same layout
	constexpr uintptr_t kSSpriteFns[] = { 0x66e720, 0x66e610, 0x66e1c0 };   // expected dtor, clone, draw
	constexpr uintptr_t kSTextFns[] = { 0x6b7dd0, 0x6b7850, 0x6b6780 };
	constexpr uintptr_t kSpriteVerts = 0xef1f98;     // static vertex buffer sSprite::Draw fills (4 vertices, fan)
	constexpr size_t kSpriteFlags = 0xc;             // SDraw2D flags: bit 3 = 9-float vertices (stride 0x24, else 0x1c)
	constexpr size_t kSpriteTex = 0x30;              // sSprite: texture (u16 +0xe/+0x10 image size)
	constexpr size_t kVertU = 0x14;                  // vertex: x, y, z, w, colour, u, v
	constexpr size_t kTextPos = 0x3c;                // SText: float x, y
	constexpr uintptr_t kTransformUpdate = 0x5a05e0; // void __thiscall (element): world transform from the parent
	constexpr uint8_t kTransformPrologue[] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1 };   // sub esp,8; push esi; mov esi,ecx
	constexpr uintptr_t kVtSpriteElement = 0xa28474; // Front::CSpriteElement
	constexpr uintptr_t kVtTextElement = 0xa2bde4;   // Front::CTextElement (TextBlock lines are text elements too)
	constexpr uintptr_t kVtNxSprite = 0xa2a1a4;      // Nx::CSprite
	constexpr uintptr_t kVtXboxText = 0xa345cc;      // Nx::CXboxText
	constexpr size_t kSpriteElementNx = 0x218, kTextElementNx = 0x214, kTextElementShadow = 0x218;
	constexpr size_t kNxSpriteDraw = 0x24, kXboxTextDraw = 0x5c;
	constexpr size_t kElementId = 0x2c;              // element id checksum
	constexpr size_t kElementParent = 0x64;
	constexpr size_t kElementTexture = 0x214;        // sprite element: texture checksum
	constexpr uint32_t kHudRoot = 0x39155705;        // hud_root

	enum Kind : uint8_t { K_OTHER, K_SPRITE, K_TEXT };
	enum Mode : uint8_t { M_CENTRE, M_LEFT, M_RIGHT, M_STRETCH, M_COVER, M_SLICE, M_TILE, M_KEEP, M_BARS, M_SPAN, M_FILL_LEFT,
		M_FILL_RIGHT, M_COUNT };
	const char *const kModeNames[] = { "centre", "left", "right", "stretch", "cover", "slice", "tile", "keep", "bars", "span",
		"fillleft", "fillright" };

	struct TexRule
	{
		Mode mode = M_STRETCH;
		float a = 0, b = 1;                          // slice / tile band, as fractions of the image width
		float feather = 0;                           // tile: fade-in width of the kept part's left edge (fraction)
		float ay = 0.5f;                             // cover: the screen height fraction that stays put (0.5 = centred)
		float aspect = 0;                            // cover: the picture's own width/height (0 = the 16:9 canvas); with it
		                                             // the height becomes width/aspect, and every sprite with the rule
		                                             // (not only full-screen ones) follows the same mapping
	};

	// a texture rule that only applies under one element (any ancestor) and, optionally, to sprites at one canvas x
	// range (1280-wide canvas units, +-4)
	struct ScopedRule { uint32_t under, tex; float cx0, cx1; TexRule rule; };
	const ScopedRule kScoped[] = {
		{ 0, 0, 0, 0, { M_CENTRE } },   // none built in (span example: { 0x0cbda828, 0xbd30325b, 256, 1024, { M_SPAN } })
	};

	// built-in element groups (QB checksums of the element ids; README lists them)
	struct IdRule { uint32_t id; Mode mode; };
	const IdRule kDefaultIds[] = {
		{ 0x97dc36b4, M_LEFT },   // career / quickplay "select mode" paper (ui_game_mode), with its menu
		{ 0x29aa72ea, M_LEFT },   // its header ("Career", "Quickplay")
		{ 0x98fe6fdc, M_LEFT },   // "select instrument" paper (ui_select_instrument), with its menu
		{ 0x43823aca, M_LEFT },   // intro_container: song title / artist at the start of a song
		{ 0xaed7cd2e, M_BARS },   // loading_screen_container (bars unless its movie got covered, see movie_up_hook)
		// title screen flashes (ui_boot_iis: glow 5c54178d anchored at the bottom-right / bottom-left corner, scaled
		// 0.5..1 at random): pinned to their own screen edge, so they burst from the screen's corners, not the 16:9 ones
		{ 0x3415d542, M_RIGHT },
		{ 0xce1ae821, M_LEFT },
		// YOU ROCK screens (all four YouRockTheme options are 16:9 sprite layouts): the whole layout scales evenly
		// until it fills the width (sprites; texts stay centred)
		{ 0xa39d36f7, M_COVER },  // You_Rock: Metallica (you_rock_ghm) and Van Halen (you_rock_ghvh)
		{ 0xc5fe03f7, M_COVER },  // YouRock: Smash Hits (you_rock_ghshits)
		{ 0x427ea435, M_COVER },  // HandsOfGod: World Tour (you_rock)
		{ 0x6139b417, M_COVER },  // you_rock_physics: the exploding YOU ROCK text's container (WoR theme's glow, explosion)
		{ 0x32cc87b4, M_COVER },  // exploding_particle_container: the sparks around the exploding text
		{ 0x881079ee, M_COVER },  // WoR_HUD_yourock_fx: GH5 / WoR HUD mod's WoR YOU ROCK lightning bolts
	};
	const std::pair<uint32_t, TexRule> kDefaultTex[] = {
		{ 0x5049d8f2, { M_TILE, 0.58f, 1.0f, 0.04f } },   // title screen wall (logo baked in on the left half)
		{ 0xf8d1f74f, { M_STRETCH } },   // its dark overlay: stretched so its dim reaches the left edge and still covers the logo
		// song list (WTDE setlist): the two character strips grow (same scale both ways) from the screen edge to where
		// the centred picture puts their inner edge, so they fill the sides; the menu itself stays as designed
		{ 0x4b93dd1e, { M_FILL_LEFT } },
		{ 0xb19ce07d, { M_FILL_RIGHT } },
		// GH5 / WoR HUD mod's Warriors of Rock loading screen: the picture (WoR_HUD_loading) covers the screen at WoR's
		// 16:9 shape even inside the loading container's bars group, anchored so its ring stays whole at the top; the
		// LOADING plate is its own sprite, anchored to sit whole at the bottom (both can't fit at that shape on 21:9);
		// the load wheel follows the picture (kNamedFrames)
		{ 0x468df16a, { M_COVER, 0, 1, 0, 0.298f, 16.0f / 9.0f } },
	};
	// textures named <prefix><n>, n = 0..count-1 (animation frames), sharing one rule
	struct NamedFrames { const char *prefix; int count; TexRule rule; };
	const NamedFrames kNamedFrames[] = {
		{ "WoR_HUD_load_wheel_", 64, { M_COVER, 0, 1, 0, 0.298f, 16.0f / 9.0f } },   // WoR loading screen: load wheel (with the picture)
		{ "WoR_HUD_load_plate_", 64, { M_COVER, 0, 1, 0, 0.650f, 16.0f / 9.0f } },   // its LOADING plate: anchored to sit whole at the bottom
	};

	// QB checksum of a name (the game's texture ids): ~CRC-32 of the lower-cased text
	uint32_t qbkey(const char *s)
	{
		uint32_t c = 0xffffffffu;
		for (; *s; ++s)
		{
			c ^= static_cast<uint8_t>(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
			for (int k = 0; k < 8; ++k)
				c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
		return c;   // ~(~crc) : the game's key is the CRC register without the final inversion
	}

	// ---- settings ----
	bool g_enabled = true;
	bool g_element_modes = true;
	bool g_loading_fill = false;                     // loading screens: false = 16:9 with black side bars, true = fill the width
	bool g_auto_anchor = false;
	bool g_anchor_in_hud = false;
	bool g_catalog_on = false;                       // developer log (a line + flush per new element: stutters with moving text)
	Mode g_full_image = M_COVER;                     // full-screen sprite with a large texture
	Mode g_full_other = M_STRETCH;                   // other full-width sprites (gradients, dimmers, bands)
	int g_min_image = 512;                           // texture size (both sides) that makes a full-screen sprite an image
	uint32_t g_composite_ps = 0xDE796938;
	std::unordered_map<uint32_t, TexRule> g_tex_rules;   // texture checksum -> mode
	std::unordered_map<uint32_t, Mode> g_id_modes;       // element id (self or ancestor) -> group mode

	// ---- shader tracking (render thread) ----
	std::mutex g_map_mutex;
	std::unordered_map<uint64_t, uint32_t> g_shader_hash;   // shader object -> FNV-1a of its bytecode

	uint32_t fnv1a(const void *data, size_t size)
	{
		uint32_t h = 2166136261u;
		for (size_t i = 0; i < size; ++i)
			h = (h ^ static_cast<const uint8_t *>(data)[i]) * 16777619u;
		return h;
	}

	uint32_t g_cur_ps = 0;
	void *g_cur_rt0 = nullptr;          // render target 0 the game set (surface pointer)
	void *g_backbuffer = nullptr;       // the swap chain's back buffer (not AddRef'd; refreshed every Present)
	uint32_t g_bw = 0, g_bh = 0;        // its size
	bool g_ui_phase = false;           // the composite was drawn this frame: later back-buffer draws are UI
	D3DVIEWPORT9 g_vp_game = {}, g_vp_set = {};   // the game's viewport / the one we last set
	bool g_vp_ours = false;            // the device holds g_vp_set (else the game's own viewport)
	RECT g_sc_game = {}, g_sc_set = {};
	bool g_sc_ours = false;
	bool g_bars_done = false;          // side bars cleared this frame
	uint32_t g_mapped = 0, g_mapped_last = 0, g_known = 0, g_known_last = 0, g_special = 0, g_special_last = 0;
	bool g_logged = false;
	std::atomic<uint32_t> g_frame{ 0 };

	// ---- element links (main thread writes, render thread reads) ----
	struct Info
	{
		void *element = nullptr;
		uint32_t id = 0, tex = 0;
	};
	struct CloneInfo
	{
		void *orig = nullptr;
		Mode group = M_CENTRE;
		Info info;
		uint32_t chain[4] = {};        // ancestor ids, for the catalog / dump
		int scoped = -1;               // kScoped index whose element scope matched
		bool dump_trigger = false;     // under a screen whose untagged (movie) draw we want to see in a dump
	};
	// screens that play a full-screen movie through a draw the add-on can't tag: dumped once when they show up
	constexpr uint32_t kDumpTriggers[] = { 0xaed7cd2e /* loading_screen_container */, 0x04c9631b /* you_rock_container */ };
	std::mutex g_game_mutex;
	std::unordered_map<void *, Info> g_obj;              // original draw object -> owning element
	std::unordered_map<void *, CloneInfo> g_clone;       // renderer clone (or original) -> group mode
	std::unordered_map<void *, uint32_t> g_touch_left, g_touch_right;   // original draw object -> frame it touched an edge
	std::unordered_map<void *, uint32_t> g_group_left, g_group_right;   // element (group) -> frame
	bool g_hooks_ok = false;
	std::string g_hook_status = "not installed";

	// the draw object being drawn right now (render thread)
	Kind g_cur_kind = K_OTHER;
	void *g_cur_self = nullptr;
	CloneInfo g_cur_info;
	bool g_cur_has_info = false;

	// ---- dump / catalog ----
	bool g_dump_request = false, g_key_down = false;
	FILE *g_dump = nullptr;
	uint32_t g_dump_index = 0;
	char g_dump_path[MAX_PATH] = {}, g_catalog_path[MAX_PATH] = {};
	FILE *g_catalog = nullptr;
	uint32_t g_draw_vertices = 0;
	bool g_draw_indexed = false;

	// The loading screen's background is a movie drawn with DrawPrimitiveUP (4 vertices: float4 position, colour,
	// float2 uv) drawn outside the sprite / text objects. In the device's DrawPrimitiveUP hook (movie_up below),
	// when map_ui flags that draw, its quad's height is scaled by the cover factor (its width already spans the
	// screen, the draw gets the full viewport), so the movie fills the screen without distortion.
	using DrawUpFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
	DrawUpFn g_up_orig = nullptr;
	bool g_cover_up_pending = false, g_up_logged = false;
	float g_cover_scale = 1;
	uint32_t g_movie_screen_frame = 0x80000000u, g_movie_covered_frame = 0x80000000u;

	HRESULT movie_up(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT prims, const void *data, UINT stride)
	{
		if (g_cover_up_pending && data != nullptr && prims == 2 && stride >= 28 && stride <= 64)
		{
			g_cover_up_pending = false;
			uint8_t buf[4 * 64];
			memcpy(buf, data, 4 * stride);
			float miny = 1e30f, maxy = -1e30f;
			for (int i = 0; i < 4; ++i)
			{
				const float y = *reinterpret_cast<const float *>(buf + i * stride + 4);
				miny = y < miny ? y : miny;
				maxy = y > maxy ? y : maxy;
			}
			const float cy = (miny + maxy) * 0.5f;
			for (int i = 0; i < 4; ++i)
			{
				float *y = reinterpret_cast<float *>(buf + i * stride + 4);
				*y = cy + (*y - cy) * g_cover_scale;
			}
			if (!g_up_logged)
			{
				g_up_logged = true;
				const float *v = static_cast<const float *>(data);
				char msg[220];
				sprintf_s(msg, "ghwtde_ultrawide: movie quad covered (x %.1f..%.1f, y %.1f..%.1f, w %.2f, stride %u, scale %.3f)",
					v[0], *reinterpret_cast<const float *>(static_cast<const uint8_t *>(data) + 2 * stride), miny, maxy, v[3], stride, g_cover_scale);
				log_msg("%s", msg);
			}
			return g_up_orig(dev, type, prims, buf, stride);
		}
		g_cover_up_pending = false;
		return g_up_orig(dev, type, prims, data, stride);
	}

	// the movie's vertex layout: float4 position at 0, D3DCOLOR at 16, float2 uv at 20
	bool is_movie_layout(IDirect3DDevice9 *dev)
	{
		IDirect3DVertexDeclaration9 *decl = nullptr;
		if (FAILED(dev->GetVertexDeclaration(&decl)) || decl == nullptr)
			return false;
		D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
		UINT n = 0;
		bool ok = SUCCEEDED(decl->GetDeclaration(el, &n)) && n >= 4 &&
			el[0].Offset == 0 && el[0].Type == D3DDECLTYPE_FLOAT4 && el[0].Usage == D3DDECLUSAGE_POSITION &&
			el[1].Offset == 16 && el[1].Type == D3DDECLTYPE_D3DCOLOR &&
			el[2].Offset == 20 && el[2].Type == D3DDECLTYPE_FLOAT2 && el[2].Usage == D3DDECLUSAGE_TEXCOORD;
		decl->Release();
		return ok;
	}
	uint32_t g_auto_dumps = 0, g_last_auto_dump = 0x80000000u;
	std::unordered_set<uint64_t> g_catalog_seen;
	uint32_t g_catalog_lines = 0;

	// =========================================================================================================
	// game hooks
	// =========================================================================================================

	using DrawFn = void(__fastcall *)(void *self, void *edx);
	using CloneFn = void *(__fastcall *)(void *self, void *edx);
	using DtorFn = void *(__fastcall *)(void *self, void *edx, unsigned flags);
	using TransformFn = void(__fastcall *)(void *element, void *edx);
	DrawFn g_sprite_draw = nullptr, g_text_draw = nullptr;
	CloneFn g_sprite_clone = nullptr, g_text_clone = nullptr;
	DtorFn g_sprite_dtor = nullptr, g_text_dtor = nullptr;
	TransformFn g_transform = nullptr;

	template <typename T> T rd(const void *p, size_t off) { return *reinterpret_cast<const T *>(static_cast<const uint8_t *>(p) + off); }

	// the 2D draw object behind an Nx object pointer field, or nullptr
	void *draw_of(void *element, size_t field, uintptr_t nx_vt, size_t draw_off, uintptr_t draw_vt)
	{
		void *nx = rd<void *>(element, field);
		if (nx == nullptr || rd<uintptr_t>(nx, 0) != nx_vt)
			return nullptr;
		void *d = rd<void *>(nx, draw_off);
		return d != nullptr && rd<uintptr_t>(d, 0) == draw_vt ? d : nullptr;
	}

	// the 2D draw objects (sSprite / SText, text shadow) of a sprite or text element; returns how many
	int draw_objects_of(void *element, void **out, uint32_t &tex)
	{
		int n = 0;
		__try
		{
			const uintptr_t vt = rd<uintptr_t>(element, 0);
			if (vt == kVtSpriteElement)
			{
				tex = rd<uint32_t>(element, kElementTexture);
				if (void *d = draw_of(element, kSpriteElementNx, kVtNxSprite, kNxSpriteDraw, kVtSSprite))
					out[n++] = d;
			}
			else if (vt == kVtTextElement)
			{
				if (void *d = draw_of(element, kTextElementNx, kVtXboxText, kXboxTextDraw, kVtSText))
					out[n++] = d;
				if (void *d = draw_of(element, kTextElementShadow, kVtXboxText, kXboxTextDraw, kVtSText))
					out[n++] = d;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
		return n;
	}

	// element and its ancestors (pointer + id), up to max; returns the count (0 if the element is gone)
	int read_chain(void *element, void **elems, uint32_t *ids, int max)
	{
		int n = 0;
		__try
		{
			for (void *e = element; e != nullptr && n < max; e = rd<void *>(e, kElementParent))
			{
				elems[n] = e;
				ids[n] = rd<uint32_t>(e, kElementId);
				++n;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
		return n;
	}

	void __fastcall transform_hook(void *element, void *)
	{
		g_transform(element, nullptr);
		if (!g_element_modes)
			return;
		void *d[2];
		Info info;
		const int n = draw_objects_of(element, d, info.tex);
		if (n == 0)
			return;
		info.element = element;
		void *e[1];
		uint32_t id[1];
		if (read_chain(element, e, id, 1) == 1)
			info.id = id[0];
		std::lock_guard<std::mutex> lock(g_game_mutex);
		for (int i = 0; i < n; ++i)
			g_obj[d[i]] = info;
	}

	bool recent(const std::unordered_map<void *, uint32_t> &m, void *key, uint32_t now)
	{
		const auto it = m.find(key);
		return it != m.end() && now - it->second <= 4;
	}

	// called on the main thread when the renderer copy of a draw object is made: works out its group mode
	void note_clone(void *orig, void *clone)
	{
		Info info;
		{
			std::lock_guard<std::mutex> lock(g_game_mutex);
			const auto it = g_obj.find(orig);
			if (it == g_obj.end())
				return;
			info = it->second;
		}
		void *elems[24];
		uint32_t ids[24];
		const int n = read_chain(info.element, elems, ids, 24);
		CloneInfo ci;
		ci.orig = orig;
		ci.info = info;
		for (int i = 1; i < n && i <= 4; ++i)
			ci.chain[i - 1] = ids[i];
		bool in_hud = false;
		Mode forced = M_COUNT;
		for (int i = 0; i < n; ++i)
		{
			for (uint32_t t : kDumpTriggers)
				ci.dump_trigger = ci.dump_trigger || ids[i] == t;
			in_hud = in_hud || ids[i] == kHudRoot;
			const auto f = g_id_modes.find(ids[i]);
			if (forced == M_COUNT && f != g_id_modes.end())
				forced = f->second;            // the nearest listed ancestor decides
		}
		for (int r = 0; r < static_cast<int>(sizeof kScoped / sizeof kScoped[0]) && ci.scoped < 0; ++r)
			if (kScoped[r].tex != 0 && kScoped[r].tex == info.tex)
				for (int i = 1; i < n; ++i)
					if (ids[i] == kScoped[r].under)
					{
						ci.scoped = r;
						break;
					}
		const bool may_anchor = !(in_hud && !g_anchor_in_hud);
		const uint32_t now = g_frame.load();
		{
			std::lock_guard<std::mutex> lock(g_game_mutex);
			if (forced != M_COUNT)
				ci.group = forced;
			else if (g_auto_anchor && may_anchor)
			{
				// experimental: a sprite touching an edge pins itself and its descendants
				if (recent(g_touch_left, orig, now))
					g_group_left[info.element] = now;
				if (recent(g_touch_right, orig, now))
					g_group_right[info.element] = now;
				for (int i = 0; i < n; ++i)
				{
					if (recent(g_group_left, elems[i], now))
					{
						ci.group = M_LEFT;
						break;
					}
					if (recent(g_group_right, elems[i], now))
					{
						ci.group = M_RIGHT;
						break;
					}
				}
			}
			g_clone[clone] = ci;
			if (clone != orig)
				g_clone[orig] = ci;   // in case the original itself gets drawn
		}
	}

	void forget(void *d)
	{
		std::lock_guard<std::mutex> lock(g_game_mutex);
		g_obj.erase(d);
		g_clone.erase(d);
		g_touch_left.erase(d);
		g_touch_right.erase(d);
	}

	void begin_draw(Kind kind, void *self)
	{
		g_cur_kind = kind;
		g_cur_self = self;
		std::lock_guard<std::mutex> lock(g_game_mutex);
		const auto it = g_clone.find(self);
		g_cur_has_info = it != g_clone.end();
		if (g_cur_has_info)
			g_cur_info = it->second;
	}

	void __fastcall sprite_draw_hook(void *self, void *)
	{
		begin_draw(K_SPRITE, self);
		g_sprite_draw(self, nullptr);
		g_cur_kind = K_OTHER;
		g_cur_has_info = false;
	}

	// SText::Draw only queues glyphs; the game draws the queue when it fills up or at the end of a run of texts
	// (EndDraw), so one draw call could mix texts of different groups. Flushing here draws each text's glyphs while
	// its group is known (the flush returns at once when nothing is queued).
	using FlushFn = void(__fastcall *)(void *self, void *edx);
	constexpr uintptr_t kTextFlush = 0x6b65e0;
	constexpr uint8_t kTextFlushPrologue[] = { 0x83, 0x3D, 0x98, 0x17, 0xF8, 0x00, 0x00 };   // cmp [0xf81798], 0
	bool g_text_flush = false;

	void __fastcall text_draw_hook(void *self, void *)
	{
		begin_draw(K_TEXT, self);
		g_text_draw(self, nullptr);
		if (g_text_flush)
			reinterpret_cast<FlushFn>(kTextFlush)(self, nullptr);
		g_cur_kind = K_OTHER;
		g_cur_has_info = false;
	}

	void *__fastcall sprite_clone_hook(void *self, void *)
	{
		void *c = g_sprite_clone(self, nullptr);
		if (c != nullptr && g_element_modes)
			note_clone(self, c);
		return c;
	}

	void *__fastcall text_clone_hook(void *self, void *)
	{
		void *c = g_text_clone(self, nullptr);
		if (c != nullptr && g_element_modes)
			note_clone(self, c);
		return c;
	}

	void *__fastcall sprite_dtor_hook(void *self, void *, unsigned flags)
	{
		forget(self);
		return g_sprite_dtor(self, nullptr, flags);
	}

	void *__fastcall text_dtor_hook(void *self, void *, unsigned flags)
	{
		forget(self);
		return g_text_dtor(self, nullptr, flags);
	}

	// 5-byte jmp at site to target; the first `len` bytes (whole instructions) move to a trampoline
	void *install_jmp(uintptr_t site, size_t len, void *target)
	{
		uint8_t *tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!tramp)
			return nullptr;
		memcpy(tramp, reinterpret_cast<const void *>(site), len);
		tramp[len] = 0xE9;
		*reinterpret_cast<int32_t *>(tramp + len + 1) = static_cast<int32_t>((site + len) - reinterpret_cast<uintptr_t>(tramp + len + 5));
		DWORD old = 0;
		if (!VirtualProtect(reinterpret_cast<void *>(site), len, PAGE_EXECUTE_READWRITE, &old))
			return nullptr;
		uint8_t *p = reinterpret_cast<uint8_t *>(site);
		p[0] = 0xE9;
		*reinterpret_cast<int32_t *>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
		for (size_t i = 5; i < len; ++i)
			p[i] = 0x90;
		VirtualProtect(p, len, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, len);
		return tramp;
	}

	template <typename Fn> void patch_slot(uintptr_t vt, int slot, Fn hook, Fn &orig)
	{
		uintptr_t *p = reinterpret_cast<uintptr_t *>(vt) + slot;
		DWORD old = 0;
		VirtualProtect(p, 4, PAGE_READWRITE, &old);
		orig = reinterpret_cast<Fn>(*p);
		*p = reinterpret_cast<uintptr_t>(hook);
		VirtualProtect(p, 4, old, &old);
	}

	void install_game_hooks()
	{
		HMODULE exe = GetModuleHandleW(nullptr);
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(exe, path, MAX_PATH);
		const wchar_t *name = wcsrchr(path, L'\\');
		name = name ? name + 1 : path;
		if (_wcsicmp(name, L"GHWT_Definitive.exe") != 0 || reinterpret_cast<uintptr_t>(exe) != 0x400000)
		{
			g_hook_status = "not GHWT_Definitive.exe: per-element modes off";
			return;
		}
		const uintptr_t *sv = reinterpret_cast<const uintptr_t *>(kVtSSprite), *tv = reinterpret_cast<const uintptr_t *>(kVtSText);
		const bool match = sv[0] == kSSpriteFns[0] && sv[1] == kSSpriteFns[1] && sv[4] == kSSpriteFns[2] &&
			tv[0] == kSTextFns[0] && tv[1] == kSTextFns[1] && tv[4] == kSTextFns[2] &&
			memcmp(reinterpret_cast<const void *>(kTransformUpdate), kTransformPrologue, sizeof kTransformPrologue) == 0;
		if (!match)
		{
			g_hook_status = "unexpected game build (or another mod hooks the 2D draw): per-element modes off";
			return;
		}
		g_transform = reinterpret_cast<TransformFn>(install_jmp(kTransformUpdate, sizeof kTransformPrologue, reinterpret_cast<void *>(&transform_hook)));
		if (g_transform == nullptr)
		{
			g_hook_status = "transform hook failed: per-element modes off";
			return;
		}
		patch_slot<DtorFn>(kVtSSprite, 0, &sprite_dtor_hook, g_sprite_dtor);
		patch_slot<CloneFn>(kVtSSprite, 1, &sprite_clone_hook, g_sprite_clone);
		patch_slot<DrawFn>(kVtSSprite, 4, &sprite_draw_hook, g_sprite_draw);
		patch_slot<DtorFn>(kVtSText, 0, &text_dtor_hook, g_text_dtor);
		patch_slot<CloneFn>(kVtSText, 1, &text_clone_hook, g_text_clone);
		patch_slot<DrawFn>(kVtSText, 4, &text_draw_hook, g_text_draw);
		g_text_flush = memcmp(reinterpret_cast<const void *>(kTextFlush), kTextFlushPrologue, sizeof kTextFlushPrologue) == 0;
		g_hooks_ok = true;
		g_hook_status = g_text_flush ? "installed" : "installed (text flush not found: text follows its group only per batch)";
	}

	// ---- 3D field of view (1.1): Hor+ ----
	// The engine builds its perspective frustums (camera projection 0x70d910, a second one at 0x710b80) as
	// width = 2 near tan(fov / 2), height = width / aspect: the horizontal FOV is fixed, so a screen wider than 16:9
	// loses picture at the top and bottom (the highway, the band and the venue look zoomed in, the highway runs off the
	// bottom of the screen and no longer matches the 16:9 HUD). Each tan() call is redirected to a thunk that scales its
	// result by aspect / (16/9) on viewports wider than 16:9: 16:9's vertical FOV is kept and the sides show more.
	constexpr uintptr_t kTanThunk = 0x7be1c4;                  // jmp MSVCR80!_CItan (x87: argument and result in st0)
	constexpr uintptr_t kTanCamera = 0x70dd00, kTanCamera2 = 0x710e5c;
	// fld [esp+14h] / call tan / fstp [esp+14h] / fld [esp+14h] / movss xmm1, [esp+20h] (the aspect)
	const uint8_t kTanCameraBytes[] = { 0xd9, 0x44, 0x24, 0x14, 0xe8, 0xbf, 0x04, 0x0b, 0x00, 0xd9, 0x5c, 0x24, 0x14,
	                                    0xd9, 0x44, 0x24, 0x14, 0xf3, 0x0f, 0x10, 0x4c, 0x24, 0x20 };
	// fld [esp+0Ch] / call tan / fstp [esp+0Ch] / fld [esp+0Ch] / cvtss2sd xmm1, [ebp+14h] (the aspect)
	const uint8_t kTanCamera2Bytes[] = { 0xd9, 0x44, 0x24, 0x0c, 0xe8, 0x63, 0xd3, 0x0a, 0x00, 0xd9, 0x5c, 0x24, 0x0c,
	                                     0xd9, 0x44, 0x24, 0x0c, 0xf3, 0x0f, 0x5a, 0x4d, 0x14 };
	// 1.2 (user 2026-10-27: "the ultrawide fix never really fixed the zoomed in band members"): three more projections
	// built the same way (tan(fov / 2) -> width, height = width / aspect), all with 16:9's vertical FOV now:
	// 0x7118d0 (a twin of 0x710b80, aspect at [ebp+14h]), 0x70d490 (4 callers, aspect float at [ebp+14h]) and 0x64aca0
	// (aspect = viewport width / height, [esp+20h] / [esp+24h])
	constexpr uintptr_t kTanCamera3 = 0x711bac, kTanCamera4 = 0x70d6dd, kTanCamera5 = 0x64ae7d;
	const uint8_t kTanCamera3Bytes[] = { 0xd9, 0x44, 0x24, 0x0c, 0xe8, 0x13, 0xc6, 0x0a, 0x00, 0xd9, 0x5c, 0x24, 0x0c,
	                                     0xd9, 0x44, 0x24, 0x0c, 0xf3, 0x0f, 0x5a, 0x4d, 0x14 };
	// fld [esp+14h] / call tan / fstp [esp+14h] / mov eax, [esi+100h] / fld [esp+14h] / mov esi, [esi+0fch] /
	// fld [esp+0ch] / movss xmm1, [ebp+14h] (the aspect)
	const uint8_t kTanCamera4Bytes[] = { 0xd9, 0x44, 0x24, 0x14, 0xe8, 0xe2, 0x0a, 0x0b, 0x00, 0xd9, 0x5c, 0x24, 0x14,
	                                     0x8b, 0x86, 0x00, 0x01, 0x00, 0x00, 0xd9, 0x44, 0x24, 0x14, 0x8b, 0xb6, 0xfc, 0x00,
	                                     0x00, 0x00, 0xd9, 0x44, 0x24, 0x0c, 0xf3, 0x0f, 0x10, 0x4d, 0x14 };
	// fld [esp+18h] / call tan / fstp [esp+18h] / mov eax, [esi+100h] / fld [esp+18h] / mov ecx, [esi+0fch] /
	// fld [esp+1ch] / cvtss2sd xmm1, [esp+20h] (viewport width) ... cvtss2sd xmm6, [esp+24h] (height)
	const uint8_t kTanCamera5Bytes[] = { 0xd9, 0x44, 0x24, 0x18, 0xe8, 0x42, 0x33, 0x17, 0x00, 0xd9, 0x5c, 0x24, 0x18,
	                                     0x8b, 0x86, 0x00, 0x01, 0x00, 0x00, 0xd9, 0x44, 0x24, 0x18, 0x8b, 0x8e, 0xfc, 0x00,
	                                     0x00, 0x00, 0xd9, 0x44, 0x24, 0x1c, 0xf3, 0x0f, 0x5a, 0x4c, 0x24, 0x20 };
	// which projections get Hor+ (bit 0 = 0x70dd00 ... bit 4 = 0x64ae7d). 1.2 with all five crashed the game at boot
	// (2026-10-27, 0xc0000005 at 0x4d7022, three runs): 0x70d6dd and 0x64ae7d stay off by default until one is cleared
	int g_hor_sites = 0x07;
	bool g_hor_plus = true;
	std::string g_fov_status = "not installed";
	float g_fov_seen[8] = {};
	std::atomic<int> g_fov_seen_n{0};

	float __cdecl hor_factor(float aspect)
	{
		constexpr float k169 = 16.0f / 9.0f;
		const bool wide = g_hor_plus && aspect > k169 * 1.001f && aspect < 10.0f;
		const int n = g_fov_seen_n.load();
		if (n < 8)
		{
			bool seen = false;
			for (int i = 0; i < n && !seen; ++i)
				seen = std::fabs(g_fov_seen[i] - aspect) < 0.001f;
			if (!seen)
			{
				g_fov_seen[n] = aspect;
				g_fov_seen_n = n + 1;
				log_msg("3D projection aspect %.4f: %s", aspect, wide ? "Hor+ (16:9 vertical FOV)" : "unchanged");
			}
		}
		return wide ? aspect / k169 : 1.0f;
	}

	__declspec(naked) void tan_camera_thunk()
	{
		__asm
		{
			mov eax, 7BE1C4h               // kTanThunk
			call eax                       // st0 = tan(fov / 2)
			sub esp, 4
			fstp dword ptr [esp]
			push dword ptr [esp + 28h]     // the caller's [esp+20h]: + return address + our slot
			call hor_factor
			add esp, 4
			fmul dword ptr [esp]
			add esp, 4
			ret
		}
	}

	__declspec(naked) void tan_camera2_thunk()
	{
		__asm
		{
			mov eax, 7BE1C4h               // kTanThunk
			call eax
			sub esp, 4
			fstp dword ptr [esp]
			push dword ptr [ebp + 14h]
			call hor_factor
			add esp, 4
			fmul dword ptr [esp]
			add esp, 4
			ret
		}
	}

	float __cdecl hor_factor_wh(float w, float h)
	{
		return h > 0.0f ? hor_factor(w / h) : 1.0f;
	}

	// 0x64aca0: the aspect is the viewport's width / height, the caller's [esp+20h] / [esp+24h]
	__declspec(naked) void tan_camera5_thunk()
	{
		__asm
		{
			mov eax, 7BE1C4h               // kTanThunk
			call eax
			sub esp, 4
			fstp dword ptr [esp]
			push dword ptr [esp + 2Ch]     // the caller's [esp+24h]: + return address + our slot
			push dword ptr [esp + 2Ch]     // the caller's [esp+20h] (one more push above it)
			call hor_factor_wh
			add esp, 8
			fmul dword ptr [esp]
			add esp, 4
			ret
		}
	}

	// the call at `call_site` (5 bytes, inside `expect` starting 4 bytes earlier) goes to `target`
	bool redirect_call(uintptr_t call_site, const uint8_t *expect, size_t len, void *target)
	{
		if (memcmp(reinterpret_cast<const void *>(call_site - 4), expect, len) != 0)
			return false;
		DWORD old = 0;
		if (!VirtualProtect(reinterpret_cast<void *>(call_site), 5, PAGE_EXECUTE_READWRITE, &old))
			return false;
		*reinterpret_cast<int32_t *>(call_site + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (call_site + 5));
		VirtualProtect(reinterpret_cast<void *>(call_site), 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(call_site), 5);
		return true;
	}

	void install_fov_hooks()
	{
		if (!g_hor_plus)
		{
			g_fov_status = "off (HorPlus=0)";
			return;
		}
		if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000)
		{
			g_fov_status = "not GHWT_Definitive.exe: off";
			return;
		}
		const int on = g_hor_sites;
		const bool a = (on & 1) && redirect_call(kTanCamera, kTanCameraBytes, sizeof kTanCameraBytes, reinterpret_cast<void *>(&tan_camera_thunk));
		const bool b = (on & 2) && redirect_call(kTanCamera2, kTanCamera2Bytes, sizeof kTanCamera2Bytes, reinterpret_cast<void *>(&tan_camera2_thunk));
		const bool c = (on & 4) && redirect_call(kTanCamera3, kTanCamera3Bytes, sizeof kTanCamera3Bytes, reinterpret_cast<void *>(&tan_camera2_thunk));
		const bool d = (on & 8) && redirect_call(kTanCamera4, kTanCamera4Bytes, sizeof kTanCamera4Bytes, reinterpret_cast<void *>(&tan_camera2_thunk));
		const bool e = (on & 16) && redirect_call(kTanCamera5, kTanCamera5Bytes, sizeof kTanCamera5Bytes, reinterpret_cast<void *>(&tan_camera5_thunk));
		const int n = a + b + c + d + e;
		char buf[96];
		sprintf_s(buf, "Hor+ on %d of 5 projections (%d%d%d%d%d, HorPlusSites=%d)", n, a, b, c, d, e, on);
		g_fov_status = n ? buf : "unexpected game build: Hor+ off";
	}

	// =========================================================================================================
	// D3D side
	// =========================================================================================================

	struct SpriteGeom
	{
		uint8_t *v = nullptr;
		size_t stride = 0x1c;
		float minx = 0, maxx = 0, miny = 0, maxy = 0;
		int tw = 0, th = 0;
	};

	bool sprite_geom(void *self, SpriteGeom &g)
	{
		__try
		{
			g.stride = (rd<uint32_t>(self, kSpriteFlags) >> 3 & 1) ? 0x24 : 0x1c;
			g.v = reinterpret_cast<uint8_t *>(kSpriteVerts);
			g.minx = g.miny = 1e30f;
			g.maxx = g.maxy = -1e30f;
			for (int i = 0; i < 4; ++i)
			{
				const float x = *reinterpret_cast<float *>(g.v + i * g.stride), y = *reinterpret_cast<float *>(g.v + i * g.stride + 4);
				g.minx = x < g.minx ? x : g.minx;
				g.maxx = x > g.maxx ? x : g.maxx;
				g.miny = y < g.miny ? y : g.miny;
				g.maxy = y > g.maxy ? y : g.maxy;
			}
			const uint8_t *tex = rd<const uint8_t *>(self, kSpriteTex);
			if (tex != nullptr)
				g.tw = rd<uint16_t>(tex, 0xe), g.th = rd<uint16_t>(tex, 0x10);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void text_pos(void *self, float &x, float &y)
	{
		__try
		{
			x = rd<float>(self, kTextPos), y = rd<float>(self, kTextPos + 4);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	bool same_vp(const D3DVIEWPORT9 &a, const D3DVIEWPORT9 &b) { return memcmp(&a, &b, sizeof a) == 0; }
	bool same_rc(const RECT &a, const RECT &b) { return memcmp(&a, &b, sizeof a) == 0; }

	// x placement of each mode: x' = off + x * scale
	void mode_xform(Mode m, float bw, float w169, float &off, float &scale)
	{
		const float k = w169 / bw, ox = (bw - w169) * 0.5f;
		switch (m)
		{
		case M_LEFT: off = 0, scale = k; break;
		case M_RIGHT: off = 2 * ox, scale = k; break;
		case M_STRETCH: case M_COVER: case M_SLICE: case M_TILE: case M_SPAN: case M_FILL_LEFT: case M_FILL_RIGHT:
			off = 0, scale = 1;
			break;
		default: off = ox, scale = k; break;
		}
	}

	void apply_mode(IDirect3DDevice9 *dev, Mode m, float bw, float w169)
	{
		float off, scale;
		mode_xform(m, bw, w169, off, scale);

		DWORD scissor = FALSE;
		if (SUCCEEDED(dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor)) && scissor)
		{
			RECT r;
			if (SUCCEEDED(dev->GetScissorRect(&r)))
			{
				if (!(g_sc_ours && same_rc(r, g_sc_set)))
					g_sc_game = r;            // the game set a new one
				RECT t = g_sc_game;
				t.left = static_cast<LONG>(off + g_sc_game.left * scale + 0.5f);
				t.right = static_cast<LONG>(off + g_sc_game.right * scale + 0.5f);
				if (!same_rc(t, r))
					dev->SetScissorRect(&t);
				g_sc_set = t;
				g_sc_ours = true;
			}
		}

		D3DVIEWPORT9 vp = {};
		if (FAILED(dev->GetViewport(&vp)))
			return;
		if (!(g_vp_ours && same_vp(vp, g_vp_set)))
			g_vp_game = vp;
		D3DVIEWPORT9 t = g_vp_game;
		t.X = static_cast<DWORD>(off + g_vp_game.X * scale + 0.5f);
		t.Width = static_cast<DWORD>(g_vp_game.Width * scale + 0.5f);
		if (!same_vp(t, vp))
			dev->SetViewport(&t);
		g_vp_set = t;
		g_vp_ours = true;
	}

	// black over the screen outside the centred 16:9 picture (once per frame)
	void clear_bars(IDirect3DDevice9 *dev, uint32_t bw, uint32_t bh, float w169)
	{
		if (g_bars_done)
			return;
		g_bars_done = true;
		const LONG ox = static_cast<LONG>((bw - w169) * 0.5f + 0.5f);
		DWORD scissor = FALSE;
		dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
		if (scissor)
			dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		D3DVIEWPORT9 full = { 0, 0, bw, bh, 0.0f, 1.0f };
		dev->SetViewport(&full);
		const D3DRECT r[2] = { { 0, 0, ox, static_cast<LONG>(bh) }, { static_cast<LONG>(bw) - ox, 0, static_cast<LONG>(bw), static_cast<LONG>(bh) } };
		dev->Clear(2, r, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
		if (g_vp_ours)
			dev->SetViewport(&g_vp_set);
		if (scissor)
			dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
	}

	// one piece of a full-screen sprite: the image's [f0, f1] (fractions of its width) drawn at screen x [x0, x1].
	// Vertices left of mid are the sprite's left corners. draw = issue it now (else leave it for the game's draw).
	void sprite_piece(IDirect3DDevice9 *dev, SpriteGeom &g, const uint8_t *orig, float mid, float ul, float ur,
		float x0, float x1, float f0, float f1, bool draw, float alpha0 = 1, float alpha1 = 1)
	{
		memcpy(g.v, orig, 4 * g.stride);
		for (int i = 0; i < 4; ++i)
		{
			float *x = reinterpret_cast<float *>(g.v + i * g.stride), *u = reinterpret_cast<float *>(g.v + i * g.stride + kVertU);
			const bool left = *x < mid;
			*x = left ? x0 : x1;
			*u = ul + (ur - ul) * (left ? f0 : f1);
			const float am = left ? alpha0 : alpha1;
			if (am < 1)
			{
				uint32_t *c = reinterpret_cast<uint32_t *>(g.v + i * g.stride + 0x10);   // ARGB
				*c = (*c & 0x00ffffffu) | static_cast<uint32_t>((*c >> 24) * am + 0.5f) << 24;
			}
		}
		if (draw)
			dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, g.v, static_cast<UINT>(g.stride));
	}

	// slice: [0,a] keeps its proportions at the left edge, [b,1] at the right edge, [a,b] stretches in between.
	// span: the same between the sprite's own edges once they are pinned to the screen's left / right edges.
	// tile: [a,b] repeats at its true proportions across the screen, mirrored every other copy so neighbours meet
	// seamlessly, anchored so the first copy continues the image right where [0,a] ends; [0,a] and [b,1] stay where
	// the centred picture puts them, [0,a]'s left edge fading in over the copies (feather).
	void draw_banded(IDirect3DDevice9 *dev, SpriteGeom &g, Mode m, const TexRule &rule, float fw, float w169)
	{
		uint8_t orig[4 * 0x24];
		memcpy(orig, g.v, 4 * g.stride);
		const float mid = (g.minx + g.maxx) * 0.5f;
		float ul = 0, ur = 1;
		for (int i = 0; i < 4; ++i)
		{
			const float x = *reinterpret_cast<const float *>(orig + i * g.stride), u = *reinterpret_cast<const float *>(orig + i * g.stride + kVertU);
			(x < mid ? ul : ur) = u;
		}
		const float k = w169 / fw, ox = (fw - w169) * 0.5f;
		const float tw = (g.maxx - g.minx) * k;               // the image's width at its true proportions
		const float a = rule.a, b = rule.b;
		if (m == M_SLICE || m == M_SPAN)
		{
			// span: the left edge pinned left, the right edge pinned right (left / right modes), stretched between
			const float x0 = m == M_SPAN ? g.minx * k : g.minx, x1 = m == M_SPAN ? 2 * ox + g.maxx * k : g.maxx;
			const float xa = x0 + a * tw, xb = x1 - (1 - b) * tw;
			if (a > 0)
				sprite_piece(dev, g, orig, mid, ul, ur, x0, xa, 0, a, true);
			if (b < 1)
				sprite_piece(dev, g, orig, mid, ul, ur, xb, x1, b, 1, true);
			sprite_piece(dev, g, orig, mid, ul, ur, xa, xb, a, b, false);
			return;
		}
		// tile
		const float band = (b - a) * tw, start = ox + g.minx * k, anchor = start + a * tw;
		if (band > 8)
		{
			const int j0 = static_cast<int>(std::floor((g.minx - anchor) / band)), j1 = static_cast<int>(std::ceil((g.maxx - anchor) / band));
			for (int j = j0; j < j1; ++j)
			{
				const float x0 = anchor + j * band, xs = x0 < g.minx ? g.minx : x0, xe = x0 + band > g.maxx ? g.maxx : x0 + band;
				const bool flip = (j & 1) != 0;
				const auto f = [&](float x) { const float t = (x - x0) / band; return flip ? b - (b - a) * t : a + (b - a) * t; };
				if (xe > xs)
					sprite_piece(dev, g, orig, mid, ul, ur, xs, xe, f(xs), f(xe), true);
			}
		}
		if (b < 1)
			sprite_piece(dev, g, orig, mid, ul, ur, start + b * tw, start + tw, b, 1, true);
		const float fe = rule.feather > 0 && rule.feather < a ? rule.feather : 0;
		if (fe > 0)
			sprite_piece(dev, g, orig, mid, ul, ur, start, start + fe * tw, 0, fe, true, 0, 1);
		sprite_piece(dev, g, orig, mid, ul, ur, start + fe * tw, start + a * tw, fe, a, false);
	}

	const TexRule *tex_rule(uint32_t tex)
	{
		const auto it = g_tex_rules.find(tex);
		return it != g_tex_rules.end() ? &it->second : nullptr;
	}

	void write_entry(FILE *f, const char *kind, Mode m, const SpriteGeom *g, float tx, float ty)
	{
		fprintf(f, "%-6s %-7s", kind, kModeNames[m]);
		if (g != nullptr)
			fprintf(f, " x %7.1f..%7.1f y %7.1f..%7.1f tex %4dx%-4d", g->minx, g->maxx, g->miny, g->maxy, g->tw, g->th);
		else if (g_cur_kind == K_TEXT)
			fprintf(f, " at %7.1f,%7.1f %30s", tx, ty, "");
		else
			fprintf(f, " %53s", "");
		if (g_cur_has_info)
			fprintf(f, " id %08x tex %08x group %-6s up %08x %08x %08x %08x", g_cur_info.info.id, g_cur_info.info.tex,
				kModeNames[g_cur_info.group], g_cur_info.chain[0], g_cur_info.chain[1], g_cur_info.chain[2], g_cur_info.chain[3]);
		fputc('\n', f);
	}

	// every element (id, texture, parent) once, as it first shows up
	void catalog(Mode m, const SpriteGeom *g, float tx, float ty)
	{
		if (!g_catalog_on || g_catalog_lines >= 20000 || g_catalog_path[0] == 0 || (!g_cur_has_info && g == nullptr))
			return;
		const uint64_t key = g_cur_has_info
			? (static_cast<uint64_t>(g_cur_info.info.id) << 32 | g_cur_info.info.tex) ^
				(static_cast<uint64_t>(g_cur_info.chain[0]) * 0x9E3779B97F4A7C15ull) ^ g_cur_kind
			: (1ull << 63) ^ (static_cast<uint64_t>(g->tw) << 48) ^ (static_cast<uint64_t>(g->th) << 32) ^
				(static_cast<uint64_t>(static_cast<int>(g->minx) & 0xffff) << 16) ^ static_cast<uint64_t>(static_cast<int>(g->miny) & 0xffff);
		if (!g_catalog_seen.insert(key).second)
			return;
		if (g_catalog == nullptr && fopen_s(&g_catalog, g_catalog_path, "a") != 0)
		{
			g_catalog = nullptr;
			g_catalog_on = false;
			return;
		}
		fprintf(g_catalog, "f%-7u ", g_frame.load());
		write_entry(g_catalog, g_cur_kind == K_SPRITE ? "sprite" : "text", m, g, tx, ty);
		fflush(g_catalog);
		++g_catalog_lines;
	}

	void map_ui(IDirect3DDevice9 *dev)
	{
		if (!g_enabled || !g_ui_phase || g_backbuffer == nullptr || g_cur_rt0 != g_backbuffer)
			return;
		const uint32_t bw = g_bw, bh = g_bh;
		if (bh == 0 || bw * 9 <= bh * 16 + 9)
			return;                                       // 16:9 or narrower
		const float fw = static_cast<float>(bw), fh = static_cast<float>(bh), w169 = fh * 16.0f / 9.0f;
		g_cover_up_pending = false;
		g_cover_scale = fw / w169;

		Mode m = M_CENTRE;
		SpriteGeom g;
		bool have_geom = false, bars = false;
		const TexRule *rule = nullptr;
		float tx = 0, ty = 0;
		const bool modes = g_element_modes && g_hooks_ok;
		if (modes && g_cur_kind == K_SPRITE && (have_geom = sprite_geom(g_cur_self, g)) != false)
		{
			const float edge = fw * 0.004f;
			const bool full_w = g.minx <= edge && g.maxx >= fw - edge;
			const bool full_h = g.miny <= fh * 0.01f && g.maxy >= fh * 0.99f;
			const Mode group = g_cur_has_info ? g_cur_info.group : M_CENTRE;
			rule = tex_rule(g_cur_has_info ? g_cur_info.info.tex : 0);
			if (g_cur_has_info && g_cur_info.scoped >= 0)
			{
				const ScopedRule &sr = kScoped[g_cur_info.scoped];
				const float c = 1280.0f / fw;              // screen px -> canvas units
				if (std::fabs(g.minx * c - sr.cx0) <= 4 && std::fabs(g.maxx * c - sr.cx1) <= 4)
					rule = &sr.rule;
			}
			if (group == M_BARS && g_loading_fill && rule != nullptr && rule->mode == M_COVER && ((full_w && full_h) || rule->aspect > 0))
				m = M_COVER;                         // a full-screen picture with its own cover rule wins over the bars
			else if (group == M_BARS)
				m = M_CENTRE, bars = true;
			else if (group == M_COVER)
				m = M_COVER;                         // the whole layout scales evenly (x as stretched, y by the same)
			else if (rule != nullptr && (rule->mode == M_SPAN || rule->mode == M_FILL_LEFT || rule->mode == M_FILL_RIGHT))
				m = rule->mode;
			else if (rule != nullptr && (rule->mode == M_LEFT || rule->mode == M_RIGHT || rule->mode == M_KEEP || rule->mode == M_CENTRE))
				m = rule->mode == M_KEEP ? M_CENTRE : rule->mode;
			else if (full_w)
			{
				const bool image = full_h && g.tw >= g_min_image && g.th >= g_min_image / 2;
				m = rule != nullptr ? rule->mode : image ? g_full_image : g_full_other;
				if ((m == M_COVER || m == M_SLICE || m == M_TILE) && !full_h)
					m = M_STRETCH;                       // a full-width band: those need the full screen
			}
			else
			{
				m = group;
				if (g_auto_anchor && g_cur_has_info && g.maxx - g.minx < fw * 0.8f)
				{
					const uint32_t now = g_frame.load();
					std::lock_guard<std::mutex> lock(g_game_mutex);
					if (g.minx <= edge)
						g_touch_left[g_cur_info.orig] = now;
					if (g.maxx >= fw - edge)
						g_touch_right[g_cur_info.orig] = now;
				}
			}
		}
		else if (modes && g_cur_kind == K_TEXT)
		{
			m = g_cur_has_info ? g_cur_info.group : M_CENTRE;
			if (m == M_BARS)
				m = M_CENTRE, bars = true;
			if (m == M_COVER)
				m = M_CENTRE;                            // glyphs can't be rescaled here; text stays centred
			text_pos(g_cur_self, tx, ty);
		}
		const uint32_t frame_now = g_frame.load();
		if (bars)
			g_movie_screen_frame = frame_now;
		bool movie = false;
		if (modes && g_cur_kind == K_OTHER && g_up_orig != nullptr && !g_draw_indexed && g_draw_vertices == 4 &&
			frame_now - g_movie_screen_frame <= 30 && is_movie_layout(dev))
		{
			movie = true;
			if (g_loading_fill)
			{
				m = M_STRETCH;                            // full viewport; movie_up_hook scales the height
				g_cover_up_pending = true;
				g_movie_covered_frame = frame_now;
			}
			else
				m = M_CENTRE;                             // 16:9 in the middle; the bars keep the sides black
		}
		if (m == M_KEEP)
			m = M_CENTRE;

		apply_mode(dev, m, fw, w169);
		if (bars && g_movie_covered_frame != frame_now)
			clear_bars(dev, bw, bh, w169);              // no covered movie this frame: keep the sides black
		if (g_catalog_on && g_cur_has_info && g_cur_info.dump_trigger)
		{
			// one dump per movie screen (loading, YOU ROCK; up to 4 a session), to see what draws the movie
			const uint32_t now = g_frame.load();
			if (g_auto_dumps < 4 && g_dump == nullptr && now - g_last_auto_dump > 600)
				g_dump_request = true, g_last_auto_dump = now, ++g_auto_dumps;
		}
		++g_mapped;
		if (g_cur_has_info && g_cur_kind != K_OTHER)
			++g_known;
		if (m != M_CENTRE || bars || movie)
			++g_special;
		catalog(bars ? M_BARS : m, have_geom ? &g : nullptr, tx, ty);
		if (g_dump != nullptr && g_cur_kind == K_OTHER)
		{
			// how an untagged draw is set up (to find a way to reposition it, e.g. the loading screen's movie)
			DWORD fvf = 0;
			dev->GetFVF(&fvf);
			IDirect3DVertexBuffer9 *vb = nullptr;
			UINT off = 0, stride = 0;
			dev->GetStreamSource(0, &vb, &off, &stride);
			char decl_txt[160] = "-";
			IDirect3DVertexDeclaration9 *decl = nullptr;
			if (SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl != nullptr)
			{
				D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
				UINT n = 0;
				if (SUCCEEDED(decl->GetDeclaration(el, &n)))
				{
					int at = 0;
					for (UINT i = 0; i < n && el[i].Stream != 0xff && at < 150; ++i)
						at += sprintf_s(decl_txt + at, sizeof decl_txt - at, "%u:%u/t%u/u%u ", el[i].Stream, el[i].Offset, el[i].Type, el[i].Usage);
				}
				decl->Release();
			}
			uint32_t vs_hash = 0;
			IDirect3DVertexShader9 *vs = nullptr;
			if (SUCCEEDED(dev->GetVertexShader(&vs)) && vs != nullptr)
			{
				UINT size = 0;
				if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size > 0 && size < 65536)
				{
					uint8_t code[65536];
					if (SUCCEEDED(vs->GetFunction(code, &size)))
						vs_hash = fnv1a(code, size);
				}
				vs->Release();
			}
			float c[16] = {};
			dev->GetVertexShaderConstantF(0, c, 4);
			fprintf(g_dump, "     [%s %u] fvf %lx vb %p off %u stride %u decl %s vs %08x c0 %.3f %.3f %.3f %.3f c1 %.3f %.3f %.3f %.3f\n",
				g_draw_indexed ? "indexed" : "draw", g_draw_vertices, fvf, static_cast<void *>(vb), off, stride, decl_txt, vs_hash,
				c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
			if (vb != nullptr)
				vb->Release();
		}
		if (g_dump != nullptr)
		{
			fprintf(g_dump, "%4u ", g_dump_index++);
			write_entry(g_dump, g_cur_kind == K_SPRITE ? "sprite" : g_cur_kind == K_TEXT ? "text" : "other", bars ? M_BARS : m,
				have_geom ? &g : nullptr, tx, ty);
		}

		if (have_geom && m == M_COVER)
		{
			// uniform scale: full width, height grows the same around the rule's anchor (default: the middle); a rule
			// with the picture's own aspect makes the height width/aspect instead (never shorter than the screen)
			const bool own = rule != nullptr && rule->mode == M_COVER;
			float s = fw / w169;
			if (own && rule->aspect > 0)
				s = fw / rule->aspect / fh > 1.0f ? fw / rule->aspect / fh : 1.0f;
			const float cy = fh * (own ? rule->ay : 0.5f);
			for (int i = 0; i < 4; ++i)
			{
				float *y = reinterpret_cast<float *>(g.v + i * g.stride + 4);
				*y = cy + (*y - cy) * s;
			}
		}
		else if (have_geom && (m == M_SLICE || m == M_TILE || m == M_SPAN) && rule != nullptr)
			draw_banded(dev, g, m, *rule, fw, w169);
		else if (have_geom && (m == M_FILL_LEFT || m == M_FILL_RIGHT))
		{
			// fill left: from the screen's left edge to the sprite's centred right edge; fill right: from its centred
			// left edge to the screen's right edge. Height scales by the same factor around the screen's middle.
			const float k = w169 / fw, ox = (fw - w169) * 0.5f, mid = (g.minx + g.maxx) * 0.5f;
			const float x0 = m == M_FILL_LEFT ? 0 : ox + g.minx * k, x1 = m == M_FILL_LEFT ? ox + g.maxx * k : fw;
			const float sc = (x1 - x0) / ((g.maxx - g.minx) * k), cy = fh * 0.5f;
			for (int i = 0; i < 4; ++i)
			{
				float *x = reinterpret_cast<float *>(g.v + i * g.stride), *y = x + 1;
				*x = *x < mid ? x0 : x1;
				*y = cy + (*y - cy) * sc;
			}
		}

		if (!g_logged)
		{
			g_logged = true;
			char msg[200];
			sprintf_s(msg, "ghwtde_ultrawide: back buffer %ux%u, UI drawn at x %.0f, width %.0f; per-element modes: %s",
				bw, bh, (fw - w169) * 0.5f, w169, g_hook_status.c_str());
			log_msg("%s", msg);
		}
	}

	void prune(std::unordered_map<void *, uint32_t> &m, uint32_t now)
	{
		for (auto it = m.begin(); it != m.end();)
			it = now - it->second > 600 ? m.erase(it) : std::next(it);
	}

	void end_frame()
	{
		g_ui_phase = false;
		g_vp_ours = false;
		g_sc_ours = false;
		g_bars_done = false;
		g_mapped_last = g_mapped, g_known_last = g_known, g_special_last = g_special;
		g_mapped = g_known = g_special = 0;
		const uint32_t now = ++g_frame;

		if (g_dump != nullptr)
		{
			fclose(g_dump);
			g_dump = nullptr;
			log_msg("dumped one frame's 2D draws");
		}
		const bool key = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('U') & 0x8000);
		if (key && !g_key_down)
			g_dump_request = true;
		g_key_down = key;
		if (g_dump_request && g_dump_path[0] != 0)
		{
			g_dump_request = false;
			if (fopen_s(&g_dump, g_dump_path, "a") == 0 && g_dump != nullptr)
			{
				SYSTEMTIME st;
				GetLocalTime(&st);
				fprintf(g_dump, "\n==== frame %u, %02d:%02d:%02d, %ux%u, hooks %s ====\n", now, st.wHour, st.wMinute, st.wSecond, g_bw, g_bh,
					g_hook_status.c_str());
				g_dump_index = 0;
			}
			else
				g_dump = nullptr;
		}
		if (now % 600 == 0)
		{
			std::lock_guard<std::mutex> lock(g_game_mutex);
			prune(g_touch_left, now), prune(g_touch_right, now), prune(g_group_left, now), prune(g_group_right, now);
			if (g_clone.size() > 20000)
				g_clone.clear();
		}
	}

	// ---- settings (ghwtde_ultrawide.ini, else ReShade.ini's [GHWTDE_ULTRAWIDE] as in 1.0) ----
	char g_ini[MAX_PATH] = {}, g_ini_reshade[MAX_PATH] = {};

	bool read_string(const char *key, char *buf, size_t size)
	{
		for (const char *ini : { g_ini, g_ini_reshade })
		{
			if (ini[0] == 0)
				continue;
			GetPrivateProfileStringA("GHWTDE_ULTRAWIDE", key, "\x01", buf, static_cast<DWORD>(size), ini);
			if (!(buf[0] == 1 && buf[1] == 0))
				return true;
		}
		buf[0] = 0;
		return false;
	}

	void read_bool(const char *key, bool &v)
	{
		char buf[16];
		if (read_string(key, buf, sizeof buf))
			v = atoi(buf) != 0;
	}

	void read_int(const char *key, int &v)
	{
		char buf[16];
		if (read_string(key, buf, sizeof buf))
			v = atoi(buf);
	}

	Mode parse_mode(const char *s, Mode fallback)
	{
		for (int i = 0; i < M_COUNT; ++i)
			if (_stricmp(s, kModeNames[i]) == 0)
				return static_cast<Mode>(i);
		return fallback;
	}

	// "ID[:a:b], ID[:a:b], ..." (hex ids; a/b = band for slice / tile)
	template <typename F> void for_each_item(const char *key, F f)
	{
		char buf[4096] = {};
		if (!read_string(key, buf, sizeof buf))
			return;
		for (char *ctx = nullptr, *tok = strtok_s(buf, ", ;", &ctx); tok != nullptr; tok = strtok_s(nullptr, ", ;", &ctx))
		{
			char *rest = nullptr;
			const uint32_t id = static_cast<uint32_t>(strtoul(tok, &rest, 16));
			float a = -1, b = -1, c = 0;
			if (rest != nullptr && *rest == ':')
			{
				a = static_cast<float>(strtod(rest + 1, &rest));
				if (rest != nullptr && *rest == ':')
				{
					b = static_cast<float>(strtod(rest + 1, &rest));
					if (rest != nullptr && *rest == ':')
						c = static_cast<float>(strtod(rest + 1, &rest));
				}
			}
			f(id, a, b, c);
		}
	}

	void read_tex(const char *key, Mode m)
	{
		for_each_item(key, [m](uint32_t id, float a, float b, float c) {
			TexRule r;
			r.mode = m;
			r.feather = c;
			if (a >= 0 && b > a && b <= 1)
				r.a = a, r.b = b;
			else if (m == M_SLICE || m == M_TILE)
				r.a = 0.25f, r.b = 0.75f;
			g_tex_rules[id] = r;
		});
	}

	void read_ids(const char *key, Mode m)
	{
		for_each_item(key, [m](uint32_t id, float, float, float) { g_id_modes[id] = m; });
	}

	void read_mode(const char *key, Mode &m)
	{
		char buf[32] = {};
		if (read_string(key, buf, sizeof buf))
			m = parse_mode(buf, m);
	}

	void load_settings()
	{
		read_bool("Enabled", g_enabled);
		read_bool("ElementModes", g_element_modes);
		read_bool("LoadingFill", g_loading_fill);
		read_bool("AutoAnchor", g_auto_anchor);
		read_bool("AnchorInHud", g_anchor_in_hud);
		read_bool("Catalog", g_catalog_on);
		read_bool("HorPlus", g_hor_plus);
		read_int("HorPlusSites", g_hor_sites);
		read_int("MinImageSize", g_min_image);
		read_mode("FullScreenImages", g_full_image);
		read_mode("FullWidth", g_full_other);
		char hex[16] = {};
		if (read_string("CompositePS", hex, sizeof hex))
			g_composite_ps = static_cast<uint32_t>(strtoul(hex, nullptr, 16));
		g_tex_rules.clear();
		g_id_modes.clear();
		for (const auto &r : kDefaultTex)
			g_tex_rules[r.first] = r.second;
		for (const auto &f : kNamedFrames)
			for (int n = 0; n < f.count; ++n)
			{
				char name[96];
				sprintf_s(name, "%s%d", f.prefix, n);
				g_tex_rules[qbkey(name)] = f.rule;
			}
		for (const auto &r : kDefaultIds)
			g_id_modes[r.id] = r.mode;
		read_tex("Stretch", M_STRETCH);
		read_tex("Cover", M_COVER);
		read_tex("Slice", M_SLICE);
		read_tex("Tile", M_TILE);
		read_tex("Keep", M_KEEP);
		read_ids("Left", M_LEFT);
		read_ids("Right", M_RIGHT);
		read_ids("Centre", M_CENTRE);
		read_ids("Bars", M_BARS);
	}

	// =========================================================================================================
	// Direct3D 9 hooks: the game's IDirect3DDevice9 (vtable slots), found through its Direct3DCreate9 import
	// =========================================================================================================

	constexpr int kCreateDevice = 16;                                   // IDirect3D9
	constexpr int kReset = 16, kPresent = 17, kSetRenderTarget = 37,    // IDirect3DDevice9
		kDrawPrimitive = 81, kDrawIndexedPrimitive = 82, kDrawPrimitiveUP = 83, kDrawIndexedPrimitiveUP = 84,
		kCreatePixelShader = 106, kSetPixelShader = 107;
	using Create9Fn = IDirect3D9 *(WINAPI *)(UINT);
	using CreateDeviceFn = HRESULT(__stdcall *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
	using ResetFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
	using PresentFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
	using SetRTFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
	using DPFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
	using DIPFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	using DIPUPFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
	using CreatePSFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **);
	using SetPSFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
	Create9Fn g_create9 = nullptr;
	CreateDeviceFn g_create_device = nullptr;
	ResetFn g_reset = nullptr;
	PresentFn g_present = nullptr;
	SetRTFn g_set_rt = nullptr;
	DPFn g_dp = nullptr;
	DIPFn g_dip = nullptr;
	DIPUPFn g_dipup = nullptr;
	CreatePSFn g_create_ps = nullptr;
	SetPSFn g_set_ps = nullptr;
	void **g_d3d_vt = nullptr, **g_dev_vt = nullptr;
	bool g_in_ours = false;               // map_ui's own draws (sliced / tiled pieces) pass straight through
	bool g_composite_seen = false;
	std::unordered_map<void *, uint32_t> g_ps_hash;   // pixel shader -> FNV-1a of its bytecode (render thread)

	UINT prim_vertices(D3DPRIMITIVETYPE t, UINT n)
	{
		switch (t)
		{
		case D3DPT_POINTLIST: return n;
		case D3DPT_LINELIST: return n * 2;
		case D3DPT_LINESTRIP: return n + 1;
		case D3DPT_TRIANGLELIST: return n * 3;
		case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n + 2;
		default: return 0;
		}
	}

	void refresh_backbuffer(IDirect3DDevice9 *dev)
	{
		IDirect3DSurface9 *s = nullptr;
		g_backbuffer = nullptr;
		if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s)) && s != nullptr)
		{
			D3DSURFACE_DESC d = {};
			if (SUCCEEDED(s->GetDesc(&d)))
				g_bw = d.Width, g_bh = d.Height;
			g_backbuffer = s;
			s->Release();                 // the swap chain keeps it alive; only compared, never used
		}
	}

	void before_draw(IDirect3DDevice9 *dev, UINT vertices, bool indexed)
	{
		if (g_backbuffer == nullptr)
			refresh_backbuffer(dev);
		g_draw_vertices = vertices, g_draw_indexed = indexed;
		g_in_ours = true;
		map_ui(dev);
		g_in_ours = false;
		if (g_cur_ps == g_composite_ps && g_backbuffer != nullptr && g_cur_rt0 == g_backbuffer)
		{
			g_ui_phase = true;                // from the next draw on
			if (!g_composite_seen)
			{
				g_composite_seen = true;
				log_msg("scene composite found (pixel shader %08x): UI drawn after it is mapped", g_composite_ps);
			}
		}
	}

	HRESULT __stdcall dp_hook(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT start, UINT n)
	{
		if (!g_in_ours)
			before_draw(dev, prim_vertices(t, n), false);
		return g_dp(dev, t, start, n);
	}

	HRESULT __stdcall dip_hook(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, INT base, UINT minv, UINT nv, UINT start, UINT n)
	{
		if (!g_in_ours)
			before_draw(dev, prim_vertices(t, n), true);
		return g_dip(dev, t, base, minv, nv, start, n);
	}

	HRESULT __stdcall dpup_hook(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT n, const void *data, UINT stride)
	{
		if (g_in_ours)
			return g_up_orig(dev, t, n, data, stride);
		before_draw(dev, prim_vertices(t, n), false);
		return movie_up(dev, t, n, data, stride);
	}

	HRESULT __stdcall dipup_hook(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT minv, UINT nv, UINT n, const void *idx, D3DFORMAT fmt,
	                             const void *data, UINT stride)
	{
		if (!g_in_ours)
			before_draw(dev, prim_vertices(t, n), true);
		return g_dipup(dev, t, minv, nv, n, idx, fmt, data, stride);
	}

	HRESULT __stdcall set_rt_hook(IDirect3DDevice9 *dev, DWORD index, IDirect3DSurface9 *s)
	{
		if (index == 0)
			g_cur_rt0 = s;
		return g_set_rt(dev, index, s);
	}

	// shader bytecode size: version token, instructions, end token (0x0000ffff)
	size_t shader_size(const DWORD *code)
	{
		size_t n = 0;
		__try
		{
			while (n < 65536 && code[n] != 0x0000ffff)
				++n;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
		return n < 65536 ? (n + 1) * sizeof(DWORD) : 0;
	}

	HRESULT __stdcall create_ps_hook(IDirect3DDevice9 *dev, const DWORD *code, IDirect3DPixelShader9 **out)
	{
		const HRESULT hr = g_create_ps(dev, code, out);
		if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && code != nullptr)
		{
			const size_t size = shader_size(code);
			g_ps_hash[*out] = size ? fnv1a(code, size) : 0;
		}
		return hr;
	}

	HRESULT __stdcall set_ps_hook(IDirect3DDevice9 *dev, IDirect3DPixelShader9 *ps)
	{
		const auto it = g_ps_hash.find(ps);
		g_cur_ps = it != g_ps_hash.end() ? it->second : 0;
		return g_set_ps(dev, ps);
	}

	HRESULT __stdcall present_hook(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
	{
		const HRESULT hr = g_present(dev, src, dst, wnd, dirty);
		refresh_backbuffer(dev);
		g_cur_rt0 = g_backbuffer;             // a new frame starts on the back buffer
		end_frame();
		return hr;
	}

	HRESULT __stdcall reset_hook(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
	{
		g_backbuffer = g_cur_rt0 = nullptr;
		const HRESULT hr = g_reset(dev, pp);
		if (SUCCEEDED(hr))
			refresh_backbuffer(dev);
		return hr;
	}

	template <typename Fn> void hook_slot(void **vt, int slot, void *hook, Fn &orig)
	{
		DWORD old = 0;
		if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old))
			return;
		orig = reinterpret_cast<Fn>(vt[slot]);
		vt[slot] = hook;
		VirtualProtect(&vt[slot], sizeof(void *), old, &old);
	}

	void hook_device(IDirect3DDevice9 *dev)
	{
		void **vt = *reinterpret_cast<void ***>(dev);
		if (vt == g_dev_vt)
			return;                           // this device class is hooked already
		if (g_dev_vt != nullptr)
		{
			log_msg("a second device class showed up: left alone");
			return;
		}
		g_dev_vt = vt;
		hook_slot(vt, kReset, reinterpret_cast<void *>(&reset_hook), g_reset);
		hook_slot(vt, kPresent, reinterpret_cast<void *>(&present_hook), g_present);
		hook_slot(vt, kSetRenderTarget, reinterpret_cast<void *>(&set_rt_hook), g_set_rt);
		hook_slot(vt, kDrawPrimitive, reinterpret_cast<void *>(&dp_hook), g_dp);
		hook_slot(vt, kDrawIndexedPrimitive, reinterpret_cast<void *>(&dip_hook), g_dip);
		hook_slot(vt, kDrawPrimitiveUP, reinterpret_cast<void *>(&dpup_hook), g_up_orig);
		hook_slot(vt, kDrawIndexedPrimitiveUP, reinterpret_cast<void *>(&dipup_hook), g_dipup);
		hook_slot(vt, kCreatePixelShader, reinterpret_cast<void *>(&create_ps_hook), g_create_ps);
		hook_slot(vt, kSetPixelShader, reinterpret_cast<void *>(&set_ps_hook), g_set_ps);
		refresh_backbuffer(dev);
		g_cur_rt0 = g_backbuffer;
		log_msg("device hooked (back buffer %ux%u); per-element modes: %s", g_bw, g_bh, g_hook_status.c_str());
	}

	HRESULT __stdcall create_device_hook(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
	                                     D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
	{
		const HRESULT hr = g_create_device(d3d, adapter, type, wnd, flags, pp, out);
		if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
			hook_device(*out);
		return hr;
	}

	IDirect3D9 *WINAPI create9_hook(UINT sdk)
	{
		// The 2D hooks go in here, not in DllMain: by now every ASI plugin has started, and the GH5 / WoR HUD
		// plugin checks the original bytes of the transform update (0x5a05e0) at its start and stays inert if they
		// changed (the ASI loader starts ghwtde_ultrawide.asi first). 1.0 hooked at the same point, as a ReShade
		// add-on loaded with the Direct3D runtime.
		static bool game_hooks = false;
		if (!game_hooks)
		{
			game_hooks = true;
			install_game_hooks();
			log_msg("game hooks at Direct3DCreate9: per-element modes %s", g_hook_status.c_str());
		}
		IDirect3D9 *d = g_create9(sdk);
		if (d != nullptr)
		{
			void **vt = *reinterpret_cast<void ***>(d);
			if (g_d3d_vt == nullptr)
			{
				g_d3d_vt = vt;
				hook_slot(vt, kCreateDevice, reinterpret_cast<void *>(&create_device_hook), g_create_device);
			}
		}
		return d;
	}

	const char *d3d9_provider()
	{
		static char name[MAX_PATH];
		HMODULE m = GetModuleHandleA("d3d9.dll");
		if (m == nullptr || GetModuleFileNameA(m, name, MAX_PATH) == 0)
			return "d3d9.dll not loaded yet";
		return name;
	}

	// the game's import of d3d9!Direct3DCreate9 points at our hook
	bool hook_import()
	{
		uint8_t *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
		auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
		auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
		const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name != 0; ++imp)
		{
			if (_stricmp(reinterpret_cast<const char *>(base + imp->Name), "d3d9.dll") != 0)
				continue;
			auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
			auto *iat = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
			for (; names->u1.AddressOfData != 0; ++names, ++iat)
			{
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
					continue;
				const auto *by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
				if (strcmp(reinterpret_cast<const char *>(by_name->Name), "Direct3DCreate9") != 0)
					continue;
				DWORD old = 0;
				if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
					return false;
				g_create9 = reinterpret_cast<Create9Fn>(iat->u1.Function);
				iat->u1.Function = reinterpret_cast<uintptr_t>(&create9_hook);
				VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
				return true;
			}
		}
		return false;
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		char exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		char *slash = strrchr(exe, '\\');
		if (slash != nullptr)
		{
			slash[1] = 0;
			sprintf_s(g_dump_path, "%sghwtde_ultrawide_dump.txt", exe);
			sprintf_s(g_catalog_path, "%sghwtde_ultrawide_catalog.txt", exe);
			sprintf_s(g_log_path, "%sghwtde_ultrawide.log", exe);
			sprintf_s(g_ini, "%sghwtde_ultrawide.ini", exe);
			sprintf_s(g_ini_reshade, "%sReShade.ini", exe);
			FILE *f = nullptr;
			if (fopen_s(&f, g_log_path, "w") == 0 && f != nullptr)
				fclose(f);                    // a fresh log every start
		}
		load_settings();
		install_fov_hooks();
		// 1.0 was a ReShade add-on: with it still in the folder both would map the 2D layer (and hook the same draw
		// methods), so the 2D part stays off until it is deleted; the 3D field of view is ours alone either way
		char old_addon[MAX_PATH] = {};
		if (slash != nullptr)
			sprintf_s(old_addon, "%sghwtde_ultrawide.addon32", exe);
		if (old_addon[0] != 0 && GetFileAttributesA(old_addon) != INVALID_FILE_ATTRIBUTES)
		{
			log_msg("GHWT:DE Ultrawide Fix 1.2: 3D FOV %s; 2D layer OFF: delete the old ghwtde_ultrawide.addon32 (1.0) from the game folder",
				g_fov_status.c_str());
			return TRUE;
		}
		const bool ok = hook_import();
		log_msg("GHWT:DE Ultrawide Fix 1.2: %s (d3d9: %s); 3D FOV: %s", ok ? "Direct3DCreate9 hooked" : "Direct3DCreate9 import NOT found",
			d3d9_provider(), g_fov_status.c_str());
	}
	else if (reason == DLL_PROCESS_DETACH && g_catalog != nullptr)
		fclose(g_catalog);
	return TRUE;
}
