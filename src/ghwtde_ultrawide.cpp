// ghwtde_ultrawide: ReShade add-on for Guitar Hero World Tour: Definitive Edition (32-bit, Direct3D 9).
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
// Settings (ReShade.ini, section [GHWTDE_ULTRAWIDE]): see README.txt.

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <d3d9.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "GHWT:DE Ultrawide Fix";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"Draws the 2D layer (HUD, menus) of Guitar Hero World Tour: Definitive Edition as a centred 16:9 picture on "
	"ultrawide screens; full-screen backgrounds fill the screen and side panels pin to the screen edges.";

namespace
{
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
		// YOU ROCK screens (all four YouRockTheme options are 16:9 sprite layouts): the whole layout scales evenly
		// until it fills the width (sprites; texts stay centred)
		{ 0xa39d36f7, M_COVER },  // You_Rock: Metallica (you_rock_ghm) and Van Halen (you_rock_ghvh)
		{ 0xc5fe03f7, M_COVER },  // YouRock: Smash Hits (you_rock_ghshits)
		{ 0x427ea435, M_COVER },  // HandsOfGod: World Tour (you_rock)
	};
	const std::pair<uint32_t, TexRule> kDefaultTex[] = {
		{ 0x5049d8f2, { M_TILE, 0.58f, 1.0f, 0.04f } },   // title screen wall (logo baked in on the left half)
		{ 0xf8d1f74f, { M_STRETCH } },   // its dark overlay: stretched so its dim reaches the left edge and still covers the logo
		// song list (WTDE setlist): the two character strips grow (same scale both ways) from the screen edge to where
		// the centred picture puts their inner edge, so they fill the sides; the menu itself stays as designed
		{ 0x4b93dd1e, { M_FILL_LEFT } },
		{ 0xb19ce07d, { M_FILL_RIGHT } },
		// GH5 / WoR HUD mod's Warriors of Rock loading screen: the picture (WoR_HUD_loading, 2:1 art) covers the screen
		// at its own 2:1 shape even inside the loading container's bars group, anchored so the ring and the LOADING
		// plate stay whole (2560x1080: 120 px off the top, 80 off the bottom); its load wheel frames follow the same
		// mapping (kNamedFrames)
		{ 0x468df16a, { M_COVER, 0, 1, 0, 0.6f, 2.0f } },
	};
	// textures named <prefix><n>, n = 0..count-1 (animation frames), sharing one rule
	struct NamedFrames { const char *prefix; int count; TexRule rule; };
	const NamedFrames kNamedFrames[] = {
		{ "WoR_HUD_load_wheel_", 32, { M_COVER, 0, 1, 0, 0.6f, 2.0f } },   // WoR loading screen: scrolling load wheel
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
	bool g_auto_anchor = false;
	bool g_anchor_in_hud = false;
	bool g_catalog_on = true;
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

	effect_runtime *g_runtime = nullptr;
	uint32_t g_cur_ps = 0;
	uint64_t g_cur_rt0 = 0;
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
	// float2 uv) that the add-on can't reach through ReShade's events. The real device's DrawPrimitiveUP is hooked:
	// when map_ui flags that draw, its quad's height is scaled by the cover factor (its width already spans the
	// screen, the draw gets the full viewport), so the movie fills the screen without distortion.
	using DrawUpFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
	DrawUpFn g_up_orig = nullptr;
	bool g_up_hooked = false, g_cover_up_pending = false, g_up_logged = false;
	float g_cover_scale = 1;
	uint32_t g_movie_screen_frame = 0x80000000u, g_movie_covered_frame = 0x80000000u;

	HRESULT __stdcall movie_up_hook(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT prims, const void *data, UINT stride)
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
				reshade::log::message(reshade::log::level::info, msg);
			}
			return g_up_orig(dev, type, prims, buf, stride);
		}
		g_cover_up_pending = false;
		return g_up_orig(dev, type, prims, data, stride);
	}

	void hook_draw_up(IDirect3DDevice9 *dev)
	{
		g_up_hooked = true;
		void **vt = *reinterpret_cast<void ***>(dev);
		DWORD old = 0;
		if (!VirtualProtect(&vt[83], sizeof(void *), PAGE_READWRITE, &old))
			return;
		g_up_orig = reinterpret_cast<DrawUpFn>(vt[83]);           // IDirect3DDevice9::DrawPrimitiveUP
		vt[83] = reinterpret_cast<void *>(&movie_up_hook);
		VirtualProtect(&vt[83], sizeof(void *), old, &old);
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

	// =========================================================================================================
	// D3D side
	// =========================================================================================================

	IDirect3DDevice9 *native(command_list *cmd_list)
	{
		return reinterpret_cast<IDirect3DDevice9 *>(cmd_list->get_device()->get_native());
	}

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

	void map_ui(command_list *cmd_list)
	{
		if (!g_enabled || !g_ui_phase || g_runtime == nullptr || g_cur_rt0 != g_runtime->get_current_back_buffer().handle)
			return;
		uint32_t bw = 0, bh = 0;
		g_runtime->get_screenshot_width_and_height(&bw, &bh);
		if (bh == 0 || bw * 9 <= bh * 16 + 9)
			return;                                       // 16:9 or narrower
		const float fw = static_cast<float>(bw), fh = static_cast<float>(bh), w169 = fh * 16.0f / 9.0f;
		IDirect3DDevice9 *dev = native(cmd_list);
		if (!g_up_hooked)
			hook_draw_up(dev);
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
			if (group == M_BARS && rule != nullptr && rule->mode == M_COVER && ((full_w && full_h) || rule->aspect > 0))
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
			m = M_STRETCH;                                // full viewport; movie_up_hook scales the height
			g_cover_up_pending = true;
			g_movie_covered_frame = frame_now;
		}
		if (m == M_KEEP)
			m = M_CENTRE;

		apply_mode(dev, m, fw, w169);
		if (bars && g_movie_covered_frame != frame_now)
			clear_bars(dev, bw, bh, w169);              // no covered movie this frame: keep the sides black
		if (g_cur_has_info && g_cur_info.dump_trigger)
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
			reshade::log::message(reshade::log::level::info, msg);
		}
	}

	void on_draw_common(command_list *cmd_list)
	{
		map_ui(cmd_list);
		if (g_cur_ps == g_composite_ps && g_runtime != nullptr && g_cur_rt0 == g_runtime->get_current_back_buffer().handle)
			g_ui_phase = true;                            // from the next draw on
	}

	bool on_draw(command_list *cmd_list, uint32_t vertices, uint32_t, uint32_t, uint32_t)
	{
		g_draw_vertices = vertices, g_draw_indexed = false;
		on_draw_common(cmd_list);
		return false;
	}

	bool on_draw_indexed(command_list *cmd_list, uint32_t indices, uint32_t, uint32_t, int32_t, uint32_t)
	{
		g_draw_vertices = indices, g_draw_indexed = true;
		on_draw_common(cmd_list);
		return false;
	}

	void on_init_pipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline handle)
	{
		std::lock_guard<std::mutex> lock(g_map_mutex);
		for (uint32_t i = 0; i < count; ++i)
		{
			const pipeline_subobject &so = subobjects[i];
			if (so.type == pipeline_subobject_type::pixel_shader && so.data != nullptr)
			{
				const auto *desc = static_cast<const shader_desc *>(so.data);
				g_shader_hash[handle.handle] = desc->code != nullptr ? fnv1a(desc->code, desc->code_size) : 0;
			}
		}
	}

	void on_destroy_pipeline(device *, pipeline handle)
	{
		std::lock_guard<std::mutex> lock(g_map_mutex);
		g_shader_hash.erase(handle.handle);
	}

	void on_bind_pipeline(command_list *, pipeline_stage stages, pipeline handle)
	{
		if ((stages & pipeline_stage::pixel_shader) != pipeline_stage::pixel_shader)
			return;
		std::lock_guard<std::mutex> lock(g_map_mutex);
		const auto it = g_shader_hash.find(handle.handle);
		g_cur_ps = it != g_shader_hash.end() ? it->second : 0;
	}

	void on_bind_render_targets(command_list *, uint32_t count, const resource_view *rtvs, resource_view)
	{
		g_cur_rt0 = count != 0 ? rtvs[0].handle : 0;
	}

	void prune(std::unordered_map<void *, uint32_t> &m, uint32_t now)
	{
		for (auto it = m.begin(); it != m.end();)
			it = now - it->second > 600 ? m.erase(it) : std::next(it);
	}

	void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
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
			reshade::log::message(reshade::log::level::info, "ghwtde_ultrawide: dumped one frame's 2D draws");
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
				uint32_t bw = 0, bh = 0;
				if (g_runtime != nullptr)
					g_runtime->get_screenshot_width_and_height(&bw, &bh);
				fprintf(g_dump, "\n==== frame %u, %02d:%02d:%02d, %ux%u, hooks %s ====\n", now, st.wHour, st.wMinute, st.wSecond, bw, bh,
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

	// ---- settings ----
	Mode parse_mode(const char *s, Mode fallback)
	{
		for (int i = 0; i < M_COUNT; ++i)
			if (_stricmp(s, kModeNames[i]) == 0)
				return static_cast<Mode>(i);
		return fallback;
	}

	bool read_string(effect_runtime *runtime, const char *key, char *buf, size_t size)
	{
		size_t n = size;
		return reshade::get_config_value(runtime, "GHWTDE_ULTRAWIDE", key, buf, &n);
	}

	// "ID[:a:b], ID[:a:b], ..." (hex ids; a/b = band for slice / tile)
	template <typename F> void for_each_item(effect_runtime *runtime, const char *key, F f)
	{
		char buf[4096] = {};
		if (!read_string(runtime, key, buf, sizeof buf))
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

	void read_tex(effect_runtime *runtime, const char *key, Mode m)
	{
		for_each_item(runtime, key, [m](uint32_t id, float a, float b, float c) {
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

	void read_ids(effect_runtime *runtime, const char *key, Mode m)
	{
		for_each_item(runtime, key, [m](uint32_t id, float, float, float) { g_id_modes[id] = m; });
	}

	void read_mode(effect_runtime *runtime, const char *key, Mode &m)
	{
		char buf[32] = {};
		if (read_string(runtime, key, buf, sizeof buf))
			m = parse_mode(buf, m);
	}

	void on_init_effect_runtime(effect_runtime *runtime)
	{
		g_runtime = runtime;
		reshade::get_config_value(runtime, "GHWTDE_ULTRAWIDE", "Enabled", g_enabled);
		reshade::get_config_value(runtime, "GHWTDE_ULTRAWIDE", "ElementModes", g_element_modes);
		reshade::get_config_value(runtime, "GHWTDE_ULTRAWIDE", "AutoAnchor", g_auto_anchor);
		reshade::get_config_value(runtime, "GHWTDE_ULTRAWIDE", "AnchorInHud", g_anchor_in_hud);
		reshade::get_config_value(runtime, "GHWTDE_ULTRAWIDE", "Catalog", g_catalog_on);
		reshade::get_config_value(runtime, "GHWTDE_ULTRAWIDE", "MinImageSize", g_min_image);
		read_mode(runtime, "FullScreenImages", g_full_image);
		read_mode(runtime, "FullWidth", g_full_other);
		char hex[16] = {};
		if (read_string(runtime, "CompositePS", hex, sizeof hex))
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
		read_tex(runtime, "Stretch", M_STRETCH);
		read_tex(runtime, "Cover", M_COVER);
		read_tex(runtime, "Slice", M_SLICE);
		read_tex(runtime, "Tile", M_TILE);
		read_tex(runtime, "Keep", M_KEEP);
		read_ids(runtime, "Left", M_LEFT);
		read_ids(runtime, "Right", M_RIGHT);
		read_ids(runtime, "Centre", M_CENTRE);
		read_ids(runtime, "Bars", M_BARS);
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		if (g_runtime == runtime)
			g_runtime = nullptr;
	}

	void draw_overlay(effect_runtime *runtime)
	{
		if (ImGui::Checkbox("Draw the 2D layer as centred 16:9", &g_enabled))
			reshade::set_config_value(runtime, "GHWTDE_ULTRAWIDE", "Enabled", g_enabled);
		if (ImGui::Checkbox("Per-element modes (backgrounds, side panels, loading bars)", &g_element_modes))
			reshade::set_config_value(runtime, "GHWTDE_ULTRAWIDE", "ElementModes", g_element_modes);
		if (ImGui::Checkbox("Experimental: pin any panel touching a screen edge", &g_auto_anchor))
			reshade::set_config_value(runtime, "GHWTDE_ULTRAWIDE", "AutoAnchor", g_auto_anchor);
		ImGui::Text("Hooks: %s", g_hook_status.c_str());
		ImGui::Text("UI draws last frame: %u (with element info %u, not centred %u)", g_mapped_last, g_known_last, g_special_last);
		ImGui::Text("Catalog: %u elements logged", g_catalog_lines);
		if (ImGui::Button("Dump this frame's 2D draws (Ctrl+Shift+U)"))
			g_dump_request = true;
		ImGui::TextUnformatted(g_dump_path);
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(module))
			return FALSE;
		{
			char exe[MAX_PATH] = {};
			GetModuleFileNameA(nullptr, exe, MAX_PATH);
			char *slash = strrchr(exe, '\\');
			if (slash != nullptr)
			{
				slash[1] = 0;
				sprintf_s(g_dump_path, "%sghwtde_ultrawide_dump.txt", exe);
				sprintf_s(g_catalog_path, "%sghwtde_ultrawide_catalog.txt", exe);
			}
		}
		install_game_hooks();
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::present>(on_present);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_overlay(nullptr, draw_overlay);
		break;
	case DLL_PROCESS_DETACH:
		if (g_catalog != nullptr)
			fclose(g_catalog);
		reshade::unregister_addon(module);
		break;
	}
	return TRUE;
}
