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
// Settings (ReShade.ini, section [GHWTDE_ULTRAWIDE]): Enabled=1, CompositePS=DE796938. The overlay's add-on tab
// has a checkbox and the number of UI draws mapped in the last frame.

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <d3d9.h>

#include <cstdio>
#include <mutex>
#include <unordered_map>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "GHWT:DE Ultrawide Fix";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"Draws the 2D layer (HUD, menus) of Guitar Hero World Tour: Definitive Edition as a centred 16:9 picture on "
	"ultrawide screens; the 3D scene keeps the full width.";

namespace
{
	bool g_enabled = true;
	uint32_t g_composite_ps = 0xDE796938;

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
	D3DVIEWPORT9 g_vp_set = {};        // the last viewport / scissor we set (re-map only when the game changed it)
	bool g_vp_valid = false;
	RECT g_sc_set = {};
	bool g_sc_valid = false;
	uint32_t g_mapped = 0, g_mapped_last = 0;
	bool g_logged = false;

	IDirect3DDevice9 *native(command_list *cmd_list)
	{
		return reinterpret_cast<IDirect3DDevice9 *>(cmd_list->get_device()->get_native());
	}

	void map_ui(command_list *cmd_list)
	{
		if (!g_enabled || !g_ui_phase || g_runtime == nullptr || g_cur_rt0 != g_runtime->get_current_back_buffer().handle)
			return;
		uint32_t bw = 0, bh = 0;
		g_runtime->get_screenshot_width_and_height(&bw, &bh);
		if (bh == 0 || bw * 9 <= bh * 16 + 9)
			return;                                       // 16:9 or narrower
		const float w169 = bh * 16.0f / 9.0f, k = w169 / bw, ox = (bw - w169) * 0.5f;
		IDirect3DDevice9 *dev = native(cmd_list);

		DWORD scissor = FALSE;
		if (SUCCEEDED(dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor)) && scissor)
		{
			RECT r;
			if (SUCCEEDED(dev->GetScissorRect(&r)) && !(g_sc_valid && memcmp(&r, &g_sc_set, sizeof r) == 0))
			{
				r.left = static_cast<LONG>(ox + r.left * k + 0.5f);
				r.right = static_cast<LONG>(ox + r.right * k + 0.5f);
				dev->SetScissorRect(&r);
				g_sc_set = r;
				g_sc_valid = true;
			}
		}

		D3DVIEWPORT9 vp = {};
		if (FAILED(dev->GetViewport(&vp)) || (g_vp_valid && memcmp(&vp, &g_vp_set, sizeof vp) == 0))
			return;
		D3DVIEWPORT9 m = vp;
		m.X = static_cast<DWORD>(ox + vp.X * k + 0.5f);
		m.Width = static_cast<DWORD>(vp.Width * k + 0.5f);
		dev->SetViewport(&m);
		g_vp_set = m;
		g_vp_valid = true;
		++g_mapped;
		if (!g_logged)
		{
			g_logged = true;
			char msg[160];
			sprintf_s(msg, "ghwtde_ultrawide: back buffer %ux%u, UI drawn at x %.0f, width %.0f", bw, bh, ox, w169);
			reshade::log::message(reshade::log::level::info, msg);
		}
	}

	void on_draw_common(command_list *cmd_list)
	{
		map_ui(cmd_list);
		if (g_cur_ps == g_composite_ps && g_runtime != nullptr && g_cur_rt0 == g_runtime->get_current_back_buffer().handle)
			g_ui_phase = true;                            // from the next draw on
	}

	bool on_draw(command_list *cmd_list, uint32_t, uint32_t, uint32_t, uint32_t)
	{
		on_draw_common(cmd_list);
		return false;
	}

	bool on_draw_indexed(command_list *cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
	{
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

	void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
	{
		g_ui_phase = false;
		g_vp_valid = false;
		g_sc_valid = false;
		g_mapped_last = g_mapped;
		g_mapped = 0;
	}

	void on_init_effect_runtime(effect_runtime *runtime)
	{
		g_runtime = runtime;
		reshade::get_config_value(runtime, "GHWTDE_ULTRAWIDE", "Enabled", g_enabled);
		char hex[16] = {};
		size_t n = sizeof hex;
		if (reshade::get_config_value(runtime, "GHWTDE_ULTRAWIDE", "CompositePS", hex, &n))
			g_composite_ps = static_cast<uint32_t>(strtoul(hex, nullptr, 16));
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
		ImGui::Text("UI draws mapped last frame: %u", g_mapped_last);
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(module))
			return FALSE;
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
		reshade::unregister_addon(module);
		break;
	}
	return TRUE;
}
