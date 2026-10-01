#include "gui/wxgui.h"
#include "gui/DispatcherControl.h"
#include "gui/MainWindow.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/OS/libs/nsyshid/Skylander.h"
#include "Common/FileStream.h"
#include "Common/version.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "util/helpers/helpers.h"
#include "Cemu/Logging/CemuLogging.h"

#include <boost/asio.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <future>

namespace DispatcherControl
{
	namespace asio = boost::asio;
	using asio::ip::tcp;
	using Json = rapidjson::Document;
	using Value = rapidjson::Value;

	struct CommandError : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};

	static std::unique_ptr<asio::io_context> s_io;
	static std::unique_ptr<tcp::acceptor> s_acceptor;
	static std::thread s_thread;
	static std::atomic_bool s_running{false};

	constexpr uint32 kMaxMemoryTransfer = 1024 * 1024;

	// --- helpers --------------------------------------------------------------------------

	// Runs f on the wx main thread and waits for its result (graphic packs and the main window are
	// only touched from there, like the UI does).
	template<typename F>
	static auto RunOnMainThread(F&& f) -> decltype(f())
	{
		using R = decltype(f());
		auto promise = std::make_shared<std::promise<R>>();
		auto future = promise->get_future();
		wxTheApp->CallAfter([promise, f = std::forward<F>(f)]() mutable {
			try
			{
				if constexpr (std::is_void_v<R>)
				{
					f();
					promise->set_value();
				}
				else
					promise->set_value(f());
			}
			catch (...)
			{
				promise->set_exception(std::current_exception());
			}
		});
		// no timeout: f captures the caller's locals by reference, so the caller must outlive it
		return future.get();
	}

	static const Value& Arg(const Value& args, const char* name)
	{
		if (!args.IsObject() || !args.HasMember(name))
			throw CommandError(fmt::format("missing argument \"{}\"", name));
		return args[name];
	}

	static bool HasArg(const Value& args, const char* name)
	{
		return args.IsObject() && args.HasMember(name) && !args[name].IsNull();
	}

	static std::string StringArg(const Value& args, const char* name)
	{
		const auto& v = Arg(args, name);
		if (!v.IsString())
			throw CommandError(fmt::format("argument \"{}\" must be a string", name));
		return {v.GetString(), v.GetStringLength()};
	}

	// numbers may be JSON numbers or strings ("0x02003F1C", "1234")
	static uint64 UintArg(const Value& args, const char* name)
	{
		const auto& v = Arg(args, name);
		if (v.IsUint64())
			return v.GetUint64();
		if (v.IsString())
		{
			std::string s{v.GetString(), v.GetStringLength()};
			int base = 10;
			if (s.starts_with("0x") || s.starts_with("0X"))
			{
				s = s.substr(2);
				base = 16;
			}
			uint64 out = 0;
			const auto res = std::from_chars(s.data(), s.data() + s.size(), out, base);
			if (!s.empty() && res.ec == std::errc{} && res.ptr == s.data() + s.size())
				return out;
		}
		throw CommandError(fmt::format("argument \"{}\" must be an unsigned number", name));
	}

	static bool BoolArg(const Value& args, const char* name, bool fallback)
	{
		if (!HasArg(args, name))
			return fallback;
		const auto& v = args[name];
		if (!v.IsBool())
			throw CommandError(fmt::format("argument \"{}\" must be a boolean", name));
		return v.GetBool();
	}

	static std::string ToHex(const uint8* data, size_t size)
	{
		static constexpr char digits[] = "0123456789abcdef";
		std::string out(size * 2, '0');
		for (size_t i = 0; i < size; i++)
		{
			out[i * 2] = digits[data[i] >> 4];
			out[i * 2 + 1] = digits[data[i] & 0xF];
		}
		return out;
	}

	static std::vector<uint8> FromHex(std::string_view hex, const char* name)
	{
		std::string clean;
		for (char c : hex)
			if (!isspace((unsigned char)c))
				clean.push_back(c);
		if (clean.size() % 2 != 0)
			throw CommandError(fmt::format("argument \"{}\" must be an even number of hex digits", name));
		std::vector<uint8> out(clean.size() / 2);
		for (size_t i = 0; i < out.size(); i++)
		{
			const auto res = std::from_chars(clean.data() + i * 2, clean.data() + i * 2 + 2, out[i], 16);
			if (res.ec != std::errc{} || res.ptr != clean.data() + i * 2 + 2)
				throw CommandError(fmt::format("argument \"{}\" is not hex", name));
		}
		return out;
	}

	static Value Str(std::string_view s, Json::AllocatorType& a)
	{
		return Value(s.data(), (rapidjson::SizeType)s.size(), a);
	}

	static std::string Hex32(uint64 v, int width)
	{
		return fmt::format("{:0{}x}", v, width);
	}

	// --- commands -------------------------------------------------------------------------

	static void CmdHello(const Value&, Value& out, Json::AllocatorType& a)
	{
		out.SetObject();
		out.AddMember("protocol", kProtocolVersion, a);
		out.AddMember("cemu", Str(BUILD_VERSION_STRING, a), a);
		out.AddMember("userDataPath", Str(_pathToUtf8(ActiveSettings::GetUserDataPath()), a), a);
		const bool running = CafeSystem::IsTitleRunning();
		out.AddMember("titleRunning", running, a);
		if (running)
		{
			out.AddMember("titleId", Str(Hex32(CafeSystem::GetForegroundTitleId(), 16), a), a);
			out.AddMember("titleName", Str(CafeSystem::GetForegroundTitleName(), a), a);
			out.AddMember("titleVersion", (unsigned)CafeSystem::GetForegroundTitleVersion(), a);
			out.AddMember("rpxHashBase", Str(Hex32(CafeSystem::GetRPXHashBase(), 8), a), a);
			out.AddMember("rpxHashUpdated", Str(Hex32(CafeSystem::GetRPXHashUpdated(), 8), a), a);
		}
	}

	static void CmdQuit(const Value&, Value& out, Json::AllocatorType&)
	{
		// threads paused by the debugger would keep the title from shutting down
		if (g_gdbstub)
			g_gdbstub->ReleaseThreads();
		// not awaited: closing the main window ends the process
		wxTheApp->CallAfter([] {
			if (g_mainFrame)
				g_mainFrame->Close();
		});
		out.SetObject();
	}

	static void RequireRange(uint32 address, uint32 length)
	{
		if (length == 0 || length > kMaxMemoryTransfer)
			throw CommandError(fmt::format("length must be between 1 and {}", kMaxMemoryTransfer));
		if ((uint64)address + length > 0x100000000ull || !memory_isAddressRangeAccessible(address, length))
			throw CommandError(fmt::format("0x{:08x}..0x{:08x} is not mapped", address, (uint64)address + length));
	}

	static void CmdMemoryRead(const Value& args, Value& out, Json::AllocatorType& a)
	{
		const uint32 address = (uint32)UintArg(args, "address");
		const uint32 length = (uint32)UintArg(args, "length");
		RequireRange(address, length);
		std::vector<uint8> buf(length);
		memcpy(buf.data(), memory_getPointerFromVirtualOffset(address), length);
		out.SetObject();
		out.AddMember("address", Str(Hex32(address, 8), a), a);
		out.AddMember("bytes", Str(ToHex(buf.data(), buf.size()), a), a);
	}

	static void CmdMemoryWrite(const Value& args, Value& out, Json::AllocatorType& a)
	{
		const uint32 address = (uint32)UintArg(args, "address");
		const auto bytes = FromHex(StringArg(args, "bytes"), "bytes");
		RequireRange(address, (uint32)bytes.size());
		memcpy(memory_getPointerFromVirtualOffset(address), bytes.data(), bytes.size());
		// translated code for the range is dropped so patched instructions take effect
		PPCRecompiler_invalidateRange(address, address + (uint32)bytes.size());
		out.SetObject();
		out.AddMember("written", (uint32)bytes.size(), a);
	}

	static void CmdMemoryFind(const Value& args, Value& out, Json::AllocatorType& a)
	{
		const uint64 start = UintArg(args, "start");
		const uint64 end = UintArg(args, "end");
		const auto pattern = FromHex(StringArg(args, "pattern"), "pattern");
		std::vector<uint8> mask(pattern.size(), 0xFF);
		if (HasArg(args, "mask"))
		{
			mask = FromHex(StringArg(args, "mask"), "mask");
			if (mask.size() != pattern.size())
				throw CommandError("mask must be as long as pattern");
		}
		const uint32 align = HasArg(args, "align") ? std::max<uint32>(1, (uint32)UintArg(args, "align")) : 1;
		const uint32 maxResults = HasArg(args, "maxResults") ? (uint32)UintArg(args, "maxResults") : 100;
		if (pattern.empty())
			throw CommandError("pattern is empty");
		if (end <= start || end > 0x100000000ull)
			throw CommandError("start must be below end, and end at most 0x100000000");

		Value matches(rapidjson::kArrayType);
		bool truncated = false;
		uint64 scanned = 0;
		// scan page by page so unmapped holes are skipped; matches may span page boundaries
		constexpr uint64 kChunk = 0x10000;
		for (uint64 chunk = start & ~(kChunk - 1); chunk < end && !truncated; chunk += kChunk)
		{
			const uint64 from = std::max(chunk, start);
			const uint64 to = std::min(chunk + kChunk, end);
			if (!memory_isAddressRangeAccessible((uint32)from, (uint32)(to - from)))
				continue;
			scanned += to - from;
			for (uint64 addr = (from + align - 1) / align * align; addr < to; addr += align)
			{
				if (addr + pattern.size() > end || !memory_isAddressRangeAccessible((uint32)addr, (uint32)pattern.size()))
					break;
				const uint8* p = memory_getPointerFromVirtualOffset((uint32)addr);
				bool hit = true;
				for (size_t i = 0; i < pattern.size() && hit; i++)
					hit = (p[i] & mask[i]) == (pattern[i] & mask[i]);
				if (!hit)
					continue;
				if (matches.Size() >= maxResults)
				{
					truncated = true;
					break;
				}
				matches.PushBack(Str(Hex32(addr, 8), a), a);
			}
		}
		out.SetObject();
		out.AddMember("matches", matches, a);
		out.AddMember("truncated", truncated, a);
		out.AddMember("bytesScanned", (uint64_t)scanned, a);
	}

	static Value FigureJson(uint16 id, uint16 variant, Json::AllocatorType& a)
	{
		Value v(rapidjson::kObjectType);
		v.AddMember("id", (unsigned)id, a);
		v.AddMember("variant", (unsigned)variant, a);
		v.AddMember("name", Str(nsyshid::g_skyportal.FindSkylander(id, variant), a), a);
		return v;
	}

	static void CmdPortalList(const Value&, Value& out, Json::AllocatorType& a)
	{
		Value slots(rapidjson::kArrayType);
		for (const auto& s : nsyshid::g_skyportal.GetLoadedSlots())
		{
			Value v = FigureJson(s.skyId, s.skyVar, a);
			v.AddMember("slot", (unsigned)s.slot, a);
			slots.PushBack(v, a);
		}
		out.SetObject();
		out.AddMember("emulated", GetConfig().emulated_usb_devices.emulate_skylander_portal.GetValue(), a);
		out.AddMember("figures", slots, a);
	}

	static void CmdPortalLoad(const Value& args, Value& out, Json::AllocatorType& a)
	{
		const std::string path = StringArg(args, "path");
		std::unique_ptr<FileStream> file(FileStream::openFile2(_utf8ToPath(path), true));
		if (!file)
			throw CommandError(fmt::format("cannot open {} for reading and writing", path));
		std::array<uint8, nsyshid::SKY_FIGURE_SIZE> data;
		if (file->readData(data.data(), data.size()) != data.size())
			throw CommandError(fmt::format("{} is smaller than a figure dump ({} bytes)", path, nsyshid::SKY_FIGURE_SIZE));
		const uint16 id = uint16(data[0x11]) << 8 | uint16(data[0x10]);
		const uint16 variant = uint16(data[0x1D]) << 8 | uint16(data[0x1C]);
		const uint8 slot = nsyshid::g_skyportal.LoadSkylander(data.data(), std::move(file));
		if (slot == 0xFF)
			throw CommandError("the portal is full (16 figures)");
		out = FigureJson(id, variant, a);
		out.AddMember("slot", (unsigned)slot, a);
	}

	static void CmdPortalRemove(const Value& args, Value& out, Json::AllocatorType& a)
	{
		const uint64 slot = UintArg(args, "slot");
		if (slot >= nsyshid::MAX_SKYLANDERS)
			throw CommandError(fmt::format("slot must be below {}", nsyshid::MAX_SKYLANDERS));
		if (!nsyshid::g_skyportal.RemoveSkylander((uint8)slot))
			throw CommandError(fmt::format("no figure in slot {}", slot));
		out.SetObject();
	}

	static void CmdPortalCreate(const Value& args, Value& out, Json::AllocatorType& a)
	{
		const std::string path = StringArg(args, "path");
		const uint16 id = (uint16)UintArg(args, "id");
		const uint16 variant = HasArg(args, "variant") ? (uint16)UintArg(args, "variant") : 0;
		if (!nsyshid::g_skyportal.CreateSkylander(_utf8ToPath(path), id, variant))
			throw CommandError(fmt::format("cannot create {}", path));
		out = FigureJson(id, variant, a);
		out.AddMember("path", Str(path, a), a);
	}

	static void CmdPortalKnown(const Value&, Value& out, Json::AllocatorType& a)
	{
		out.SetArray();
		for (const auto& [key, name] : nsyshid::SkylanderUSB::GetListSkylanders())
		{
			Value v(rapidjson::kObjectType);
			v.AddMember("id", (unsigned)key.first, a);
			v.AddMember("variant", (unsigned)key.second, a);
			v.AddMember("name", Str(name, a), a);
			out.PushBack(v, a);
		}
	}

	static GraphicPackPtr FindPack(const std::string& path)
	{
		const auto wanted = _utf8ToPath(path).lexically_normal();
		for (const auto& gp : GraphicPack2::GetGraphicPacks())
		{
			if (_utf8ToPath(gp->GetNormalizedPathString()).lexically_normal() == wanted)
				return gp;
		}
		throw CommandError(fmt::format("no graphic pack {} (use packs.list for paths, packs.rescan after adding one)", path));
	}

	static Value PackJson(const GraphicPackPtr& gp, Json::AllocatorType& a)
	{
		Value v(rapidjson::kObjectType);
		v.AddMember("path", Str(gp->GetNormalizedPathString(), a), a);
		v.AddMember("name", Str(gp->GetName(), a), a);
		v.AddMember("virtualPath", Str(gp->GetVirtualPath(), a), a);
		v.AddMember("version", gp->GetVersion(), a);
		v.AddMember("enabled", gp->IsEnabled(), a);
		v.AddMember("activated", gp->IsActivated(), a);
		Value titles(rapidjson::kArrayType);
		for (const auto id : gp->GetTitleIds())
			titles.PushBack(Str(Hex32(id, 16), a), a);
		v.AddMember("titleIds", titles, a);
		Value presets(rapidjson::kArrayType);
		for (const auto& p : gp->GetPresets())
		{
			if (!p->visible)
				continue;
			Value pv(rapidjson::kObjectType);
			pv.AddMember("category", Str(p->category, a), a);
			pv.AddMember("name", Str(p->name, a), a);
			pv.AddMember("active", p->active, a);
			presets.PushBack(pv, a);
		}
		v.AddMember("presets", presets, a);
		return v;
	}

	// what GraphicPacksWindow2::SaveStateToConfig stores
	static void SavePackStateToConfig()
	{
		auto& data = g_config.data();
		data.graphic_pack_entries.clear();
		for (const auto& gp : GraphicPack2::GetGraphicPacks())
		{
			auto filename = _utf8ToPath(gp->GetNormalizedPathString());
			if (gp->IsEnabled())
			{
				auto& entry = data.graphic_pack_entries[filename];
				for (const auto& preset : gp->GetActivePresets())
					entry.try_emplace(preset->category, preset->name);
			}
			else if (gp->IsDefaultEnabled())
				data.graphic_pack_entries[filename].try_emplace("_disabled", "false");
		}
		g_config.Save();
	}

	static void DeleteShadersFromRuntimeCache(const GraphicPackPtr& gp)
	{
		for (const auto& shader : gp->GetCustomShaders())
		{
			LatteConst::ShaderType type = LatteConst::ShaderType::Pixel;
			if (shader.type == GraphicPack2::GP_SHADER_TYPE::VERTEX)
				type = LatteConst::ShaderType::Vertex;
			else if (shader.type == GraphicPack2::GP_SHADER_TYPE::GEOMETRY)
				type = LatteConst::ShaderType::Geometry;
			LatteAsyncCommands_queueDeleteShader(shader.shader_base_hash, shader.shader_aux_hash, type);
		}
	}

	// GraphicPacksWindow2::ReloadPack: re-reads rules.txt and patches from disk
	static bool ReloadPack(const GraphicPackPtr& gp)
	{
		if (!(gp->HasShaders() || gp->HasPatches() || gp->HasCustomVSyncFrequency()))
			return false;
		if (!gp->Reload())
			return false;
		DeleteShadersFromRuntimeCache(gp);
		return true;
	}

	static bool RunningForPack(const GraphicPackPtr& gp)
	{
		return CafeSystem::IsTitleRunning() && gp->ContainsTitleId(CafeSystem::GetForegroundTitleId());
	}

	static void CmdPacksList(const Value& args, Value& out, Json::AllocatorType& a)
	{
		const bool all = BoolArg(args, "all", false);
		RunOnMainThread([&] {
			const bool running = CafeSystem::IsTitleRunning();
			const uint64 titleId = running ? CafeSystem::GetForegroundTitleId() : 0;
			Value packs(rapidjson::kArrayType);
			for (const auto& gp : GraphicPack2::GetGraphicPacks())
			{
				if (!all && running && !gp->ContainsTitleId(titleId))
					continue;
				packs.PushBack(PackJson(gp, a), a);
			}
			out.SetObject();
			out.AddMember("filteredToTitle", !all && running, a);
			out.AddMember("packs", packs, a);
		});
	}

	static void CmdPacksRescan(const Value&, Value& out, Json::AllocatorType& a)
	{
		RunOnMainThread([&] {
			// like GraphicPack2::LoadAll, but only adds packs that are not loaded yet, so it is safe
			// while a title runs
			std::set<fs::path> known;
			for (const auto& gp : GraphicPack2::GetGraphicPacks())
				known.insert(gp->GetRulesPath().lexically_normal());
			Value added(rapidjson::kArrayType);
			std::error_code ec;
			const fs::path basePath = ActiveSettings::GetUserDataPath("graphicPacks");
			for (fs::recursive_directory_iterator it(basePath, fs::directory_options::follow_directory_symlink, ec); it != end(it); ++it)
			{
				if (!it->is_directory(ec) || !fs::exists(it->path() / "rules.txt", ec))
					continue;
				it.disable_recursion_pending();
				if (known.contains((it->path() / "rules.txt").lexically_normal()))
					continue;
				GraphicPack2::LoadGraphicPack(it->path());
				added.PushBack(Str(_pathToUtf8(it->path()), a), a);
			}
			out.SetObject();
			out.AddMember("added", added, a);
		});
	}

	static void CmdPacksSet(const Value& args, Value& out, Json::AllocatorType& a)
	{
		const std::string path = StringArg(args, "path");
		std::vector<std::pair<std::string, std::string>> presets;
		if (HasArg(args, "presets"))
		{
			const auto& p = args["presets"];
			if (!p.IsObject())
				throw CommandError("presets must be an object of category -> preset name");
			for (auto it = p.MemberBegin(); it != p.MemberEnd(); ++it)
			{
				if (!it->value.IsString())
					throw CommandError("preset names must be strings");
				presets.emplace_back(it->name.GetString(), it->value.GetString());
			}
		}
		const std::optional<bool> enabled = HasArg(args, "enabled") ? std::optional(BoolArg(args, "enabled", false)) : std::nullopt;

		RunOnMainThread([&] {
			const auto gp = FindPack(path);
			bool requiresRestart = false;

			for (const auto& [category, name] : presets)
			{
				const auto& all = gp->GetPresets();
				if (std::none_of(all.begin(), all.end(), [&](const auto& pr) { return pr->category == category && pr->name == name; }))
					throw CommandError(fmt::format("{} has no preset \"{}\" in category \"{}\"", path, name, category));
				gp->SetActivePreset(category, name);
			}
			if (!presets.empty() && RunningForPack(gp) && gp->IsActivated())
			{
				if (gp->RequiresRestart(false, true))
					requiresRestart = true;
				else
					ReloadPack(gp);
			}

			// GraphicPacksWindow2::OnTreeChoiceChanged
			if (enabled && *enabled != gp->IsEnabled())
			{
				gp->SetEnabled(*enabled);
				const bool needsRestart = gp->RequiresRestart(true, false);
				if (RunningForPack(gp))
				{
					if (*enabled)
					{
						GraphicPack2::ActivateGraphicPack(gp);
						if (!needsRestart)
							ReloadPack(gp);
					}
					else
					{
						if (!needsRestart)
							DeleteShadersFromRuntimeCache(gp);
						GraphicPack2::DeactivateGraphicPack(gp);
					}
					requiresRestart |= needsRestart;
				}
			}
			SavePackStateToConfig();
			out = PackJson(gp, a);
			out.AddMember("requiresRestart", requiresRestart, a);
		});
	}

	static void CmdPacksReload(const Value& args, Value& out, Json::AllocatorType& a)
	{
		const std::string path = StringArg(args, "path");
		RunOnMainThread([&] {
			const auto gp = FindPack(path);
			if (!gp->IsActivated())
				throw CommandError(fmt::format("{} is not active in the running title", path));
			const bool reloaded = ReloadPack(gp);
			out = PackJson(gp, a);
			out.AddMember("reloaded", reloaded, a);
		});
	}

	using Handler = void (*)(const Value& args, Value& out, Json::AllocatorType& a);
	static const std::unordered_map<std::string, Handler> s_handlers = {
		{"hello", CmdHello},
		{"quit", CmdQuit},
		{"memory.read", CmdMemoryRead},
		{"memory.write", CmdMemoryWrite},
		{"memory.find", CmdMemoryFind},
		{"portal.list", CmdPortalList},
		{"portal.load", CmdPortalLoad},
		{"portal.remove", CmdPortalRemove},
		{"portal.create", CmdPortalCreate},
		{"portal.known", CmdPortalKnown},
		{"packs.list", CmdPacksList},
		{"packs.rescan", CmdPacksRescan},
		{"packs.set", CmdPacksSet},
		{"packs.reload", CmdPacksReload},
	};

	// --- connection -----------------------------------------------------------------------

	static std::string HandleLine(std::string_view line, bool& authenticated, const std::string& token)
	{
		Json response(rapidjson::kObjectType);
		auto& a = response.GetAllocator();
		Json request;
		request.Parse(line.data(), line.size());
		if (request.HasParseError() || !request.IsObject())
		{
			response.AddMember("ok", false, a);
			response.AddMember("error", "request is not a JSON object", a);
		}
		else
		{
			if (request.HasMember("id"))
				response.AddMember("id", Value(request["id"], a), a);
			const std::string cmd = request.HasMember("cmd") && request["cmd"].IsString() ? request["cmd"].GetString() : "";
			const Value empty(rapidjson::kObjectType);
			const Value& args = request.HasMember("args") ? request["args"] : empty;
			try
			{
				Value result;
				if (cmd == "auth")
				{
					authenticated = token.empty() || StringArg(args, "token") == token;
					if (!authenticated)
						throw CommandError("wrong token");
					result.SetObject();
				}
				else if (!authenticated)
					throw CommandError("authenticate first: {\"cmd\":\"auth\",\"args\":{\"token\":...}}");
				else if (const auto it = s_handlers.find(cmd); it != s_handlers.end())
					it->second(args, result, a);
				else
					throw CommandError(fmt::format("unknown command \"{}\"", cmd));
				response.AddMember("ok", true, a);
				response.AddMember("result", result, a);
			}
			catch (const std::exception& e)
			{
				response.AddMember("ok", false, a);
				response.AddMember("error", Str(e.what(), a), a);
			}
		}
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		response.Accept(writer);
		return std::string(buffer.GetString(), buffer.GetSize()) + "\n";
	}

	static void ServeClient(tcp::socket socket, const std::string& token)
	{
		bool authenticated = token.empty();
		asio::streambuf buffer;
		boost::system::error_code ec;
		while (s_running)
		{
			const size_t n = asio::read_until(socket, buffer, '\n', ec);
			if (ec)
				return;
			std::string line(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + n);
			buffer.consume(n);
			while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
				line.pop_back();
			if (line.empty())
				continue;
			const std::string reply = HandleLine(line, authenticated, token);
			asio::write(socket, asio::buffer(reply), ec);
			if (ec)
				return;
		}
	}

	void Start(uint16 port)
	{
		if (s_running)
			return;
		const char* envToken = std::getenv("CEMU_DISPATCHER_TOKEN");
		const std::string token = envToken ? envToken : "";
		try
		{
			s_io = std::make_unique<asio::io_context>();
			s_acceptor = std::make_unique<tcp::acceptor>(*s_io);
			const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);
			s_acceptor->open(endpoint.protocol());
			s_acceptor->bind(endpoint);
			s_acceptor->listen();
		}
		catch (const std::exception& e)
		{
			cemuLog_log(LogType::Force, "Dispatcher control: cannot listen on 127.0.0.1:{}: {}", port, e.what());
			s_acceptor.reset();
			s_io.reset();
			return;
		}
		s_running = true;
		cemuLog_log(LogType::Force, "Dispatcher control: listening on 127.0.0.1:{}{}", port, token.empty() ? " (no token)" : "");
		s_thread = std::thread([token] {
			SetThreadName("DispatcherControl");
			// a thread per client: the bridge holds one connection for the whole session while
			// Dispatcher connects separately to ask Cemu to quit
			while (s_running)
			{
				boost::system::error_code ec;
				tcp::socket socket(*s_io);
				s_acceptor->accept(socket, ec);
				if (ec)
				{
					if (!s_running)
						return;
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					continue;
				}
				std::thread([socket = std::move(socket), token]() mutable {
					SetThreadName("DispatcherClient");
					ServeClient(std::move(socket), token);
				}).detach();
			}
		});
	}

	void Stop()
	{
		if (!s_running.exchange(false))
			return;
		boost::system::error_code ec;
		s_acceptor->close(ec);
		if (s_thread.joinable())
			s_thread.detach(); // a connected client may still block in read; the process is ending
	}
}
