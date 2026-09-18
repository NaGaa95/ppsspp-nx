// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(SWITCH)

#include <switch.h>
#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "Common/Data/Format/JSONReader.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/System/Request.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/UI/ScrollView.h"
#include "Common/UI/ScreenManager.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "UI/SwitchUpdate.h"

#ifndef PPSSPP_SWITCH_RELEASE_VERSION
#define PPSSPP_SWITCH_RELEASE_VERSION "1.0.0"
#endif

namespace {

constexpr char LATEST_RELEASE_URL[] = "https://api.github.com/repos/NaGaa95/ppsspp-nx/releases/latest";
constexpr char RELEASE_ASSET_PREFIX[] = "https://github.com/NaGaa95/ppsspp-nx/releases/download/";
constexpr size_t MAX_RELEASE_RESPONSE = 2 * 1024 * 1024;
constexpr uint64_t MINIMUM_NRO_SIZE = 1024 * 1024;
constexpr uint64_t MAXIMUM_NRO_SIZE = 512ULL * 1024 * 1024;

std::string executablePath_;

enum class UpdateState {
	Checking,
	UpdateAvailable,
	UpToDate,
	Downloading,
	ReadyToInstall,
	Installing,
	Installed,
	Cancelled,
	Error,
};

struct ReleaseInfo {
	std::string tag;
	std::string name;
	std::string notes;
	std::string assetName;
	std::string assetUrl;
	std::string assetDigest;
	uint64_t assetSize = 0;
};

struct UpdateSnapshot {
	UpdateState state = UpdateState::Checking;
	ReleaseInfo release;
	std::string error;
	uint64_t downloaded = 0;
	uint64_t total = 0;
};

bool StartsWith(std::string_view value, std::string_view prefix) {
	return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return (char)std::tolower(c);
	});
	return value;
}

int HexDigit(char value) {
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

bool ParseRelease(const std::string &data, ReleaseInfo *release, std::string *error) {
	json::JsonReader reader(data.data(), data.size());
	json::JsonGet root = reader.root();
	if (!reader.ok() || !root || !root.getString("tag_name", &release->tag) || release->tag.empty()) {
		*error = "GitHub returned a release without a valid version.";
		return false;
	}
	root.getString("name", &release->name);
	root.getString("body", &release->notes);
	if (release->name.empty())
		release->name = release->tag;
	if (release->notes.empty())
		release->notes = "No changelog was provided for this release.";

	int bestAsset = -1;
	const JsonNode *assets = root.getArray("assets");
	if (!assets) {
		*error = "The latest release does not contain a downloadable PPSSPP NRO.";
		return false;
	}
	int index = 0;
	for (const JsonNode *node : assets->value) {
		json::JsonGet asset = node->value;
		std::string name;
		std::string url;
		double jsonSize = asset.getFloat("size", -1.0);
		if (!asset.getString("name", &name) || !asset.getString("browser_download_url", &url) || jsonSize < 0.0) {
			++index;
			continue;
		}
		uint64_t size = (uint64_t)jsonSize;
		std::string lowerName = Lower(name);
		if (lowerName.size() < 4 || lowerName.substr(lowerName.size() - 4) != ".nro" ||
			size < MINIMUM_NRO_SIZE || size > MAXIMUM_NRO_SIZE || !StartsWith(url, RELEASE_ASSET_PREFIX)) {
			++index;
			continue;
		}
		if (bestAsset < 0 || lowerName == "ppsspp.nro") {
			bestAsset = index;
			release->assetName = std::move(name);
			release->assetUrl = std::move(url);
			release->assetSize = size;
			asset.getString("digest", &release->assetDigest);
			if (lowerName == "ppsspp.nro")
				break;
		}
		++index;
	}
	if (bestAsset < 0) {
		*error = "The latest release does not contain a downloadable PPSSPP NRO.";
		return false;
	}
	return true;
}

struct MemoryDownload {
	std::string data;
	size_t limit = 0;
	std::atomic<bool> *cancel = nullptr;
};

size_t MemoryWrite(void *pointer, size_t size, size_t count, void *userdata) {
	if (count != 0 && size > std::numeric_limits<size_t>::max() / count)
		return 0;
	size_t bytes = size * count;
	auto &download = *(MemoryDownload *)userdata;
	if (download.cancel->load(std::memory_order_relaxed) || bytes > download.limit - download.data.size())
		return 0;
	download.data.append((const char *)pointer, bytes);
	return bytes;
}

int TransferProgress(void *userdata, curl_off_t, curl_off_t current, curl_off_t, curl_off_t) {
	auto *data = (std::pair<std::atomic<bool> *, std::atomic<uint64_t> *> *)userdata;
	if (current >= 0 && data->second)
		data->second->store((uint64_t)current, std::memory_order_relaxed);
	return data->first->load(std::memory_order_relaxed) ? 1 : 0;
}

void SetCommonCurlOptions(CURL *curl, std::pair<std::atomic<bool> *, std::atomic<uint64_t> *> *progress) {
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "PPSSPP-nx-Updater/" PPSSPP_SWITCH_RELEASE_VERSION);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, TransferProgress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, progress);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
}

bool FetchLatestRelease(std::atomic<bool> *cancel, ReleaseInfo *release, std::string *error) {
	CURL *curl = curl_easy_init();
	if (!curl) {
		*error = "Could not initialize the network request.";
		return false;
	}
	MemoryDownload download{{}, MAX_RELEASE_RESPONSE, cancel};
	std::pair<std::atomic<bool> *, std::atomic<uint64_t> *> progress{cancel, nullptr};
	char curlError[CURL_ERROR_SIZE]{};
	curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
	headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
	curl_easy_setopt(curl, CURLOPT_URL, LATEST_RELEASE_URL);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, MemoryWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)MAX_RELEASE_RESPONSE);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
	SetCommonCurlOptions(curl, &progress);
	CURLcode result = curl_easy_perform(curl);
	long responseCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (cancel->load(std::memory_order_relaxed)) {
		*error = "Update check cancelled.";
		return false;
	}
	if (result != CURLE_OK) {
		*error = curlError[0] ? curlError : curl_easy_strerror(result);
		return false;
	}
	if (responseCode == 404) {
		*error = "No published release is available yet.";
		return false;
	}
	if (responseCode == 403) {
		*error = "GitHub refused the request or its anonymous rate limit was reached.";
		return false;
	}
	if (responseCode < 200 || responseCode >= 300) {
		*error = "GitHub returned HTTP " + std::to_string(responseCode) + ".";
		return false;
	}
	return ParseRelease(download.data, release, error);
}

bool ParseSHA256(const std::string &digest, std::array<u8, SHA256_HASH_SIZE> *output) {
	constexpr std::string_view prefix = "sha256:";
	if (digest.size() != prefix.size() + SHA256_HASH_SIZE * 2 || !StartsWith(Lower(digest), prefix))
		return false;
	for (size_t index = 0; index < output->size(); ++index) {
		int high = HexDigit(digest[prefix.size() + index * 2]);
		int low = HexDigit(digest[prefix.size() + index * 2 + 1]);
		if (high < 0 || low < 0)
			return false;
		(*output)[index] = (u8)((high << 4) | low);
	}
	return true;
}

bool ValidNRO(const std::string &path, uint64_t expectedSize) {
	struct stat fileStat{};
	if (stat(path.c_str(), &fileStat) != 0 || fileStat.st_size <= 0 ||
		(expectedSize != 0 && (uint64_t)fileStat.st_size != expectedSize)) {
		return false;
	}
	FILE *file = std::fopen(path.c_str(), "rb");
	if (!file)
		return false;
	NroStart start{};
	NroHeader header{};
	bool read = std::fread(&start, 1, sizeof(start), file) == sizeof(start) &&
		std::fread(&header, 1, sizeof(header), file) == sizeof(header);
	std::fclose(file);
	if (!read || header.magic != NROHEADER_MAGIC || header.size < sizeof(start) + sizeof(header) ||
		header.size > (u64)fileStat.st_size) {
		return false;
	}
	for (const auto &segment : header.segments) {
		if (segment.file_off > header.size || segment.size > header.size - segment.file_off)
			return false;
	}
	return true;
}

struct FileDownload {
	FILE *file = nullptr;
	Sha256Context hash{};
	uint64_t written = 0;
	uint64_t maximum = 0;
	bool writeFailed = false;
	std::atomic<bool> *cancel = nullptr;
	std::atomic<uint64_t> *downloaded = nullptr;
};

size_t FileWrite(void *pointer, size_t size, size_t count, void *userdata) {
	if (count != 0 && size > std::numeric_limits<size_t>::max() / count)
		return 0;
	size_t bytes = size * count;
	auto &download = *(FileDownload *)userdata;
	if (download.cancel->load(std::memory_order_relaxed) || bytes > download.maximum - download.written ||
		std::fwrite(pointer, 1, bytes, download.file) != bytes) {
		download.writeFailed = true;
		return 0;
	}
	sha256ContextUpdate(&download.hash, pointer, bytes);
	download.written += bytes;
	download.downloaded->store(download.written, std::memory_order_relaxed);
	return bytes;
}

bool HasDownloadSpace(const std::string &path, uint64_t size) {
	struct statvfs info{};
	if (statvfs(path.c_str(), &info) != 0)
		return true;
	unsigned __int128 available = (unsigned __int128)info.f_bavail * info.f_frsize;
	return available >= (unsigned __int128)size + 8 * 1024 * 1024;
}

bool DownloadRelease(const ReleaseInfo &release, const std::string &executablePath, std::atomic<bool> *cancel,
	std::atomic<uint64_t> *downloaded, std::string *error) {
	std::array<u8, SHA256_HASH_SIZE> expectedHash{};
	if (!ParseSHA256(release.assetDigest, &expectedHash)) {
		*error = "The GitHub release asset does not provide a valid SHA-256 digest.";
		return false;
	}
	if (!HasDownloadSpace(executablePath, release.assetSize)) {
		*error = "There is not enough free SD card space for this update.";
		return false;
	}
	std::string temporary = executablePath + ".update.tmp";
	std::remove(temporary.c_str());
	FILE *file = std::fopen(temporary.c_str(), "wb");
	if (!file) {
		*error = "Could not create the update file on the SD card.";
		return false;
	}
	FileDownload download{};
	download.file = file;
	download.maximum = release.assetSize;
	download.cancel = cancel;
	download.downloaded = downloaded;
	sha256ContextCreate(&download.hash);
	CURL *curl = curl_easy_init();
	if (!curl) {
		std::fclose(file);
		std::remove(temporary.c_str());
		*error = "Could not initialize the update download.";
		return false;
	}
	std::pair<std::atomic<bool> *, std::atomic<uint64_t> *> progress{cancel, downloaded};
	char curlError[CURL_ERROR_SIZE]{};
	curl_easy_setopt(curl, CURLOPT_URL, release.assetUrl.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FileWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
	curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)release.assetSize);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
	SetCommonCurlOptions(curl, &progress);
	CURLcode result = curl_easy_perform(curl);
	long responseCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
	curl_easy_cleanup(curl);
	bool fileOK = !download.writeFailed;
	if (std::fflush(file) != 0 || fsync(fileno(file)) != 0)
		fileOK = false;
	if (std::fclose(file) != 0)
		fileOK = false;
	fsdevCommitDevice("sdmc");
	if (cancel->load(std::memory_order_relaxed)) {
		std::remove(temporary.c_str());
		*error = "Update download cancelled.";
		return false;
	}
	if (result != CURLE_OK || responseCode < 200 || responseCode >= 300 || !fileOK) {
		std::remove(temporary.c_str());
		*error = curlError[0] ? curlError : "The update download failed.";
		return false;
	}
	std::array<u8, SHA256_HASH_SIZE> actualHash{};
	sha256ContextGetHash(&download.hash, actualHash.data());
	if (actualHash != expectedHash || !ValidNRO(temporary, release.assetSize)) {
		std::remove(temporary.c_str());
		*error = actualHash != expectedHash ? "The downloaded update failed SHA-256 verification." :
			"The downloaded update is not a valid NRO.";
		return false;
	}
	return true;
}

bool ReplaceExecutable(const std::string &executablePath, std::string *error) {
	std::string temporary = executablePath + ".update.tmp";
	std::string backup = executablePath + ".update.old";
	struct stat currentStat{};
	bool haveCurrent = lstat(executablePath.c_str(), &currentStat) == 0 && S_ISREG(currentStat.st_mode) && !S_ISLNK(currentStat.st_mode);
	if (!haveCurrent) {
		*error = "The installed PPSSPP NRO is missing or is not a regular file.";
		return false;
	}
	struct stat backupStat{};
	if (lstat(backup.c_str(), &backupStat) == 0 && std::remove(backup.c_str()) != 0) {
		*error = "Could not remove the previous update backup.";
		return false;
	}
	if (std::rename(executablePath.c_str(), backup.c_str()) != 0) {
		*error = "Could not preserve the installed PPSSPP NRO.";
		return false;
	}
	if (std::rename(temporary.c_str(), executablePath.c_str()) != 0) {
		std::rename(backup.c_str(), executablePath.c_str());
		fsdevCommitDevice("sdmc");
		*error = "Could not activate the downloaded update.";
		return false;
	}
	fsdevCommitDevice("sdmc");
	if (!ValidNRO(executablePath, 0)) {
		std::remove(executablePath.c_str());
		std::rename(backup.c_str(), executablePath.c_str());
		fsdevCommitDevice("sdmc");
		*error = "The installed update failed final validation.";
		return false;
	}
	return true;
}

std::string NormalizeTag(std::string tag) {
	while (!tag.empty() && std::isspace((unsigned char)tag.front()))
		tag.erase(tag.begin());
	while (!tag.empty() && std::isspace((unsigned char)tag.back()))
		tag.pop_back();
	if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V'))
		tag.erase(tag.begin());
	return tag;
}

bool ParseVersion(const std::string &tag, std::vector<uint64_t> *parts) {
	std::string normalized = NormalizeTag(tag);
	size_t position = 0;
	while (position < normalized.size()) {
		if (!std::isdigit((unsigned char)normalized[position]))
			break;
		uint64_t value = 0;
		while (position < normalized.size() && std::isdigit((unsigned char)normalized[position])) {
			unsigned digit = normalized[position++] - '0';
			if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
				return false;
			value = value * 10 + digit;
		}
		parts->push_back(value);
		if (position == normalized.size() || normalized[position] == '-' || normalized[position] == '+')
			break;
		if (normalized[position++] != '.')
			return false;
	}
	return !parts->empty();
}

bool IsNewer(const std::string &candidate, const std::string &installed) {
	std::string candidateTag = NormalizeTag(candidate);
	std::string installedTag = NormalizeTag(installed);
	if (candidateTag == installedTag)
		return false;
	std::vector<uint64_t> candidateParts;
	std::vector<uint64_t> installedParts;
	if (!ParseVersion(candidateTag, &candidateParts) || !ParseVersion(installedTag, &installedParts))
		return true;
	size_t count = std::max(candidateParts.size(), installedParts.size());
	for (size_t index = 0; index < count; ++index) {
		uint64_t candidatePart = index < candidateParts.size() ? candidateParts[index] : 0;
		uint64_t installedPart = index < installedParts.size() ? installedParts[index] : 0;
		if (candidatePart != installedPart)
			return candidatePart > installedPart;
	}
	return true;
}

}  // namespace

struct SwitchUpdateScreen::Impl {
	std::mutex mutex;
	std::thread worker;
	UpdateState state = UpdateState::Checking;
	ReleaseInfo release;
	std::string error;
	std::atomic<bool> cancel{false};
	std::atomic<uint64_t> downloaded{0};
	std::atomic<uint64_t> total{0};

	~Impl() {
		CancelAndJoin();
	}

	void CancelAndJoin() {
		cancel.store(true, std::memory_order_relaxed);
		if (worker.joinable())
			worker.join();
	}

	UpdateSnapshot Snapshot() {
		UpdateSnapshot snapshot;
		{
			std::scoped_lock lock(mutex);
			snapshot.state = state;
			snapshot.release = release;
			snapshot.error = error;
		}
		snapshot.downloaded = downloaded.load(std::memory_order_relaxed);
		snapshot.total = total.load(std::memory_order_relaxed);
		return snapshot;
	}

	void StartCheck() {
		CancelAndJoin();
		cancel.store(false, std::memory_order_relaxed);
		downloaded.store(0, std::memory_order_relaxed);
		total.store(0, std::memory_order_relaxed);
		{
			std::scoped_lock lock(mutex);
			state = UpdateState::Checking;
			release = {};
			error.clear();
		}
		worker = std::thread([this] {
			SetCurrentThreadAffinity(ThreadAffinityRole::IO);
			SetCurrentThreadName("SwitchUpdateCheck");
			ReleaseInfo found;
			std::string failure;
			bool success = FetchLatestRelease(&cancel, &found, &failure);
			std::scoped_lock lock(mutex);
			if (!success) {
				error = std::move(failure);
				state = cancel.load(std::memory_order_relaxed) ? UpdateState::Cancelled : UpdateState::Error;
				return;
			}
			release = std::move(found);
			state = IsNewer(release.tag, PPSSPP_SWITCH_RELEASE_VERSION) ? UpdateState::UpdateAvailable : UpdateState::UpToDate;
		});
	}

	void StartDownload() {
		ReleaseInfo selected;
		{
			std::scoped_lock lock(mutex);
			if (state != UpdateState::UpdateAvailable)
				return;
			selected = release;
		}
		CancelAndJoin();
		cancel.store(false, std::memory_order_relaxed);
		downloaded.store(0, std::memory_order_relaxed);
		total.store(selected.assetSize, std::memory_order_relaxed);
		{
			std::scoped_lock lock(mutex);
			state = UpdateState::Downloading;
			error.clear();
		}
		worker = std::thread([this, selected = std::move(selected)] {
			SetCurrentThreadAffinity(ThreadAffinityRole::IO);
			SetCurrentThreadName("SwitchUpdateDownload");
			std::string failure;
			bool success = DownloadRelease(selected, executablePath_, &cancel, &downloaded, &failure);
			std::scoped_lock lock(mutex);
			if (success) {
				state = UpdateState::ReadyToInstall;
			} else {
				error = std::move(failure);
				state = cancel.load(std::memory_order_relaxed) ? UpdateState::Cancelled : UpdateState::Error;
			}
		});
	}

	bool Install() {
		{
			std::scoped_lock lock(mutex);
			if (state != UpdateState::ReadyToInstall)
				return false;
			state = UpdateState::Installing;
		}
		if (worker.joinable())
			worker.join();
		std::string failure;
		bool success = ReplaceExecutable(executablePath_, &failure);
		{
			std::scoped_lock lock(mutex);
			if (success) {
				state = UpdateState::Installed;
			} else {
				error = std::move(failure);
				state = UpdateState::Error;
			}
		}
		return success;
	}
};

void SwitchUpdate_SetExecutablePath(std::string path) {
	executablePath_ = std::move(path);
}

bool SwitchUpdate_RecoverInstallation(std::string *error) {
	if (executablePath_.empty()) {
		*error = "The PPSSPP NRO path is unavailable.";
		return false;
	}
	std::string backup = executablePath_ + ".update.old";
	std::string temporary = executablePath_ + ".update.tmp";
	struct stat currentStat{};
	struct stat backupStat{};
	bool haveCurrent = lstat(executablePath_.c_str(), &currentStat) == 0;
	bool haveBackup = lstat(backup.c_str(), &backupStat) == 0;
	if (lstat(temporary.c_str(), &currentStat) == 0 && std::remove(temporary.c_str()) != 0) {
		*error = "Could not remove an incomplete update download.";
		return false;
	}
	if (!haveCurrent && haveBackup) {
		if (!ValidNRO(backup, 0) || std::rename(backup.c_str(), executablePath_.c_str()) != 0) {
			*error = "Could not restore PPSSPP after an interrupted update.";
			return false;
		}
		fsdevCommitDevice("sdmc");
		return true;
	}
	if (haveCurrent && haveBackup) {
		if (ValidNRO(executablePath_, 0)) {
			if (std::remove(backup.c_str()) != 0) {
				*error = "Could not remove the previous update backup.";
				return false;
			}
		} else {
			if (!ValidNRO(backup, 0) || std::remove(executablePath_.c_str()) != 0 ||
				std::rename(backup.c_str(), executablePath_.c_str()) != 0) {
				*error = "Could not roll back an invalid PPSSPP update.";
				return false;
			}
		}
		fsdevCommitDevice("sdmc");
	}
	return true;
}

SwitchUpdateScreen::SwitchUpdateScreen()
	: UI::PopupScreen(T(I18NCat::MAINSETTINGS, "PPSSPP Update")), impl_(new Impl()) {
	impl_->StartCheck();
}

SwitchUpdateScreen::~SwitchUpdateScreen() = default;

void SwitchUpdateScreen::CreatePopupContents(UI::ViewGroup *parent) {
	using namespace UI;
	auto di = GetI18NCategory(I18NCat::DIALOG);
	auto ms = GetI18NCategory(I18NCat::MAINSETTINGS);

	statusView_ = parent->Add(new TextView("", ALIGN_LEFT | ALIGN_VCENTER | FLAG_WRAP_TEXT, false,
		new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(12, 8))));

	releaseView_ = parent->Add(new TextView("", ALIGN_LEFT | FLAG_WRAP_TEXT, false,
		new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(12, 4))));
	releaseView_->SetBig(true);

	notesScroll_ = parent->Add(new ScrollView(ORIENT_VERTICAL,
		new LinearLayoutParams(FILL_PARENT, 240, Margins(12, 4))));
	LinearLayout *notes = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	notesView_ = notes->Add(new TextView("", ALIGN_LEFT | FLAG_WRAP_TEXT, false,
		new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(8))));
	notesScroll_->Add(notes);

	progressBar_ = parent->Add(new ProgressBar(new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(12, 4))));

	LinearLayout *buttons = parent->Add(new LinearLayout(ORIENT_HORIZONTAL,
		new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(8, 4))));
	actionChoice_ = buttons->Add(new Choice(ms->T("Download update"), ImageID("I_DOWNLOAD"), new LinearLayoutParams(1.0f, Margins(4))));
	actionChoice_->OnClick.Handle(this, &SwitchUpdateScreen::OnAction);
	closeChoice_ = buttons->Add(new Choice(di->T("Cancel"), ImageID("I_NAVIGATE_BACK"), new LinearLayoutParams(1.0f, Margins(4))));
	closeChoice_->OnClick.Handle(this, &SwitchUpdateScreen::OnClose);

	Refresh();
}

bool SwitchUpdateScreen::CanComplete(DialogResult result) {
	return impl_->Snapshot().state != UpdateState::Installing;
}

void SwitchUpdateScreen::OnCompleted(DialogResult result) {
	impl_->cancel.store(true, std::memory_order_relaxed);
}

void SwitchUpdateScreen::OnAction(UI::EventParams &e) {
	UpdateState state = impl_->Snapshot().state;
	if (state == UpdateState::UpdateAvailable) {
		impl_->StartDownload();
	} else if (state == UpdateState::UpToDate || state == UpdateState::Error || state == UpdateState::Cancelled) {
		impl_->StartCheck();
	}
	Refresh();
}

void SwitchUpdateScreen::OnClose(UI::EventParams &e) {
	TriggerFinish(DR_CANCEL);
}

void SwitchUpdateScreen::Refresh() {
	if (!statusView_)
		return;
	using namespace UI;
	auto di = GetI18NCategory(I18NCat::DIALOG);
	auto ms = GetI18NCategory(I18NCat::MAINSETTINGS);
	UpdateSnapshot snapshot = impl_->Snapshot();
	bool busy = snapshot.state == UpdateState::Checking || snapshot.state == UpdateState::Downloading ||
		snapshot.state == UpdateState::Installing;
	bool showRelease = snapshot.state == UpdateState::UpdateAvailable || snapshot.state == UpdateState::Downloading ||
		snapshot.state == UpdateState::ReadyToInstall || snapshot.state == UpdateState::Installing ||
		snapshot.state == UpdateState::Installed;
	std::string status;
	switch (snapshot.state) {
	case UpdateState::Checking:
		status = ms->T("Checking GitHub for the latest release...");
		break;
	case UpdateState::UpdateAvailable:
		status = ApplySafeSubstitutions(ms->T("A newer version is available. Installed: %1"), PPSSPP_SWITCH_RELEASE_VERSION);
		break;
	case UpdateState::UpToDate:
		status = ApplySafeSubstitutions(ms->T("PPSSPP is up to date. Installed: %1"), PPSSPP_SWITCH_RELEASE_VERSION);
		break;
	case UpdateState::Downloading:
		status = ApplySafeSubstitutions(ms->T("Downloading %1"), snapshot.release.assetName);
		break;
	case UpdateState::ReadyToInstall:
		status = ms->T("Download verified. Installing update...");
		break;
	case UpdateState::Installing:
		status = ms->T("Installing update...");
		break;
	case UpdateState::Installed:
		status = ms->T("Update installed. Restarting PPSSPP...");
		break;
	case UpdateState::Cancelled:
		status = ms->T("Update cancelled.");
		break;
	case UpdateState::Error:
		status = snapshot.error.empty() ? ms->T("Update failed.") : snapshot.error;
		break;
	}
	statusView_->SetText(status);
	releaseView_->SetText(showRelease ? snapshot.release.name + " (" + snapshot.release.tag + ")" : "");
	releaseView_->SetVisibility(showRelease ? V_VISIBLE : V_GONE);
	notesView_->SetText(showRelease ? snapshot.release.notes : "");
	notesScroll_->SetVisibility(showRelease ? V_VISIBLE : V_GONE);
	progressBar_->SetVisibility(snapshot.state == UpdateState::Downloading ? V_VISIBLE : V_GONE);
	if (snapshot.state == UpdateState::Downloading && snapshot.total > 0)
		progressBar_->SetProgress((float)((double)snapshot.downloaded / (double)snapshot.total));

	actionChoice_->SetEnabled(!busy && snapshot.state != UpdateState::Installed && snapshot.state != UpdateState::ReadyToInstall);
	if (snapshot.state == UpdateState::UpdateAvailable) {
		actionChoice_->SetText(ms->T("Download update"));
	} else if (snapshot.state == UpdateState::Error || snapshot.state == UpdateState::Cancelled) {
		actionChoice_->SetText(ms->T("Try again"));
	} else if (snapshot.state == UpdateState::UpToDate) {
		actionChoice_->SetText(ms->T("Check again"));
	} else {
		actionChoice_->SetText(ms->T("Please wait"));
	}
	closeChoice_->SetText(busy ? di->T("Cancel") : di->T("Back"));
	closeChoice_->SetEnabled(snapshot.state != UpdateState::Installing);
}

void SwitchUpdateScreen::update() {
	UI::PopupScreen::update();
	UpdateSnapshot snapshot = impl_->Snapshot();
	if (snapshot.state == UpdateState::ReadyToInstall) {
		Refresh();
		if (impl_->Install() && !restartRequested_) {
			restartRequested_ = true;
			INFO_LOG(Log::System, "Installed PPSSPP update %s", snapshot.release.tag.c_str());
			System_RestartApp("");
		}
	}
	Refresh();
}

#endif
