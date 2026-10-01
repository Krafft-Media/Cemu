#pragma once

#include <optional>
#include <string>

class LaunchSettings
{
public:
	// winmain
	static bool HandleCommandline(const wchar_t* lpCmdLine);
	// wmain
	static bool HandleCommandline(int argc, wchar_t* argv[]);
	// main (unix)
	static bool HandleCommandline(int argc, char* argv[]);

	static bool HandleCommandline(const std::vector<std::wstring>& args);

	static std::optional<fs::path> GetLoadFile() { return s_load_game_file; }
    static std::optional<uint64> GetLoadTitleID() {return s_load_title_id;}
	static std::optional<fs::path> GetMLCPath() { return s_mlc_path; }

	static std::optional<bool> RenderUpsideDownEnabled() { return s_render_upside_down; }
	static std::optional<bool> FullscreenEnabled() { return s_fullscreen; }

	static bool GDBStubEnabled() { return s_enable_gdbstub; }
	static bool NSightModeEnabled() { return s_nsight_mode; }

	static bool ForceInterpreter() { return s_force_interpreter; };

	static std::optional<uint32> GetPersistentId() { return s_persistent_id; }

	// Dispatcher: per-instance data folder, control server port, log mirroring to stdout
	static std::optional<fs::path> GetUserDataDir() { return s_user_data_dir; }
	static std::optional<uint16> GetControlPort() { return s_control_port; }
	static bool LogToStdout() { return s_log_stdout; }
	static bool GDBStubEntryStop() { return s_gdbstub_entry_stop; }

private:
	inline static std::optional<fs::path> s_load_game_file{};
    inline static std::optional<uint64> s_load_title_id{};
	inline static std::optional<fs::path> s_mlc_path{};

	inline static std::optional<bool> s_render_upside_down{};
	inline static std::optional<bool> s_fullscreen{};
	
	inline static bool s_enable_gdbstub = false;
	inline static bool s_nsight_mode = false;

	inline static bool s_force_interpreter = false;
	
	inline static std::optional<uint32> s_persistent_id{};

	inline static std::optional<fs::path> s_user_data_dir{};
	inline static std::optional<uint16> s_control_port{};
	inline static bool s_log_stdout = false;
	inline static bool s_gdbstub_entry_stop = true;

	static bool ExtractorTool(std::wstring_view wud_path, std::string_view output_path, std::wstring_view log_path);
};


