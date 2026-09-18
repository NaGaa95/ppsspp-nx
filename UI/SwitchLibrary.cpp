#include "UI/SwitchLibrary.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include "Common/Thread/ThreadUtil.h"

namespace SwitchLibrary {
namespace {

constexpr const char *kConfigPath = "/switch/ppsspp-nx/library.ini";
constexpr const char *kDefaultGamesPath = "/switch/ppsspp-nx/games";

std::mutex s_mutex;
std::condition_variable s_condition;
std::vector<Source> s_sources;
std::vector<SwitchStorage::SmbShare> s_smbShares;
std::vector<Path> s_games;
std::map<std::string, std::vector<Path>> s_sourceGames;
std::thread s_worker;
bool s_initialized = false;
bool s_scanRequested = false;
std::set<std::string> s_sourceScanRequests;
std::atomic_bool s_stop{false};
std::atomic_bool s_scanning{false};
std::atomic_uint64_t s_generation{0};

std::string Trim(std::string value) {
	while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t'))
		value.pop_back();
	size_t start = 0;
	while (start < value.size() && (value[start] == ' ' || value[start] == '\t'))
		++start;
	return value.substr(start);
}

std::map<std::string, std::string> ReadConfig() {
	std::map<std::string, std::string> values;
	FILE *file = std::fopen(kConfigPath, "rb");
	if (!file)
		return values;
	char line[4096];
	while (std::fgets(line, sizeof(line), file)) {
		std::string text = Trim(line);
		if (text.empty() || text[0] == '#' || text[0] == ';' || text[0] == '[')
			continue;
		size_t separator = text.find('=');
		if (separator != std::string::npos)
			values[Trim(text.substr(0, separator))] = Trim(text.substr(separator + 1));
	}
	std::fclose(file);
	return values;
}

std::string Value(const std::map<std::string, std::string> &values, const std::string &key) {
	auto it = values.find(key);
	return it == values.end() ? std::string() : it->second;
}

std::string CleanValue(std::string value) {
	value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
	value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
	return value;
}

void SaveConfigLocked() {
	File::CreateFullPath(Path("/switch/ppsspp-nx"));
	const Path temporary(std::string(kConfigPath) + ".tmp");
	const Path backup(std::string(kConfigPath) + ".old");
	FILE *file = File::OpenCFile(temporary, "wb");
	if (!file)
		return;
	std::fprintf(file, "[Library]\nSourceCount=%zu\n", s_sources.size());
	for (size_t i = 0; i < s_sources.size(); ++i) {
		const Source &source = s_sources[i];
		const char *type = source.type == SourceType::Usb ? "usb" : source.type == SourceType::Smb ? "smb" : "local";
		std::fprintf(file, "Source%zuType=%s\nSource%zuId=%s\nSource%zuPath=%s\n", i, type,
			i, CleanValue(source.id).c_str(), i, CleanValue(source.path).c_str());
	}
	std::fprintf(file, "\n[SMB]\nSmbCount=%zu\n", s_smbShares.size());
	for (size_t i = 0; i < s_smbShares.size(); ++i) {
		const auto &share = s_smbShares[i];
		std::fprintf(file,
			"Smb%zuId=%s\nSmb%zuName=%s\nSmb%zuServer=%s\nSmb%zuShare=%s\nSmb%zuPath=%s\n"
			"Smb%zuUser=%s\nSmb%zuPassword=%s\nSmb%zuDomain=%s\nSmb%zuAutoMount=%d\n",
			i, CleanValue(share.id).c_str(), i, CleanValue(share.name).c_str(), i, CleanValue(share.server).c_str(),
			i, CleanValue(share.share).c_str(), i, CleanValue(share.path).c_str(), i, CleanValue(share.user).c_str(),
			i, CleanValue(share.password).c_str(), i, CleanValue(share.domain).c_str(), i, share.autoMount ? 1 : 0);
	}
	std::fclose(file);
	const Path current(kConfigPath);
	File::Delete(backup, true);
	const bool hadCurrent = File::Exists(current);
	if (hadCurrent && !File::Rename(current, backup)) {
		File::Delete(temporary, true);
		return;
	}
	if (!File::Rename(temporary, current)) {
		if (hadCurrent)
			File::Rename(backup, current);
		File::Delete(temporary, true);
		return;
	}
	File::Delete(backup, true);
}

std::string RelativeFromRoot(const std::string &path, const std::string &root) {
	std::string relative = path.substr(root.size());
	while (!relative.empty() && relative.front() == '/')
		relative.erase(relative.begin());
	return relative;
}

std::string AppendRelative(std::string root, std::string relative) {
	if (relative.empty())
		return root;
	while (!relative.empty() && relative.front() == '/')
		relative.erase(relative.begin());
	if (!root.empty() && root.back() != '/')
		root.push_back('/');
	return root + relative;
}

std::string SourceKey(const Source &source) {
	return std::to_string((int)source.type) + "\n" + source.id + "\n" + source.path;
}

std::string ResolveSourceInternal(const Source &source, const std::vector<SwitchStorage::SmbShare> &shares,
	bool connectSmb, std::string *error) {
	if (source.type == SourceType::Local)
		return source.path;
	if (source.type == SourceType::Usb) {
		std::string root = SwitchStorage::ResolveUsbPath(source.id);
		if (root.empty() && error)
			*error = "USB drive is not connected";
		return root.empty() ? std::string() : AppendRelative(root, source.path);
	}
	auto it = std::find_if(shares.begin(), shares.end(), [&](const auto &share) { return share.id == source.id; });
	if (it == shares.end()) {
		if (error)
			*error = "SMB share no longer exists";
		return {};
	}
	if (connectSmb && !SwitchStorage::IsSmbMounted(source.id) && !SwitchStorage::MountSmb(*it, error))
		return {};
	std::string root = SwitchStorage::SmbBrowsePath(*it);
	return root.empty() ? std::string() : AppendRelative(root, source.path);
}

bool IsGameExtension(const Path &path) {
	const std::string extension = path.GetFileExtension();
	return extension == ".iso" || extension == ".cso" || extension == ".chd" || extension == ".pbp" ||
		extension == ".elf" || extension == ".prx" || extension == ".ppdmp";
}

void ScanDirectory(const Path &directory, std::vector<Path> &games, int depth) {
	if (s_stop.load(std::memory_order_acquire) || depth > 24)
		return;
	std::vector<File::FileInfo> entries;
	if (!File::GetFilesInDir(directory, &entries))
		return;
	for (const File::FileInfo &entry : entries) {
		if (s_stop.load(std::memory_order_acquire))
			return;
		if (!entry.isDirectory) {
			if (IsGameExtension(entry.fullName))
				games.push_back(entry.fullName);
			continue;
		}
		if (File::Exists(entry.fullName / "PSP_GAME/SYSDIR") || File::Exists(entry.fullName / "EBOOT.PBP")) {
			games.push_back(entry.fullName);
			continue;
		}
		ScanDirectory(entry.fullName, games, depth + 1);
	}
}

void Worker() {
	SetCurrentThreadName("SwitchLibrary");
	SetCurrentThreadAffinity(ThreadAffinityRole::IO);
	for (;;) {
		std::vector<Source> sources;
		std::vector<SwitchStorage::SmbShare> shares;
		std::set<std::string> sourceScanRequests;
		bool scanAll = false;
		{
			std::unique_lock<std::mutex> lock(s_mutex);
			s_condition.wait(lock, [] {
				return s_scanRequested || !s_sourceScanRequests.empty() || s_stop.load(std::memory_order_acquire);
			});
			if (s_stop.load(std::memory_order_acquire))
				break;
			scanAll = s_scanRequested;
			s_scanRequested = false;
			sourceScanRequests.swap(s_sourceScanRequests);
			sources = s_sources;
			shares = s_smbShares;
		}
		s_scanning.store(true, std::memory_order_release);
		std::map<std::string, std::vector<Path>> sourceGames;
		if (!scanAll) {
			std::lock_guard<std::mutex> lock(s_mutex);
			sourceGames = s_sourceGames;
		}
		std::set<std::string> currentKeys;
		for (const Source &source : sources) {
			std::string key = SourceKey(source);
			currentKeys.insert(key);
			if (!scanAll && sourceScanRequests.find(key) == sourceScanRequests.end())
				continue;
			std::vector<Path> games;
			std::string root = ResolveSourceInternal(source, shares, true, nullptr);
			if (!root.empty())
				ScanDirectory(Path(root), games, 0);
			sourceGames[key] = std::move(games);
		}
		for (auto it = sourceGames.begin(); it != sourceGames.end();) {
			if (currentKeys.find(it->first) == currentKeys.end())
				it = sourceGames.erase(it);
			else
				++it;
		}
		std::vector<Path> games;
		for (const auto &entry : sourceGames)
			games.insert(games.end(), entry.second.begin(), entry.second.end());
		std::sort(games.begin(), games.end(), [](const Path &a, const Path &b) { return a.ToString() < b.ToString(); });
		games.erase(std::unique(games.begin(), games.end()), games.end());
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			s_sourceGames = std::move(sourceGames);
			s_games = std::move(games);
		}
		s_scanning.store(false, std::memory_order_release);
		s_generation.fetch_add(1, std::memory_order_acq_rel);
	}
}

void UsbChanged(void *) {
	RequestScan();
}

}  // namespace

void Initialize() {
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (s_initialized)
			return;
		auto values = ReadConfig();
		int sourceCount = std::clamp(std::atoi(Value(values, "SourceCount").c_str()), 0, 64);
		for (int i = 0; i < sourceCount; ++i) {
			std::string prefix = "Source" + std::to_string(i);
			Source source;
			std::string type = Value(values, prefix + "Type");
			source.type = type == "usb" ? SourceType::Usb : type == "smb" ? SourceType::Smb : SourceType::Local;
			source.id = Value(values, prefix + "Id");
			source.path = Value(values, prefix + "Path");
			if (!source.path.empty() || source.type != SourceType::Local)
				s_sources.push_back(std::move(source));
		}
		int smbCount = std::clamp(std::atoi(Value(values, "SmbCount").c_str()), 0, 16);
		for (int i = 0; i < smbCount; ++i) {
			std::string prefix = "Smb" + std::to_string(i);
			SwitchStorage::SmbShare share;
			share.id = Value(values, prefix + "Id");
			share.name = Value(values, prefix + "Name");
			share.server = Value(values, prefix + "Server");
			share.share = Value(values, prefix + "Share");
			share.path = Value(values, prefix + "Path");
			share.user = Value(values, prefix + "User");
			share.password = Value(values, prefix + "Password");
			share.domain = Value(values, prefix + "Domain");
			share.autoMount = Value(values, prefix + "AutoMount") != "0";
			if (!share.id.empty() && !share.server.empty() && !share.share.empty())
				s_smbShares.push_back(std::move(share));
		}
		if (s_sources.empty()) {
			File::CreateFullPath(Path(kDefaultGamesPath));
			s_sources.push_back({SourceType::Local, {}, kDefaultGamesPath});
			SaveConfigLocked();
		}
		s_stop = false;
		s_scanRequested = true;
		s_initialized = true;
	}
	SwitchStorage::InitializeUsb(nullptr);
	SwitchStorage::SetUsbStatusCallback(UsbChanged, nullptr);
	s_worker = std::thread(Worker);
	s_condition.notify_one();
}

void Shutdown() {
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (!s_initialized)
			return;
		s_stop = true;
		s_condition.notify_one();
	}
	if (s_worker.joinable())
		s_worker.join();
	SwitchStorage::Shutdown();
	std::lock_guard<std::mutex> lock(s_mutex);
	s_initialized = false;
	s_scanning = false;
}

std::vector<Source> GetSources() {
	std::lock_guard<std::mutex> lock(s_mutex);
	return s_sources;
}

std::string ResolveSource(const Source &source, bool connectSmb, std::string *error) {
	std::vector<SwitchStorage::SmbShare> shares;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		shares = s_smbShares;
	}
	return ResolveSourceInternal(source, shares, connectSmb, error);
}

std::string DescribeSource(const Source &source) {
	std::string resolved = ResolveSource(source, false, nullptr);
	if (!resolved.empty())
		return resolved;
	if (source.type == SourceType::Usb)
		return "USB: " + source.path;
	if (source.type == SourceType::Smb)
		return "SMB: " + source.id + "/" + source.path;
	return source.path;
}

bool AddSource(const Path &path) {
	std::string text = path.ToString();
	Source source{SourceType::Local, {}, text};
	for (const auto &location : SwitchStorage::ListUsbLocations()) {
		if (text.rfind(location.path, 0) == 0) {
			source = {SourceType::Usb, location.id, RelativeFromRoot(text, location.path)};
			break;
		}
	}
	if (source.type == SourceType::Local) {
		for (const auto &share : GetSmbShares()) {
			std::string root = SwitchStorage::SmbBrowsePath(share);
			if (!root.empty() && text.rfind(root, 0) == 0) {
				source = {SourceType::Smb, share.id, RelativeFromRoot(text, root)};
				break;
			}
		}
	}
	std::lock_guard<std::mutex> lock(s_mutex);
	auto same = [&](const Source &other) {
		return source.type == other.type && source.id == other.id && source.path == other.path;
	};
	if (std::any_of(s_sources.begin(), s_sources.end(), same))
		return false;
	s_sources.push_back(std::move(source));
	SaveConfigLocked();
	s_scanRequested = true;
	s_condition.notify_one();
	return true;
}

void RemoveSource(size_t index) {
	std::lock_guard<std::mutex> lock(s_mutex);
	if (index >= s_sources.size())
		return;
	s_sources.erase(s_sources.begin() + index);
	SaveConfigLocked();
	s_scanRequested = true;
	s_condition.notify_one();
}

std::vector<SwitchStorage::SmbShare> GetSmbShares() {
	std::lock_guard<std::mutex> lock(s_mutex);
	return s_smbShares;
}

bool SaveSmbShare(const SwitchStorage::SmbShare &share, std::string *error) {
	bool validId = !share.id.empty() && share.id.size() <= 16 &&
		std::all_of(share.id.begin(), share.id.end(), [](unsigned char c) {
			return std::isalnum(c) || c == '_';
		});
	if (!validId || share.server.empty() || share.share.empty()) {
		if (error)
			*error = "A valid server, share, and name are required";
		return false;
	}
	SwitchStorage::UnmountSmb(share.id);
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		auto it = std::find_if(s_smbShares.begin(), s_smbShares.end(), [&](const auto &item) { return item.id == share.id; });
		if (it == s_smbShares.end())
			s_smbShares.push_back(share);
		else
			*it = share;
		SaveConfigLocked();
	}
	if (share.autoMount && !SwitchStorage::MountSmb(share, error))
		return false;
	RequestScan();
	return true;
}

void RemoveSmbShare(const std::string &id) {
	SwitchStorage::UnmountSmb(id);
	std::lock_guard<std::mutex> lock(s_mutex);
	s_smbShares.erase(std::remove_if(s_smbShares.begin(), s_smbShares.end(), [&](const auto &share) {
		return share.id == id;
	}), s_smbShares.end());
	s_sources.erase(std::remove_if(s_sources.begin(), s_sources.end(), [&](const Source &source) {
		return source.type == SourceType::Smb && source.id == id;
	}), s_sources.end());
	SaveConfigLocked();
	s_scanRequested = true;
	s_condition.notify_one();
}

void RequestScan() {
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_initialized)
		return;
	s_scanRequested = true;
	s_sourceScanRequests.clear();
	s_condition.notify_one();
}

void RequestScan(size_t sourceIndex) {
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_initialized || sourceIndex >= s_sources.size())
		return;
	if (!s_scanRequested)
		s_sourceScanRequests.insert(SourceKey(s_sources[sourceIndex]));
	s_condition.notify_one();
}

std::vector<Path> GetGames() {
	std::lock_guard<std::mutex> lock(s_mutex);
	return s_games;
}

uint64_t Generation() {
	return s_generation.load(std::memory_order_acquire);
}

bool IsScanning() {
	return s_scanning.load(std::memory_order_acquire);
}

}  // namespace SwitchLibrary
