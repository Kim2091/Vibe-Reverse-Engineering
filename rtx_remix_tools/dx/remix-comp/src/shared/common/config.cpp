#include "std_include.hpp"
#include "config.hpp"

namespace shared::common
{
	config& config::get()
	{
		static config instance;
		return instance;
	}

	void config::load(const std::string& path)
	{
		ini_path_ = path;
		loaded_ = true;
		parse_all();
	}

	int config::get_int(const char* section, const char* key, int default_val) const
	{
		if (!loaded_) return default_val;
		return GetPrivateProfileIntA(section, key, default_val, ini_path_.c_str());
	}

	std::string config::get_string(const char* section, const char* key, const char* default_val) const
	{
		if (!loaded_) return default_val;
		char buf[512];
		GetPrivateProfileStringA(section, key, default_val, buf, sizeof(buf), ini_path_.c_str());
		return buf;
	}

	float config::get_float(const char* section, const char* key, float default_val) const
	{
		auto str = get_string(section, key, "");
		if (str.empty()) return default_val;
		try { return std::stof(str); }
		catch (...) { return default_val; }
	}

	bool config::get_bool(const char* section, const char* key, bool default_val) const
	{
		return get_int(section, key, default_val ? 1 : 0) != 0;
	}

	void config::parse_all()
	{
		// [Remix]
		remix.enabled = get_bool("Remix", "Enabled", true);
		remix.dll_name = get_string("Remix", "DLLName", "d3d9_remix.dll");

		// [Chain]
		chain.preload_dll = get_string("Chain", "PreloadDLL", "");

		// [FFP]
		ffp.enabled = get_bool("FFP", "Enabled", true);
		ffp.albedo_stage = get_int("FFP", "AlbedoStage", 0);
		if (ffp.albedo_stage < 0 || ffp.albedo_stage > 7)
			ffp.albedo_stage = 0;

		// [FFP.Registers]
		ffp.vs_reg_view_start = get_int("FFP.Registers", "ViewStart", 0);
		ffp.vs_reg_view_end = get_int("FFP.Registers", "ViewEnd", 4);
		ffp.vs_reg_proj_start = get_int("FFP.Registers", "ProjStart", 4);
		ffp.vs_reg_proj_end = get_int("FFP.Registers", "ProjEnd", 8);
		ffp.vs_reg_world_start = get_int("FFP.Registers", "WorldStart", 16);
		ffp.vs_reg_world_end = get_int("FFP.Registers", "WorldEnd", 20);
		ffp.vs_reg_bone_threshold = get_int("FFP.Registers", "BoneThreshold", 20);
		ffp.vs_regs_per_bone = get_int("FFP.Registers", "RegsPerBone", 3);
		ffp.vs_bone_min_regs = get_int("FFP.Registers", "BoneMinRegs", 3);

		// Validate register ranges (matrix starts index into vs_const_[256*4])
		auto clamp = [](int val, int lo, int hi, int def) { return (val >= lo && val <= hi) ? val : def; };
		ffp.vs_reg_view_start  = clamp(ffp.vs_reg_view_start,  0, 252, 0);
		ffp.vs_reg_view_end    = clamp(ffp.vs_reg_view_end,    0, 256, 4);
		ffp.vs_reg_proj_start  = clamp(ffp.vs_reg_proj_start,  0, 252, 4);
		ffp.vs_reg_proj_end    = clamp(ffp.vs_reg_proj_end,    0, 256, 8);
		ffp.vs_reg_world_start = clamp(ffp.vs_reg_world_start, 0, 252, 16);
		ffp.vs_reg_world_end   = clamp(ffp.vs_reg_world_end,   0, 256, 20);
		ffp.vs_reg_bone_threshold = clamp(ffp.vs_reg_bone_threshold, 0, 255, 20);
		ffp.vs_regs_per_bone   = clamp(ffp.vs_regs_per_bone,   1, 16,  3);
		ffp.vs_bone_min_regs   = clamp(ffp.vs_bone_min_regs,   1, 256, 3);

		// [Skinning]
		skinning.enabled = get_bool("Skinning", "Enabled", false);

		// [Diagnostics]
		diagnostics.enabled = get_bool("Diagnostics", "Enabled", true);
		diagnostics.auto_capture = get_bool("Diagnostics", "AutoCapture", true);
		diagnostics.delay_ms = get_int("Diagnostics", "DelayMs", 50000);
		diagnostics.log_frames = get_int("Diagnostics", "LogFrames", 3);
		diagnostics.log_draw_calls = get_bool("Diagnostics", "LogDrawCalls", true);
		diagnostics.log_vs_constants = get_bool("Diagnostics", "LogVSConstants", true);
		diagnostics.log_vertex_data = get_bool("Diagnostics", "LogVertexData", true);
		diagnostics.log_declarations = get_bool("Diagnostics", "LogDeclarations", true);
		diagnostics.log_textures = get_bool("Diagnostics", "LogTextures", true);
		diagnostics.log_present_info = get_bool("Diagnostics", "LogPresentInfo", true);

		// [Tracer]
		tracer.backtrace_depth = get_int("Tracer", "BacktraceDepth", 8);
		tracer.output_dir = get_string("Tracer", "OutputDir", "captures");

		log("Config", std::format("Loaded from: {}", ini_path_));
		log("Config", std::format("FFP={} AlbedoStage={} View=c{}-c{} Proj=c{}-c{} World=c{}-c{}",
			ffp.enabled ? 1 : 0, ffp.albedo_stage,
			ffp.vs_reg_view_start, ffp.vs_reg_view_end - 1,
			ffp.vs_reg_proj_start, ffp.vs_reg_proj_end - 1,
			ffp.vs_reg_world_start, ffp.vs_reg_world_end - 1));
		if (skinning.enabled)
			log("Config", "Skinning ENABLED", LOG_TYPE::LOG_TYPE_WARN);
	}
}
