// Copyright (c) 2012- PPSSPP Project.
// Launcher layout adapted from Cemu-NX and Dolphin-NX (GPL-2.0-or-later).
// SPDX-License-Identifier: GPL-2.0-or-later

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "SDL/SwitchLauncher.h"
#include "SDL/SwitchLauncherUtil.h"
#include "SDL/SwitchInputNames.h"
#include "Common/SwitchUIColors.h"
#include "Common/Common.h"
#include "Common/Log.h"
#include "Core/HLE/HLE.h"

#include <SDL3/SDL.h>
#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "SDL/SwitchText.h"
#include "Common/Render/AtlasGen.h"
#include "UI/UIAtlas.h"
#include "ext/jpge/jpgd.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/Data/Format/PNGLoad.h"
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/File/SwitchStorage.h"
#include "Common/StringUtils.h"
#include "Common/Data/Text/Parsers.h"
#include "Common/System/Request.h"
#include "Common/Thread/ThreadUtil.h"
#include "Core/Config.h"
#include "Core/RetroAchievements.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/KeyMap.h"
#include "Core/CwCheat.h"
#include "Core/FileLoaders/PS1EbootFileLoader.h"
#include <chrono>
#include "Core/HLE/sceUtility.h"
#include "Common/File/VFS/VFS.h"
#include "Core/KeyMapDefaults.h"
#include "SDL/SDLJoystick.h"
#include <sys/stat.h>
#include <unistd.h>
#include <curl/curl.h>
#include "Common/Data/Format/JSONReader.h"
#include "Common/File/PathBrowser.h"
#include "Core/WebServer.h"
#include "Core/HLE/sceUmd.h"
#include "ext/rcheevos/include/rc_client.h"
#include "Core/Loaders.h"
#include "GPU/Common/PostShader.h"
#include "UI/GamepadEmu.h"
#include "Common/Data/Text/WrapText.h"
#include "Core/Util/PathUtil.h"
#include "SDL/NKCodeFromSDL.h"
#include "Common/SwitchSaltyNX.h"
#include "Core/ConfigSettings.h"
#include "Core/System.h"
#include "Core/SaveState.h"
#include "Core/Util/RecentFiles.h"
#include "UI/CwCheatScreen.h"
#include "UI/FirmwareScreen.h"
#include "UI/GameInfoCache.h"
#include "UI/SwitchForwarderInstaller.h"
#include "UI/SwitchLibrary.h"
#include "UI/SwitchSteamGridDB.h"
#include "UI/SwitchUpdate.h"
#include "UI/GamePacker.h"
#include "Core/Util/PSARUnpack.h"
#include "Core/Util/GameManager.h"
#include "Common/Net/HTTPClient.h"

namespace SwitchLauncher {
namespace {

constexpr const char *CONFIG_PATH = "/switch/ppsspp-nx/launcher.ini";
constexpr const char *ART_DIRECTORY = "/switch/ppsspp-nx/covers";
constexpr int WIDTH = 1280;
constexpr int HEIGHT = 720;
constexpr size_t MAX_ARTWORK = 64;
constexpr size_t MAX_DECODED = 4;
constexpr size_t ARTWORK_UPLOAD_BYTES = 512 * 1024;
constexpr size_t MAX_TEXT_BYTES = 12 * 1024 * 1024;

bool LoadLauncherConfig(IniFile &config) { return config.Load(Path(CONFIG_PATH)) || config.Load(Path(std::string(CONFIG_PATH) + ".bak")); }

bool WriteLauncherConfig(IniFile &config) {
	const Path target(CONFIG_PATH);
	const Path temporary(std::string(CONFIG_PATH) + ".tmp");
	const Path backup(std::string(CONFIG_PATH) + ".bak");
	if (!config.Save(temporary)) {
		return false;
	}
	// Horizon's rename does not replace an existing file. Keep the old file until commit.
	if (File::Exists(target)) {
		if (File::Exists(backup) && !File::Delete(backup)) {
			return false;
		}
		if (!File::Rename(target, backup)) {
			return false;
		}
	}
	if (!File::Rename(temporary, target)) {
		if (File::Exists(backup)) {
			File::Rename(backup, target);
		}
		return false;
	}
	File::Delete(backup);
	return true;
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return value;
}

uint64_t Hash(std::string_view value) {
	uint64_t result = 14695981039346656037ULL;
	for (unsigned char c : value) {
		result = (result ^ c) * 1099511628211ULL;
	}
	return result;
}

std::string_view PlainTranslation(std::string_view text) {
	if (text.find('&') == std::string_view::npos) {
		return text;
	}
	static std::map<std::string, std::string> cache;
	static std::mutex mutex;
	std::lock_guard<std::mutex> guard(mutex);
	return cache.try_emplace(std::string(text), WithoutMnemonics(text)).first->second;
}

std::string_view Tr(std::string_view english) {
	const std::string key = "NX." + ReplaceAll(std::string(english), "\n", "\\n");
	const auto category = GetI18NCategory(I18NCat::MAINMENU);
	const auto &values = category->GetMap();
	const auto found = values.find(key);
	if (found != values.end()) {
		return PlainTranslation(found->second.text);
	}
	for (size_t i = 0; i < (size_t)I18NCat::CATEGORY_COUNT; ++i) {
		const auto category = GetI18NCategory((I18NCat)i);
		if (!category) {
			continue;
		}
		const auto &map = category->GetMap();
		const auto entry = map.find(english);
		if (entry != map.end()) {
			return PlainTranslation(entry->second.text);
		}
	}
	return PlainTranslation(english);
}

// Literals are UI text; strings and views can contain user titles and file paths.
class UiText {
public:
	UiText() = default;
	UiText(const char *value) : value_(Tr(value)) {}
	UiText(const std::string &value) : value_(value) {}
	UiText(std::string_view value) : value_(value) {}
	operator std::string_view() const { return value_; }

private:
	std::string_view value_;
};

uint8_t *DecodeArtwork(const std::string &data, int *width, int *height) {
	uint8_t *pixels = nullptr;
	if (data.empty()) {
		return nullptr;
	}
	pngLoadPtr((const uint8_t *)data.data(), data.size(), width, height, &pixels, 4096, 4096);
	if (!pixels && data.size() < INT_MAX) {
		jpgd::jpeg_decoder_mem_stream stream((const uint8_t *)data.data(), (unsigned)data.size());
		jpgd::jpeg_decoder header(&stream);
		int components = 0;
		if (header.get_error_code() == jpgd::JPGD_SUCCESS && header.get_width() <= 4096 && header.get_height() <= 4096) {
			pixels = jpgd::decompress_jpeg_image_from_memory((const uint8_t *)data.data(), (int)data.size(), width, height, &components, 4);
		}
	}
	if (*width <= 0 || *height <= 0 || *width > 4096 || *height > 4096) {
		free(pixels);
		return nullptr;
	}
	return pixels;
}

std::string ArtKey(const Path &path) {
	// Direct EBOOT selection and its game folder share artwork and launch history.
	const bool eboot = path.Type() == PathType::NATIVE && path.CanNavigateUp() && Lower(path.GetFilename()) == "eboot.pbp";
	const Path identity = eboot ? path.NavigateUp() : path;
	return StringFromFormat("%016llx", (unsigned long long)Hash(SwitchLibrary::MakeForwarderTarget(identity)));
}

std::string StableGameIdentity(const Path &path) {
	if (path.Type() != PathType::NATIVE) {
		return path.ToString();
	}
	// Normalize the native relative path before USB/SMB forwarders hex-encode it.
	return SwitchLibrary::MakeForwarderTarget(Path(NormalizeLibraryPath(path.ToString())));
}

bool SameOrBelow(const Path &source, const Path &target) {
	// SD paths have two spellings, and FAT/exFAT paths are case insensitive.
	auto normalized = [](const Path &path) {
		std::string result = Lower(path.ToString());
		if (result.rfind("sdmc:/", 0) == 0) {
			result.erase(0, 5);
		}
		while (result.size() > 1 && result.back() == '/') {
			result.pop_back();
		}
		return result;
	};
	const std::string a = normalized(source), b = normalized(target);
	if (a.empty() || b.empty()) {
		return true;
	}
	return a == b || (b.size() > a.size() && b.compare(0, a.size(), a) == 0 && (a.back() == '/' || b[a.size()] == '/'));
}

struct Entry {
	Path path;
	std::string identity;
	std::string key;
	std::string title;
	std::string id;
	bool customConfig = false;
	GameRegion region = GameRegion::UNKNOWN;
	std::shared_ptr<GameInfo> info;
};

struct Texture {
	SDL_Texture *texture = nullptr;
	uint64_t use = 0;
	size_t bytes = 0;
};

struct Decode {
	uint64_t revision = 0;
	std::string url;
	std::string key;
	std::string data;
	Path path;
	Path gamePath;
	std::shared_ptr<GameInfo> info;
};

struct Pixels {
	uint64_t revision = 0;
	std::string key;
	int width = 0;
	int height = 0;
	std::vector<uint8_t> data;
};

SDL_Color SdlColor(uint32_t color) { return {(uint8_t)color, (uint8_t)(color >> 8), (uint8_t)(color >> 16), (uint8_t)(color >> 24)}; }

std::string BindingName(const KeyMap::MultiInputMapping &binding) {
	std::string text;
	for (const auto &mapping : binding.mappings) {
		if (!text.empty()) {
			text += " + ";
		}
		if (const char *name = GamepadInputName(mapping)) {
			text += Tr(name);
		} else {
			if (mapping.deviceId == DEVICE_ID_KEYBOARD) {
				text += std::string(Tr("Keyboard")) + " ";
			}
			text += KeyMap::GetKeyOrAxisName(mapping);
		}
	}
	return text;
}

struct Card {
	std::string title, subtitle, metadata, description, image;
	Entry icon;
	SDL_Color status{};
	std::function<void()> open;
};

struct Row {
	Row() = default;
	Row(UiText labelText, UiText valueText, std::function<void(int)> action = {}, std::function<void()> restore = {}, bool active = true,
		std::string help = {})
		: label((std::string_view)labelText), value((std::string_view)valueText), change(std::move(action)), reset(std::move(restore)),
		  enabled(active), description(std::move(help)) {}
	std::string label;
	std::string value;
	std::function<void(int)> change;
	std::function<void()> reset;
	bool enabled = true;
	std::string description;
	SDL_Color valueColor{};
	SDL_Color labelColor{};
};

struct RowPage {
	Entry *preview = nullptr;
	int sectionStart = -1;
	std::string_view firstSection;
	std::string_view secondSection;
	std::string_view eyebrow = "PPSSPP";
	std::string context;
	bool updateButton = false;
	std::string imageUrl;
	std::function<bool(int, const SDL_Event &)> input;
	std::string hints;
	bool horizontalChanges = true;
	std::function<bool()> close;
};

class Launcher {
public:
	~Launcher();
	Result Run();

private:
	bool Init();
	bool LoadFonts();
	bool Frame();
	void Background();
	void Present();
	void Fill(float x, float y, float w, float h, SDL_Color color);
	void Round(float x, float y, float w, float h, float radius, SDL_Color color);
	void InitRoundTexture();
	void ImageFit(SDL_Texture *image, SDL_FRect rect, bool crop);
	SDL_Texture *RemoteImage(const std::string &url);
	void Message(UiText title, UiText text);
	void Flag(GameRegion region, float x, float y);
	void Panel(float x, float y, float w, float h, bool selected = false);
	void PageHeader(UiText title, UiText eyebrow, UiText context = {}, UiText detail = {});
	void SectionHeading(UiText title, float x, float y, float width);
	void Gradient(SDL_FRect rect, SDL_Color top, SDL_Color bottom);
	void Text(float x, float y, UiText value, int size = 26, SDL_Color color = {0, 0, 0, 0});
	void Center(float x, float y, UiText value, int size = 26, SDL_Color color = {0, 0, 0, 0});
	float TextWidth(UiText value, int size);
	void Footer(UiText value = {});
	void Icon(int kind, float x, float y, float size, SDL_Color color);
	void LoadLibrary();
	void RefreshMetadata();
	bool EnsureGameInfo(Entry &entry, bool requireID = true);
	void UpdateConfigBadge(Entry *entry);
	void MoveLibraryState(const Path &source, const Path &target);
	void ChangeLanguage();
	void FrameGeneration(Entry *entry = nullptr);
	void Shaders();
	void StructuredSetting(const ConfigSettingRef &ref);
	void FilterLibrary();
	void RenderLibrary();
	SDL_FRect CoverRect(int index) const;
	void Cover(Entry &entry, SDL_FRect rect, bool selected);
	SDL_Texture *Artwork(Entry &entry, bool background = false);
	void DecodeWorker();
	void PumpArtwork();
	void QueueArtwork(Entry &entry, bool background);
	void InvalidateArtwork(Entry &entry);
	bool Poll(SDL_Event *event, bool horizontalStick = false);
	int Navigate(const SDL_Event &event);
	bool Confirm(const SDL_Event &event) const;
	bool Back(const SDL_Event &event) const;
	bool Menu(const SDL_Event &event) const;
	bool Button(const SDL_Event &event, SDL_GamepadButton button, SDL_Keycode key) const;
	void Cards(UiText title, const std::function<std::vector<Card>()> &build);
	std::vector<std::string> Wrap(UiText text, float width, int size);
	void Wrapped(float x, float y, float width, std::string_view text, int size, int maxLines, SDL_Color color);
	int Rows(UiText title, const std::function<std::vector<Row>()> &build, int initial = -1, RowPage page = {});
	bool Ask(UiText title, UiText message);
	int Choose(UiText title, const std::vector<std::string> &choices, int initial = 0);
	bool Prompt(UiText title, std::string *value, bool password = false);
	std::string Browse(Path directory, bool folder, bool images = false, bool manage = false);
	void FileManager(Path directory = Path());
	void FileActions(const Path &path);
	bool CopyTree(const Path &source, const Path &target, const std::atomic<bool> &cancel);
	bool DeleteTree(const Path &path, const std::atomic<bool> &cancel);
	void Paste(const Path &directory);
	void EditShare(SwitchStorage::SmbShare share);
	void BatchCovers();
	void Controls(Entry *entry = nullptr);
	void ControllerProfiles();
	void Savedata(Entry *entry = nullptr);
	void Cheats(Entry &entry);
	void GameContent(Entry &entry);
	void CaptureBinding(int key, bool replace = true, int index = -1);
	void PerGameSettings(Entry &entry);
	void CreateShortcut(Entry &entry);
	bool PickShortcutIcon(Entry &entry, std::string *path);
	void Settings();
	void AchievementSettings(Entry *entry = nullptr);
	void FirmwareSettings();
	void TexturePacks(Entry &entry);
	void Compression(Entry &entry);
	void Appearance();
	void Updates();
	void LibrarySettings();
	void GameFolders();
	Path StorageLocation();
	bool Number(UiText title, float *value, float minimum, float maximum, float step, float defaultValue,
				const std::function<std::string(float)> &format = {});
	void ConfigSettings(std::string_view section, Entry *entry = nullptr);
	void GameMenu(Entry &entry);
	void CoverSettings(Entry &entry);
	int FinishArtworkPicker(int result);
	int ArtworkPicker(const std::vector<SteamGridDBArtwork> &artwork, bool background, bool icons = false, UiText gameName = {});
	void DownloadArtwork(Entry &entry, bool background);
	void ImportCover(Entry &entry);
	void LaunchFile(const Path &path);
	void Store();
	void RemoteDiscs();
	void AchievementBrowser(Entry *entry);
	void Leaderboard(uint32_t id, const std::string &title);
	void EditCheats(Entry &entry);
	void Savestates(Entry *entry);
	void GameInformation(Entry &entry);
	void LoadAtlasArt();
	bool Busy(UiText title, const std::function<void(const std::atomic<bool> &)> &work, bool cancelable = true);
	void Save();
	void SaveAppearance();
	void SaveGameSettings(const char *reason);
	void SelectGraphicsBackend(int backend);
	void Sound(int direction);

	SDL_Window *window_ = nullptr;
	SDL_Renderer *renderer_ = nullptr;
	SDL_Texture *roundTexture_ = nullptr;
	SDL_AudioStream *audio_ = nullptr;
	std::map<SDL_JoystickID, SDL_Gamepad *> pads_;
	std::vector<std::vector<uint8_t>> fontData_;
	std::unique_ptr<TextEngine> textEngine_;
	std::unordered_map<std::string, float> textWidths_;
	std::unordered_map<std::string, SDL_Texture *> atlasArt_;
	float fontScale_ = 1.f;
	struct Palette {
		SDL_Color text = SdlColor(SwitchUI::TEXT), dim = SdlColor(SwitchUI::DIM);
		SDL_Color value = SdlColor(SwitchUI::VALUE), accent = SdlColor(SwitchUI::ACCENT);
	} palette_;
	bool plReady_ = false;
	std::unordered_map<std::string, Texture> text_;
	size_t textBytes_ = 0;
	std::unordered_map<std::string, Texture> artwork_;
	std::set<std::string> pendingArt_;
	std::unordered_map<std::string, uint64_t> artRevisions_;
	std::deque<Decode> decodeQueue_;
	std::deque<Decode> metadataQueue_;
	std::deque<Pixels> decoded_;
	Pixels uploading_;
	SDL_Texture *uploadTexture_ = nullptr;
	int uploadRow_ = 0;
	std::mutex decodeMutex_;
	std::condition_variable decodeWake_;
	std::thread worker_;
	bool stopWorker_ = false;
	std::atomic<bool> cancelArtwork_{false};
	uint64_t use_ = 0;
	uint64_t libraryGeneration_ = UINT64_MAX;
	uint64_t metadataAt_ = 0;
	std::vector<Entry> games_;
	std::vector<int> visible_;
	std::set<std::string> favorites_;
	std::vector<std::string> recentKeys_;
	IniFile ini_;
	Section *appearance_ = nullptr;
	Path clipboard_;
	bool clipboardCut_ = false;
	std::atomic<uint64_t> transferred_{0};
	std::atomic<float> operationProgress_{-1.f};
	int packerGeneration_ = 0;
	std::mutex progressMutex_;
	std::string progress_;
	std::unique_ptr<SwitchUpdateTask> updateTask_;
	bool updateAtBoot_ = true;
	bool updateAvailable_ = false;
	bool showRegions_ = true;
	std::array<SDL_FRect, 3> tabRects_{};
	AppletOperationMode operationMode_ = AppletOperationMode_Handheld;
	bool classic_ = false;
	bool animations_ = true;
	bool titles_ = true;
	bool badges_ = true;
	bool running_ = true;
	bool headerFocus_ = false;
	int headerSelection_ = 1;
	int view_ = 1;
	int selection_ = 0;
	int columns_ = 6;
	int rows_ = 2;
	std::string search_;
	std::map<std::string, int> rowSelections_;
	std::array<float, 3> tabOpen_ = {0, 1, 0};
	float recentPosition_ = -1;
	StickNavigation stickNavigation_;
	uint64_t repeatAt_ = 0;
	int repeatDirection_ = 0;
	SDL_Keycode repeatKey_ = 0;
	float touchX_ = 0;
	float touchY_ = 0;
	uint64_t touchAt_ = 0;
	float touchDelta_ = 0;
	std::map<SDL_JoystickID, std::array<bool, 2>> libraryTriggers_;
	int pageDirection_ = 0;
	int tapSelection_ = -1;
	uint64_t tapAt_ = 0;
	bool presentFailureLogged_ = false;
	uint64_t startedAt_ = 0;
	Result result_;
};

static bool HasSettingUI(const ConfigSetting *setting) {
	return setting->Info().label || setting->type_ == ConfigSetting::Type::TYPE_TOUCH_POS ||
		   setting->type_ == ConfigSetting::Type::TYPE_CUSTOM_BUTTON;
}

static std::string SettingLabel(const ConfigSetting *setting) {
	const auto &info = setting->Info();
	if (info.label) {
		return std::string(PlainTranslation(g_i18nrepo.T(info.cat, info.label, info.labelDefault ? info.labelDefault : info.label)));
	}
	std::string key(setting->IniKey()), label;
	if (key.rfind("fcombo", 0) == 0) {
		return "Custom button " + StringFromInt(atoi(key.c_str() + 6) + 1);
	}
	if (setting->type_ == ConfigSetting::Type::TYPE_CUSTOM_BUTTON) {
		return "Custom button " + StringFromInt(atoi(key.c_str() + 6) + 1);
	}
	const std::map<std::string, const char *> controls{{"ActionButtonCenterX", "Action buttons"},
													   {"DPadX", "D-pad"},
													   {"StartKeyX", "Start"},
													   {"SelectKeyX", "Select"},
													   {"HomeKeyX", "Home"},
													   {"UnthrottleKeyX", "Fast-forward"},
													   {"LKeyX", "L trigger"},
													   {"RKeyX", "R trigger"},
													   {"PauseKeyX", "Pause"},
													   {"AnalogStickX", "Analog stick"},
													   {"RightAnalogStickX", "Right analog stick"}};
	const auto control = controls.find(key);
	if (control != controls.end()) {
		return std::string(Tr(control->second));
	}

	if (setting->type_ == ConfigSetting::Type::TYPE_TOUCH_POS && !key.empty() && key.back() == 'X') {
		key.pop_back();
	}
	for (size_t i = 0; i < key.size(); ++i) {
		if (i && std::isupper((unsigned char)key[i]) && std::islower((unsigned char)key[i - 1])) {
			label += ' ';
		}
		label += key[i];
	}
	return std::string(Tr(label));
}

static std::string SectionLabel(std::string_view section) {
	static const std::map<std::string_view, const char *> labels{
		{"General", "General"},
		{"CPU", "CPU / Emulation"},
		{"Graphics", "Graphics"},
		{"Textures", "Texture scaling"},
		{"Performance", "Speed hacks"},
		{"Speed", "Alternative speed"},
		{"Sound", "Audio"},
		{"Control", "Controller / Input"},
		{"SystemParam", "PSP system"},
		{"Network", "Networking"},
		{"Debugger", "Debugging"},
		{"JIT", "Recompiler"},
		{"Theme", "Interface"},
		{"Achievements", "RetroAchievements"},
		{"DisplayLayout.Landscape", "Display layout (landscape)"},
		{"DisplayLayout.Portrait", "Display layout (portrait)"},
		{"TouchControls.Landscape", "Touch controls (landscape)"},
		{"TouchControls.Portrait", "Touch controls (portrait)"},
		{"GestureControls.Left", "Left gestures"},
		{"GestureControls.Right", "Right gestures"},
	};
	const auto found = labels.find(section);
	return std::string(Tr(found == labels.end() ? section : std::string_view(found->second)));
}

static bool FetchUrl(const std::string &url, std::string *data, const std::atomic<bool> &cancel, size_t limit = 4 * 1024 * 1024) {
	CURL *curl = curl_easy_init();
	if (!curl) {
		return false;
	}
	struct Transfer {
		std::string *data;
		const std::atomic<bool> *cancel;
		size_t limit;
	} transfer{data, &cancel, limit};
	data->clear();
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 256L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "PPSSPP-NX/SDL");
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
	curl_easy_setopt(
		curl, CURLOPT_XFERINFOFUNCTION,
		+[](void *ptr, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int { return ((Transfer *)ptr)->cancel->load() ? 1 : 0; });
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
	curl_easy_setopt(
		curl, CURLOPT_WRITEFUNCTION, +[](char *ptr, size_t size, size_t count, void *userdata) -> size_t {
			auto *transfer = (Transfer *)userdata;
			if (count && size > SIZE_MAX / count) {
				return 0;
			}
			const size_t length = size * count;
			if (transfer->cancel->load() || length > transfer->limit - transfer->data->size()) {
				return 0;
			}
			transfer->data->append(ptr, length);
			return length;
		});
	const CURLcode result = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	return result == CURLE_OK && status == 200 && !cancel.load();
}

void Launcher::Store() {
	std::string listing;
	bool ok = false;
	Busy("Loading homebrew store", [&](const auto &cancel) { ok = FetchUrl("https://store.ppsspp.org/index.json", &listing, cancel); });
	json::JsonReader reader(listing.data(), listing.size());
	if (!ok || !reader.ok() || !reader.root()) {
		Message("Homebrew store", "Could not load the catalog. Check your network connection.");
		return;
	}
	const JsonNode *products = reader.root().getArray("entries");
	if (!products) {
		Message("Homebrew store", "The store returned an invalid catalog.");
		return;
	}
	const auto resolveUrl = [](std::string url) {
		if (url.empty() || startsWith(url, "https://") || startsWith(url, "http://")) {
			return url;
		}
		return std::string("https://store.ppsspp.org/") + (url[0] == '/' ? url.substr(1) : url);
	};
	struct Product {
		std::string name, description, author, file, download, icon, license, website, category;
		uint64_t size;
	};
	std::vector<Product> catalog;
	for (const auto *node : products->value) {
		json::JsonGet item = node->value;
		const auto translated = [&](const char *key) {
			json::JsonGet lang = item.getDict(g_Config.sLanguageIni.c_str());
			if (!lang || !lang.hasChild(key, JSON_STRING)) {
				lang = item.getDict("en_US");
			}
			return std::string(lang ? lang.getStringOr(key, "") : "");
		};
		const std::string file = item.getStringOr("file", "");
		if (file.empty() || HasParentDirComponent(file) || file.find_first_of("/:\\") != std::string::npos) {
			continue;
		}
		catalog.push_back({translated("name"), translated("description"), ReplaceAll(item.getStringOr("author", ""), "&&", "&"), file,
						   resolveUrl(item.getStringOr("download-url", "")), resolveUrl(item.getStringOr("icon-url", "")),
						   item.getStringOr("license", ""), item.getStringOr("website-url", ""), item.getStringOr("category", ""),
						   (uint64_t)std::max(0, item.getInt("size", 0))});
	}
	Cards("Homebrew store", [&] {
		std::vector<Card> cards;
		for (const auto &product : catalog) {
			Card card;
			card.title = product.name.empty() ? product.file : product.name;
			const bool installed = g_GameManager.IsGameInstalled(product.file);
			card.subtitle = product.author + (installed ? "  |  " + std::string(Tr("Installed")) : "");
			card.metadata = product.author + "  |  " + NiceSizeFormat(product.size) + "\n" + product.license;
			card.description = product.description;
			card.image = product.icon;
			card.status = installed ? SdlColor(SwitchUI::GOOD) : palette_.dim;
			card.open = [&, product] {
				Rows(product.name,
					 [&] {
						 std::vector<Row> actions{
							 {"Description", ">", [&, product](int) { Message(product.name, product.description); }},
							 {"Author", product.author},
							 {"Download size", NiceSizeFormat(product.size)},
							 {"License", product.license},
							 {"Website", product.website},
						 };
						 const bool installed = g_GameManager.IsGameInstalled(product.file);
						 actions.push_back(
							 {installed ? "Reinstall" : "Install", ">", [&, product, installed](int) {
								  if (g_GameManager.GetState() != GameManagerState::IDLE) {
									  Message("Another operation is active", "Please wait...");
									  return;
								  }
								  if (installed && !Ask("Replace existing files?", product.name)) {
									  return;
								  }
								  const std::string url = product.download.empty()
															  ? "https://store.ppsspp.org/files/" + product.file + ".zip"
															  : product.download;
								  g_GameManager.ResetInstallError();
								  if (!g_GameManager.DownloadAndInstall(url)) {
									  Message("Installation failed", product.name);
									  return;
								  }
								  while (Frame() && g_GameManager.GetState() != GameManagerState::IDLE) {
									  SDL_Event event{};
									  while (Poll(&event)) {
										  if (Back(event)) {
											  g_GameManager.CancelDownload();
										  }
									  }
									  Background();
									  Center(WIDTH / 2, 260, product.name, 30);
									  Center(WIDTH / 2, 320,
											 StringFromFormat("%.1f%%", g_GameManager.GetCurrentInstallProgressPercentage() * 100.f), 24);
									  Footer(g_GameManager.GetState() == GameManagerState::DOWNLOADING ? "B  Cancel" : "Please wait...");
									  Present();
								  }
								  const auto error = g_GameManager.GetInstallError();
								  if (!error.empty()) {
									  Message("Installation failed", error);
								  }
								  SwitchLibrary::RequestScan();
							  }});
						 if (installed) {
							 actions.push_back(
								 {"Launch", ">", [&, product](int) { LaunchFile(GetSysDirectory(DIRECTORY_GAME) / product.file); }});
							 actions.push_back({"Uninstall", ">", [&, product](int) {
													if (!Ask("Permanently delete game?", product.name)) {
														return;
													}
													if (!g_GameManager.UninstallGameOnThread(product.file)) {
														Message("Could not delete game", product.name);
														return;
													}
													while (Frame() && g_GameManager.GetState() != GameManagerState::IDLE) {
														SDL_Event event{};
														while (Poll(&event)) {
														}
														Background();
														Center(WIDTH / 2, HEIGHT / 2, "Deleting game", 28);
														Footer("Please wait...");
														Present();
													}
													SwitchLibrary::RequestScan();
												}});
						 }
						 return actions;
					 },
					 -1, {nullptr, -1, {}, {}, "Homebrew store", product.category, false, product.icon});
			};
			cards.push_back(std::move(card));
		}
		return cards;
	});
}

void Launcher::RemoteDiscs() {
	Rows("Remote disc streaming", [&] {
		const bool sharing = !WebServerStopped(WebServerFlags::DISCS);
		return std::vector<Row>{
			{"Browse games", ">",
			 [&](int) {
				 std::vector<std::pair<std::string, int>> candidates;
				 if (!g_Config.sLastRemoteISOServer.empty()) {
					 candidates.emplace_back(g_Config.sLastRemoteISOServer, g_Config.iLastRemoteISOPort);
				 }
				 std::string host;
				 int port = 0;
				 std::string url;
				 bool found = false;
				 Busy("Looking for shared games", [&](const auto &cancel) {
					 if (!g_Config.bRemoteISOManual) {
						 std::string peers;
						 if (FetchUrl("http://report.ppsspp.org/match/list", &peers, cancel)) {
							 json::JsonReader reader(peers.data(), peers.size());
							 if (reader.ok() && reader.rootArray().getTag() == JSON_ARRAY) {
								 for (const auto *node : reader.rootArray()) {
									 json::JsonGet peer = node->value;
									 candidates.emplace_back(peer.getStringOr("ip", ""), peer.getInt("p", 0));
								 }
							 }
						 }
					 }
					 for (const auto &candidate : candidates) {
						 if (cancel || candidate.first.empty() || candidate.second < 1 || candidate.second > 65535) {
							 continue;
						 }
						 const std::string subdir = g_Config.bRemoteISOManual ? g_Config.sRemoteISOSubdir : "/";
						 const std::string path = subdir.empty() || subdir[0] != '/' ? "/" + subdir : subdir;
						 const std::string address =
							 StringFromFormat("http://%s:%d%s", candidate.first.c_str(), candidate.second, path.c_str());
						 std::string response;
						 if (FetchUrl(address, &response, cancel)) {
							 host = candidate.first;
							 port = candidate.second;
							 url = address;
							 found = true;
							 break;
						 }
					 }
				 });
				 if (!found) {
					 Message("No server found", "Start Share Games on another PPSSPP device, or configure the server manually.");
					 return;
				 }
				 g_Config.sLastRemoteISOServer = host;
				 g_Config.iLastRemoteISOPort = port;
				 PathBrowser browser;
				 Path current(url);
				 for (;;) {
					 browser.SetPath(current);
					 while (Frame() && !browser.IsListingReady()) {
						 SDL_Event event{};
						 while (Poll(&event)) {
							 if (Back(event)) {
								 return;
							 }
						 }
						 Background();
						 Center(WIDTH / 2, HEIGHT / 2, "Loading shared games...", 28);
						 Footer("B  Cancel");
						 Present();
					 }
					 std::vector<File::FileInfo> files;
					 browser.GetListing(files, "iso:cso:chd:pbp:elf:prx:ppst:");
					 std::vector<Row> rows;
					 if (current != Path(url) && current.CanNavigateUp()) {
						 rows.push_back({"Parent folder", ".."});
					 }
					 const int offset = (int)rows.size();
					 for (const auto &file : files) {
						 rows.push_back({file.name, file.isDirectory ? ">" : ""});
					 }
					 const int selected = Rows(current.ToVisualString(), [&] { return rows; });
					 if (selected < 0) {
						 break;
					 }
					 if (selected < offset) {
						 current = current.NavigateUp();
						 continue;
					 }
					 const auto &file = files[selected - offset];
					 if (file.isDirectory) {
						 current = file.fullName;
					 } else {
						 LaunchFile(file.fullName);
						 return;
					 }
				 }
			 }},
			{sharing ? "Stop sharing" : "Share games", ">",
			 [&, sharing](int) {
				 if (sharing) {
					 StopWebServer(WebServerFlags::DISCS);
				 } else {
					 StartWebServer(WebServerFlags::DISCS);
				 }
			 }},
			{"Share at startup", g_Config.bRemoteShareOnStartup ? "On" : "Off",
			 [&](int) { g_Config.bRemoteShareOnStartup = !g_Config.bRemoteShareOnStartup; }},
			{"Shared files", g_Config.iRemoteISOShareType == (int)RemoteISOShareType::RECENT ? "Recent games" : "Selected folder",
			 [&](int) {
				 g_Config.iRemoteISOShareType = g_Config.iRemoteISOShareType == (int)RemoteISOShareType::RECENT
													? (int)RemoteISOShareType::LOCAL_FOLDER
													: (int)RemoteISOShareType::RECENT;
			 }},
			{"Shared folder", g_Config.sRemoteISOSharedDir,
			 [&](int) {
				 const std::string folder = Browse(Path(), true);
				 if (!folder.empty()) {
					 g_Config.sRemoteISOSharedDir = folder;
				 }
			 }},
			{"Manual connection", g_Config.bRemoteISOManual ? "On" : "Off",
			 [&](int) { g_Config.bRemoteISOManual = !g_Config.bRemoteISOManual; }},
			{"Server address", g_Config.sLastRemoteISOServer, [&](int) { Prompt("Server address", &g_Config.sLastRemoteISOServer); }},
			{"Server port", StringFromInt(g_Config.iLastRemoteISOPort),
			 [&](int) {
				 std::string value = StringFromInt(g_Config.iLastRemoteISOPort);
				 int port = 0;
				 if (Prompt("Server port", &value) && TryParse(value, &port) && port >= 1 && port <= 65535) {
					 g_Config.iLastRemoteISOPort = port;
				 }
			 }},
			{"Server subfolder", g_Config.sRemoteISOSubdir, [&](int) { Prompt("Server subfolder", &g_Config.sRemoteISOSubdir); }},
			{"Local server port", StringFromInt(g_Config.iRemoteISOPort),
			 [&](int) {
				 if (WebServerStopped(WebServerFlags::DISCS)) {
					 std::string value = StringFromInt(g_Config.iRemoteISOPort);
					 int port = 0;
					 if (Prompt("Local server port", &value) && TryParse(value, &port) && port >= 1 && port <= 65535) {
						 g_Config.iRemoteISOPort = port;
					 }
				 }
			 }},
		};
	});
}

void Launcher::LoadAtlasArt() {
	Image image;
	if (!RasterizeSVGImage("ui_images/images.svg", "I_LOGO", fontScale_, &image)) {
		ERROR_LOG(Log::System, "Could not load the PPSSPP logo");
		return;
	}
	SDL_Texture *texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, image.width(), image.height());
	if (texture) {
		SDL_UpdateTexture(texture, nullptr, image.data(), image.width() * 4);
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
		atlasArt_["I_LOGO"] = texture;
	}
}

void Launcher::StructuredSetting(const ConfigSettingRef &ref) {
	const auto *setting = ref.setting;
	ConfigTouchPos defaultPosition{};
	if (setting->type_ == ConfigSetting::Type::TYPE_TOUCH_POS) {
		const auto defaults = ref.block->NewDefault(ref.section);
		defaultPosition = *(const ConfigTouchPos *)setting->GetVoidPtr(defaults.get());
	}
	void *ptr = const_cast<void *>(setting->GetVoidPtr(ref.block));
	const std::string title = SettingLabel(setting);
	Rows(title, [&] {
		std::vector<Row> rows;
		auto number = [&](UiText label, float *value, float minimum, float maximum, float step, float fallback) {
			rows.push_back({label, StringFromFormat("%.2f", *value), [this, label, value, minimum, maximum, step, fallback](int direction) {
								if (!direction) {
									Number(label, value, minimum, maximum, step, fallback);
								} else {
									*value = std::clamp(*value + direction * step, minimum, maximum);
								}
							}});
		};
		if (setting->type_ == ConfigSetting::Type::TYPE_TOUCH_POS) {
			auto *position = (ConfigTouchPos *)ptr;
			number("Horizontal position", &position->x, -1.f, 1.f, .01f, defaultPosition.x);
			number("Vertical position", &position->y, -1.f, 1.f, .01f, defaultPosition.y);
			number("Scale", &position->scale, .25f, 3.f, .05f, defaultPosition.scale);
			rows.push_back({"Show", position->show ? "On" : "Off", [position](int) { position->show = !position->show; }});
		} else {
			auto *button = (ConfigCustomButton *)ptr;
			CustomKeyData::Sanitize(*button);
			rows.push_back({"Toggle", button->toggle ? "On" : "Off", [button](int) { button->toggle = !button->toggle; }});
			rows.push_back({"Repeat", button->repeat ? "On" : "Off", [button](int) { button->repeat = !button->repeat; }});
			rows.push_back({"Icon", StringFromInt(button->image + 1), [button](int d) {
								button->image = (button->image + (d ? d : 1) + (int)ARRAY_SIZE(CustomKeyData::customKeyImages)) %
												(int)ARRAY_SIZE(CustomKeyData::customKeyImages);
							}});
			rows.push_back({"Shape", StringFromInt(button->shape + 1), [button](int d) {
								button->shape = (button->shape + (d ? d : 1) + (int)ARRAY_SIZE(CustomKeyData::customKeyShapes)) %
												(int)ARRAY_SIZE(CustomKeyData::customKeyShapes);
							}});
			for (size_t i = 0; i < ARRAY_SIZE(CustomKeyData::g_customKeyList); ++i) {
				const uint64_t mask = UINT64_C(1) << i;
				const std::string label(
					GetI18NCategory(I18NCat::MAPPABLECONTROLS)->T(KeyMap::GetPspButtonName(CustomKeyData::g_customKeyList[i].c)));
				rows.push_back({label, (button->key & mask) ? "On" : "Off", [button, mask](int) { button->key ^= mask; }});
			}
		}
		rows.push_back({"Restore defaults", ">", [ptr](int) { Config::RestoreSettingToDefault(ptr); }});
		return rows;
	});
}

void Launcher::GameInformation(Entry &entry) {
	if (!EnsureGameInfo(entry, false)) {
		return;
	}
	const auto flags = GameInfoFlags::SIZE | GameInfoFlags::UNCOMPRESSED_SIZE | GameInfoFlags::SAVEDATA_SIZE;
	entry.info = g_gameInfoCache->GetInfo(nullptr, entry.path, flags);
	Busy("Reading game information", [&](const auto &) { entry.info->WaitUntilReady(flags); }, false);
	std::vector<Row> rows;
	const std::string title = entry.info->GetTitle();
	{
		std::lock_guard<std::mutex> lock(entry.info->lock);
		rows = {{"Name", title},
				{"Game ID", entry.info->id},
				{"Version", entry.info->id_version},
				{"Game file", entry.path.ToVisualString()},
				{"File size", NiceSizeFormat(entry.info->gameSizeOnDisk)},
				{"Uncompressed size", NiceSizeFormat(entry.info->gameSizeUncompressed)},
				{"Savedata", NiceSizeFormat(entry.info->saveDataSize)},
				{"Installed data", NiceSizeFormat(entry.info->installDataSize)}};
	}
	Rows("Game information", [&] { return rows; }, -1, {&entry, -1, "Game information"});
}

void Launcher::Savestates(Entry *entry) {
	Cards("Savestates", [&] {
		std::vector<File::FileInfo> files;
		File::GetFilesInDir(GetSysDirectory(DIRECTORY_SAVESTATE), &files, "ppst:");
		std::vector<Card> cards;
		for (const auto &file : files) {
			if (entry && !entry->id.empty() && file.name.rfind(entry->id, 0) != 0) {
				continue;
			}
			Card card;
			card.title = file.name;
			card.subtitle = NiceSizeFormat(file.size);
			card.icon.info = g_gameInfoCache->GetInfo(nullptr, file.fullName, GameInfoFlags::PARAM_SFO | GameInfoFlags::SIZE);
			if (card.icon.info->Ready(GameInfoFlags::PARAM_SFO | GameInfoFlags::SIZE)) {
				card.title = card.icon.info->GetTitle();
				std::lock_guard<std::mutex> guard(card.icon.info->lock);
				card.metadata = file.name + "\n" + card.icon.info->GetMTime();
			}
			card.image = file.fullName.WithReplacedExtension(".jpg").ToString();
			card.open = [&, path = file.fullName] {
				const int action =
					Rows("Savestate", [] { return std::vector<Row>{{"Launch", ">"}, {"Export / back up", ">"}, {"Delete", ">"}}; });
				if (action == 0) {
					LaunchFile(path);
				} else if (action == 1) {
					const std::string folder = Browse(Path(), true);
					if (!folder.empty()) {
						clipboard_ = path;
						clipboardCut_ = false;
						Paste(Path(folder));
					}
				} else if (action == 2 && Ask("Permanently delete?", path.ToVisualString())) {
					File::Delete(path);
					for (const Path &companion : SaveState::GetCompanionFilePaths(path)) {
						File::Delete(companion);
					}
				}
			};
			cards.push_back(std::move(card));
		}
		return cards;
	});
}

void Launcher::EditCheats(Entry &entry) {
	if (!EnsureGameInfo(entry)) {
		return;
	}
	CWCheatEngine engine(entry.id);
	engine.CreateCheatFile();
	Rows("Cheat database", [&] {
		return std::vector<Row>{
			{"Import from cheat.db", ">",
			 [&](int) {
				 int count = 0;
				 const bool ok =
					 ImportCheatsFromDatabase(GetSysDirectory(DIRECTORY_CHEATS) / "cheat.db", entry.id, engine.CheatFilename(), &count);
				 Message("Import cheats",
						 ok ? StringFromFormat("Imported %d cheat lines", count) : "Could not import cheats for this game.");
				 g_Config.bReloadCheats = true;
			 }},
			{"Import cheat file", ">",
			 [&](int) {
				 const std::string file = Browse(GetSysDirectory(DIRECTORY_CHEATS), false);
				 if (file.empty()) {
					 return;
				 }
				 int count = 0;
				 if (!ImportCheatsFromDatabase(Path(file), entry.id, engine.CheatFilename(), &count)) {
					 Message("Import cheats", "Could not import cheats for this game.");
				 }
				 g_Config.bReloadCheats = true;
			 }},
			{"Edit cheat file", ">",
			 [&](int) {
				 std::string contents;
				 if (!File::ReadTextFileToString(engine.CheatFilename(), &contents)) {
					 return;
				 }
				 std::vector<std::string> lines;
				 SplitString(contents, '\n', lines);
				 Rows("Edit cheat file", [&] {
					 std::vector<Row> rows;
					 for (size_t i = 0; i < lines.size(); ++i) {
						 rows.push_back({StringFromInt((int)i + 1), lines[i], [&, i](int) { Prompt("Edit cheat line", &lines[i]); }});
					 }
					 rows.push_back({"Add line", "+", [&](int) {
										 std::string line;
										 if (Prompt("Cheat line", &line)) {
											 lines.push_back(line);
										 }
									 }});
					 return rows;
				 });
				 std::string updated;
				 for (const auto &line : lines) {
					 updated += line + "\n";
				 }
				 if (updated != contents && Ask("Save cheat file?", entry.title)) {
					 const Path temp(engine.CheatFilename().ToString() + ".tmp");
					 if (!File::WriteStringToFile(false, updated, temp) || !File::Rename(temp, engine.CheatFilename())) {
						 Message("Could not save cheat", entry.title);
					 }
					 g_Config.bReloadCheats = true;
				 }
			 }},
			{"Download cheat database", ">",
			 [&](int) {
				 std::string data;
				 bool ok = false;
				 Busy("Downloading cheat database", [&](const auto &cancel) {
					 ok = FetchUrl("https://raw.githubusercontent.com/Saramagrean/CWCheat-Database-Plus-/master/cheat.db", &data, cancel,
								   32 * 1024 * 1024);
				 });
				 const Path path = GetSysDirectory(DIRECTORY_CHEATS) / "cheat.db";
				 if (!ok || data.find("_S ") == std::string::npos || data.find("_G ") == std::string::npos) {
					 Message("Download failed", "No valid cheat database was received.");
					 return;
				 }
				 if (File::Exists(path) && !Ask("Replace cheat database?", path.ToVisualString())) {
					 return;
				 }
				 const Path temporary(path.ToString() + ".tmp");
				 if (!File::WriteStringToFile(false, data, temporary) || !File::Rename(temporary, path)) {
					 Message("Download failed", path.ToVisualString());
				 }
			 }},
		};
	});
}

void Launcher::Leaderboard(uint32_t id, const std::string &title) {
	uint32_t first = 1;
	bool aroundUser = false;
	while (Frame()) {
		struct State {
			bool done = false;
			uint32_t total = 0;
			std::string error;
			std::vector<Row> rows;
		} state;
		auto callback = +[](int result, const char *error, rc_client_leaderboard_entry_list_t *list, rc_client_t *, void *userdata) {
			auto *state = (State *)userdata;
			if (error) {
				state->error = error;
			}
			if (list) {
				state->total = list->total_entries;
				for (uint32_t i = 0; i < list->num_entries; ++i) {
					const auto &entry = list->entries[i];
					state->rows.push_back({StringFromFormat("%u. %s", entry.rank, entry.user ? entry.user : ""), entry.display});
				}
				rc_client_destroy_leaderboard_entry_list(list);
			}
			state->done = true;
		};
		rc_client_t *client = Achievements::GetClient();
		auto *request = aroundUser ? rc_client_begin_fetch_leaderboard_entries_around_user(client, id, 25, callback, &state)
								   : rc_client_begin_fetch_leaderboard_entries(client, id, first, 25, callback, &state);
		if (!request && !state.done) {
			Message(title, "Could not load leaderboard scores.");
			return;
		}
		bool cancelled = false;
		while (Frame() && !state.done && !cancelled) {
			SDL_Event event{};
			while (Poll(&event)) {
				if (Back(event)) {
					cancelled = true;
				}
			}
			Background();
			Center(WIDTH / 2, HEIGHT / 2, "Loading leaderboard...", 28);
			Footer("B  Cancel");
			Present();
		}
		if (!state.done) {
			rc_client_abort_async(client, request);
			return;
		}
		if (!state.error.empty()) {
			Message(title, state.error);
			return;
		}
		const int scores = (int)state.rows.size();
		state.rows.push_back({"Around my score", ""});
		state.rows.push_back({"Previous page", ""});
		state.rows.push_back({"Next page", ""});
		const int choice = Rows(title, [&] { return state.rows; });
		if (choice < 0 || choice < scores) {
			return;
		}
		if (choice == scores) {
			aroundUser = true;
		} else if (choice == scores + 1) {
			aroundUser = false;
			first = first > 25 ? first - 25 : 1;
		} else {
			aroundUser = false;
			if (first + 25 <= state.total) {
				first += 25;
			}
		}
	}
}

void Launcher::AchievementBrowser(Entry *entry) {
	if (!entry) {
		std::vector<Row> rows;
		for (const auto &game : games_) {
			rows.push_back({game.title, ""});
		}
		const int selected = Rows("Choose game", [&] { return rows; });
		if (selected >= 0) {
			AchievementBrowser(&games_[selected]);
		}
		return;
	}
	if (!EnsureGameInfo(*entry) || !Achievements::IsLoggedIn()) {
		Message("Achievements", "Log in to RetroAchievements first.");
		return;
	}
	const auto loader = entry->info->GetFileLoader();
	Achievements::SetGame(entry->path, entry->info->fileType, loader.get());
	while (Frame() && Achievements::IsBlockingExecution()) {
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event)) {
				Achievements::UnloadGame();
				return;
			}
		}
		Background();
		Center(WIDTH / 2, HEIGHT / 2, "Loading achievements...", 28);
		Footer("B  Cancel");
		Present();
	}
	rc_client_t *client = Achievements::GetClient();
	if (!client) {
		return;
	}
	Rows("Achievements and leaderboards", [&] {
		return std::vector<Row>{
			{"Achievements", ">",
			 [&](int) {
				 auto *list = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
																RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
				 std::vector<Row> rows;
				 if (list) {
					 for (uint32_t b = 0; b < list->num_buckets; ++b) {
						 for (uint32_t i = 0; i < list->buckets[b].num_achievements; ++i) {
							 const auto *achievement = list->buckets[b].achievements[i];
							 const std::string title = achievement->title, detail = achievement->description;
							 rows.push_back({title,
											 StringFromFormat("%u pts - %s", achievement->points,
															  std::string(Tr(achievement->unlocked ? "Unlocked" : "Locked")).c_str()),
											 [this, title, detail](int) { Message(title, detail); }});
						 }
					 }
					 rc_client_destroy_achievement_list(list);
				 }
				 if (rows.empty()) {
					 rows.push_back({"No achievements available", "", {}, {}, false});
				 }
				 Rows("Achievements", [&] { return rows; });
			 }},
			{"Leaderboards", ">",
			 [&](int) {
				 auto *list = rc_client_create_leaderboard_list(client, RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
				 std::vector<Row> rows;
				 if (list) {
					 for (uint32_t b = 0; b < list->num_buckets; ++b) {
						 for (uint32_t i = 0; i < list->buckets[b].num_leaderboards; ++i) {
							 const auto *board = list->buckets[b].leaderboards[i];
							 const std::string title = board->title, detail = board->description;
							 rows.push_back({title, "", [this, title, id = board->id](int) { Leaderboard(id, title); }});
						 }
					 }
					 rc_client_destroy_leaderboard_list(list);
				 }
				 if (rows.empty()) {
					 rows.push_back({"No leaderboards available", "", {}, {}, false});
				 }
				 Rows("Leaderboards", [&] { return rows; });
			 }},
		};
	});
	Achievements::UnloadGame();
}

void Launcher::LaunchFile(const Path &path) {
	const std::string extension = Lower(path.GetFileExtension());
	if (extension == ".zip" || extension == ".7z" || extension == ".pkg") {
		const int action = Rows("Install content", [&] { return std::vector<Row>{{"Install", path.GetFilename()}, {"Cancel", ""}}; }, 1);
		if (action != 0) {
			return;
		}
		g_GameManager.ResetInstallError();
		bool started = false;
		if (extension == ".pkg") {
			started = g_GameManager.InstallPkgOnThread(path, false);
		} else {
			ZipFileTask task{};
			task.url = path;
			task.fileName = path;
			task.deleteAfter = false;
			started = g_GameManager.InstallZipOnThread(task);
		}
		if (!started) {
			Message("Another operation is active", "Wait for the current compression or installation to finish.");
			return;
		}
		while (Frame() && g_GameManager.GetState() != GameManagerState::IDLE) {
			SDL_Event event{};
			while (Poll(&event)) {
				if (Back(event)) {
					g_GameManager.CancelDownload();
				}
			}
			Background();
			Center(WIDTH / 2, 260, "Installing content...", 30);
			Center(WIDTH / 2, 320, StringFromFormat("%.1f%%", g_GameManager.GetCurrentInstallProgressPercentage() * 100.f), 24);
			Footer("Please wait...");
			Present();
		}
		const std::string error = g_GameManager.GetInstallError();
		if (!error.empty()) {
			Message("Installation failed", error);
		}
		SwitchLibrary::RequestScan();
		return;
	}
	Entry entry;
	entry.path = path;
	entry.key = ArtKey(path);
	entry.title = path.GetFilename();
	if (!EnsureGameInfo(entry, false)) {
		return;
	}
	if (entry.info->fileType == IdentifiedFileType::PSP_SAVEDATA_DIRECTORY) {
		Savedata(&entry);
		return;
	}
	if (entry.id == "MSTKUPDATE") {
		if (!Ask("Install firmware?", entry.title)) {
			return;
		}
		bool ok = false;
		std::string error;
		PSARUnpackStats stats;
		PSARUnpackOptions options;
		options.model = EmulatedModelGeneration();
		const Path updater = entry.info->fileType == IdentifiedFileType::PSP_PBP_DIRECTORY ? path / "EBOOT.PBP" : path;
		Busy(
			"Installing PSP firmware",
			[&](const auto &) { ok = InstallFirmware(updater, GetSysDirectory(DIRECTORY_NAND), options, &stats, &error); }, false);
		Message(ok ? "Firmware installed" : "Firmware installation failed", ok ? stats.firmwareVersion : error);
		return;
	}
	if (entry.info->fileType == IdentifiedFileType::PSP_UMD_VIDEO_ISO || entry.info->fileType == IdentifiedFileType::PSP_ISO) {
		const auto loader = entry.info->GetFileLoader();
		if (loader && (DetectUmdDiscType(loader.get()) & PSP_UMD_TYPE_VIDEO)) {
			const Path vsh = GetUmdVideoVSHPath();
			if (vsh.empty()) {
				Message("UMD video", "Install firmware with UMD video support to play this disc.");
				return;
			}
			const int action = UmdHasBootableGame(loader.get())
								   ? Rows("Game and video disc", [] { return std::vector<Row>{{"Play game", ">"}, {"Watch video", ">"}}; })
								   : 1;
			if (action < 0) {
				return;
			}
			if (action == 1) {
				result_ = {Action::Launch, vsh.ToString(), path.ToString(), false};
				return;
			}
		}
	}
	result_ = {Action::Launch, path.ToString()};
}

Launcher::~Launcher() {
	{
		std::lock_guard<std::mutex> guard(decodeMutex_);
		stopWorker_ = true;
		cancelArtwork_ = true;
		decodeQueue_.clear();
		metadataQueue_.clear();
	}
	decodeWake_.notify_all();
	if (worker_.joinable()) {
		worker_.join();
	}
	SDL_DestroyTexture(uploadTexture_);
	SDL_DestroyTexture(roundTexture_);
	for (auto &[key, texture] : artwork_) {
		SDL_DestroyTexture(texture.texture);
	}
	for (auto &[key, texture] : text_) {
		SDL_DestroyTexture(texture.texture);
	}
	for (auto &[id, pad] : pads_) {
		SDL_CloseGamepad(pad);
	}
	for (const auto &[name, texture] : atlasArt_) {
		SDL_DestroyTexture(texture);
	}
	textEngine_.reset();
	SDL_DestroyAudioStream(audio_);
	SDL_DestroyRenderer(renderer_);
	SDL_DestroyWindow(window_);
	if (plReady_) {
		plExit();
	}
}

bool Launcher::Init() {
	operationMode_ = appletGetOperationMode();
	File::CreateFullPath(Path(ART_DIRECTORY));
	LoadLauncherConfig(ini_);
	appearance_ = ini_.GetOrCreateSection("Launcher");
	const Path homebrew = GetSysDirectory(DIRECTORY_GAME);
	std::string previousHomebrew;
	appearance_->Get("HomebrewDirectory", &previousHomebrew);
	if (previousHomebrew != homebrew.ToString()) {
		File::CreateFullPath(homebrew);
		SwitchLibrary::AddSource(homebrew);
		appearance_->Set("HomebrewDirectory", homebrew.ToString());
	}
	appearance_->Get("Classic", &classic_);
	appearance_->Get("Animations", &animations_);
	appearance_->Get("ShowTitles", &titles_);
	appearance_->Get("ShowRegionFlags", &showRegions_);
	appearance_->Get("CheckUpdatesAtBoot", &updateAtBoot_);
	appearance_->Get("ShowBadges", &badges_);
	appearance_->Get("Columns", &columns_);
	appearance_->Get("Rows", &rows_);
	appearance_->Get("View", &view_);
	columns_ = std::clamp(columns_, 3, 8);
	rows_ = std::clamp(rows_, 1, 3);
	view_ = std::clamp(view_, 0, 2);
	for (auto &[key, value] : ini_.GetOrCreateSection("Favorites")->ToMap()) {
		if (value == "True") {
			favorites_.insert(key);
		}
	}
	for (int i = 0; i < 30; ++i) {
		std::string key;
		ini_.GetOrCreateSection("Recent")->Get(StringFromInt(i), &key);
		if (!key.empty()) {
			recentKeys_.push_back(std::move(key));
		}
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	window_ = SDL_CreateWindow("PPSSPP NX", WIDTH, HEIGHT, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
	if (!window_) {
		return false;
	}
	renderer_ = SDL_CreateRenderer(window_, "opengles2");
	if (!renderer_) {
		return false;
	}
	SDL_SetRenderLogicalPresentation(renderer_, WIDTH, HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	SDL_SetRenderVSync(renderer_, 1);
	SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
	InitRoundTexture();
	plReady_ = R_SUCCEEDED(plInitialize(PlServiceType_User));
	if (!LoadFonts()) {
		return false;
	}
	LoadAtlasArt();
	int padCount = 0;
	SDL_JoystickID *ids = SDL_GetGamepads(&padCount);
	for (int i = 0; i < padCount; ++i) {
		pads_[ids[i]] = SDL_OpenGamepad(ids[i]);
	}
	SDL_free(ids);
	SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
	audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (audio_) {
		SDL_ResumeAudioStreamDevice(audio_);
	}
	worker_ = std::thread(&Launcher::DecodeWorker, this);
	startedAt_ = SDL_GetTicks();
	LoadLibrary();
	Achievements::Initialize();
	packerGeneration_ = GamePacker::Generation();
	if (updateAtBoot_) {
		updateTask_ = std::make_unique<SwitchUpdateTask>();
		updateTask_->StartCheck();
	}
	return true;
}

bool Launcher::LoadFonts() {
	for (auto &[key, value] : text_) {
		SDL_DestroyTexture(value.texture);
	}
	text_.clear();
	textWidths_.clear();
	textBytes_ = 0;
	textEngine_.reset(new TextEngine());
	fontData_.clear();
	int outputWidth = WIDTH, outputHeight = HEIGHT;
	SDL_GetRenderOutputSize(renderer_, &outputWidth, &outputHeight);
	fontScale_ = std::max(1.f, outputHeight / (float)HEIGHT);
	PlSharedFontType preferred = PlSharedFontType_Standard;
	if (g_Config.sLanguageIni == "zh_CN") {
		preferred = PlSharedFontType_ChineseSimplified;
	} else if (g_Config.sLanguageIni == "zh_TW") {
		preferred = PlSharedFontType_ChineseTraditional;
	} else if (g_Config.sLanguageIni == "ko_KR") {
		preferred = PlSharedFontType_KO;
	}
	bool loaded = false;
	for (int index = -1; index < PlSharedFontType_Total; ++index) {
		const auto type = index < 0 ? preferred : (PlSharedFontType)index;
		if (index >= 0 && type == preferred) {
			continue;
		}
		PlFontData data{};
		if (plReady_ && R_SUCCEEDED(plGetSharedFontByType(&data, type))) {
			loaded |= textEngine_->AddFont(data.address, data.size);
		}
	}
	for (const char *name : {"NotoSans", "NotoSansArabic", "NotoSansHebrew", "NotoSansKhmer", "NotoSansLao", "NotoSansThai",
							 "NotoSansSymbols", "NotoSansSymbols2"}) {
		size_t size = 0;
		std::unique_ptr<uint8_t[]> data(g_VFS.ReadFile((std::string("launcher/fonts/") + name + "-Regular.ttf").c_str(), &size));
		if (data && size) {
			fontData_.emplace_back(data.get(), data.get() + size);
			loaded |= textEngine_->AddFont(fontData_.back().data(), fontData_.back().size());
		}
	}
	if (!loaded) {
		SDL_SetError("No usable launcher fonts");
	}
	return loaded;
}

void Launcher::SaveGameSettings(const char *reason) {
	// A per-game config must reach disk before UnloadGameConfig restores global
	// settings. Global settings are flushed by Save() when leaving the launcher,
	// and before loading a per-game config.
	if (g_Config.IsGameSpecific()) {
		g_Config.Save(reason);
	}
}

void Launcher::SelectGraphicsBackend(int backend) {
	g_Config.iGPUBackend = backend;
	g_Config.Save("SwitchLauncher::GraphicsBackendSelected");
}

void Launcher::SaveAppearance() {
	for (const char *key : {"TextColor", "SecondaryTextColor", "SelectedTextColor", "AccentColor", "Sort", "Sounds"}) {
		appearance_->Delete(key);
	}
	appearance_->Set("Classic", classic_);
	appearance_->Set("Animations", animations_);
	appearance_->Set("ShowTitles", titles_);
	appearance_->Set("ShowRegionFlags", showRegions_);
	appearance_->Set("CheckUpdatesAtBoot", updateAtBoot_);
	appearance_->Set("ShowBadges", badges_);
	appearance_->Set("Columns", columns_);
	appearance_->Set("Rows", rows_);
	appearance_->Set("View", view_);
	Section *favorites = ini_.GetOrCreateSection("Favorites");
	favorites->Clear();
	for (const std::string &key : favorites_) {
		favorites->Set(key, true);
	}
	Section *recent = ini_.GetOrCreateSection("Recent");
	recent->Clear();
	for (size_t i = 0; i < recentKeys_.size(); ++i) {
		recent->Set(StringFromInt((int)i), recentKeys_[i]);
	}
	if (!WriteLauncherConfig(ini_)) {
		ERROR_LOG(Log::System, "Could not save launcher settings");
	}
}

void Launcher::Save() {
	SaveAppearance();
	g_Config.Save("SwitchLauncher");
}

void Launcher::Fill(float x, float y, float w, float h, SDL_Color color) {
	SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
	SDL_FRect rect{x, y, w, h};
	SDL_RenderFillRect(renderer_, &rect);
}

void Launcher::InitRoundTexture() {
	std::array<uint8_t, 32 * 32 * 4> pixels;
	pixels.fill(255);
	for (int y = 0; y < 32; ++y) {
		for (int x = 0; x < 32; ++x) {
			const float dx = x - 15.5f, dy = y - 15.5f;
			pixels[(y * 32 + x) * 4 + 3] = (uint8_t)(255 * std::clamp(16.f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f));
		}
	}
	roundTexture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 32, 32);
	if (roundTexture_ && !SDL_UpdateTexture(roundTexture_, nullptr, pixels.data(), 32 * 4)) {
		SDL_DestroyTexture(roundTexture_);
		roundTexture_ = nullptr;
	}
	if (roundTexture_) {
		SDL_SetTextureBlendMode(roundTexture_, SDL_BLENDMODE_BLEND);
		SDL_SetTextureScaleMode(roundTexture_, SDL_SCALEMODE_LINEAR);
	}
}

void Launcher::Round(float x, float y, float w, float h, float radius, SDL_Color color) {
	if (w <= 0 || h <= 0 || !color.a) {
		return;
	}
	radius = std::clamp(radius, 0.f, std::min(w, h) * .5f);
	if (!roundTexture_ || radius < 1) {
		Fill(x, y, w, h, color);
		return;
	}
	const float xs[] = {x, x + radius, x + w - radius, x + w};
	const float ys[] = {y, y + radius, y + h - radius, y + h};
	constexpr float uv[] = {0, .5f, .5f, 1};
	const SDL_FColor tint{color.r / 255.f, color.g / 255.f, color.b / 255.f, color.a / 255.f};
	SDL_Vertex vertices[16];
	for (int row = 0; row < 4; ++row) {
		for (int column = 0; column < 4; ++column) {
			vertices[row * 4 + column] = {{xs[column], ys[row]}, tint, {uv[column], uv[row]}};
		}
	}
	static constexpr std::array<int, 54> indices = [] {
		std::array<int, 54> result{};
		int index = 0;
		for (int row = 0; row < 3; ++row) {
			for (int column = 0; column < 3; ++column) {
				const int a = row * 4 + column;
				for (int vertex : {a, a + 1, a + 5, a, a + 5, a + 4}) {
					result[index++] = vertex;
				}
			}
		}
		return result;
	}();
	SDL_RenderGeometry(renderer_, roundTexture_, vertices, 16, indices.data(), (int)indices.size());
}

void Launcher::Panel(float x, float y, float w, float h, bool selected) {
	Round(x, y + 3, w, h, 14, {0, 0, 0, 45});
	Round(x, y, w, h, 14, selected ? SDL_Color{116, 200, 255, 230} : SDL_Color{83, 105, 132, 100});
	Round(x + 1, y + 1, w - 2, h - 2, 13, selected ? SDL_Color{28, 69, 92, 245} : SDL_Color{16, 23, 39, 235});
}

void Launcher::PageHeader(UiText rawtitle, UiText raweyebrow, UiText rawcontext, UiText rawdetail) {
	const std::string_view title = rawtitle;
	const std::string_view eyebrow = raweyebrow;
	const std::string_view context = rawcontext;
	const std::string_view detail = rawdetail;
	Gradient({0, 0, WIDTH, 128}, {0, 0, 0, 125}, {0, 0, 0, 0});
	const float metadataWidth = std::min(WIDTH / 3.f, std::max(TextWidth(context, 22), TextWidth(detail, 22)));
	const float titleWidth = WIDTH - 80 - (metadataWidth ? metadataWidth + 32 : 0);
	SDL_Rect clip{40, 0, (int)titleWidth, 80};
	SDL_SetRenderClipRect(renderer_, &clip);
	Text(40, 9, eyebrow, 18, palette_.dim);
	const float overflow = std::max(0.f, TextWidth(title, 32) - titleWidth);
	const float scroll = animations_ && overflow ? (std::sin(SDL_GetTicks() / 2200.f - 1.5707963f) + 1) * overflow / 2 : 0;
	Text(40 - scroll, 31, title, 32);
	SDL_SetRenderClipRect(renderer_, nullptr);
	if (metadataWidth) {
		clip = {WIDTH - 40 - (int)metadataWidth, 0, (int)metadataWidth, 80};
		SDL_SetRenderClipRect(renderer_, &clip);
		Text(WIDTH - 40 - TextWidth(context, 22), 16, context, 22);
		Text(WIDTH - 40 - TextWidth(detail, 22), 44, detail, 22, palette_.dim);
		SDL_SetRenderClipRect(renderer_, nullptr);
	}
}

void Launcher::SectionHeading(UiText rawtitle, float x, float y, float width) {
	const std::string_view title = rawtitle;
	Fill(x, y + 4, 3, 16, palette_.accent);
	SDL_Rect clip{(int)x + 14, (int)y, (int)width - 24, 30};
	SDL_SetRenderClipRect(renderer_, &clip);
	Text(x + 14, y, title, 22, palette_.value);
	SDL_SetRenderClipRect(renderer_, nullptr);
	const float line = std::min(width, TextWidth(title, 22) + 30);
	Fill(x + line, y + 12, width - line, 1, {255, 255, 255, 28});
}

void Launcher::ImageFit(SDL_Texture *image, SDL_FRect rect, bool crop) {
	float width = 0, height = 0;
	if (!SDL_GetTextureSize(image, &width, &height) || width <= 0 || height <= 0) {
		return;
	}
	const float scale = crop ? std::max(rect.w / width, rect.h / height) : std::min(rect.w / width, rect.h / height);
	SDL_FRect destination{rect.x + (rect.w - width * scale) * .5f, rect.y + (rect.h - height * scale) * .5f, width * scale, height * scale};
	SDL_Rect clip{(int)rect.x, (int)rect.y, (int)rect.w, (int)rect.h};
	SDL_SetRenderClipRect(renderer_, &clip);
	SDL_RenderTexture(renderer_, image, nullptr, &destination);
	SDL_SetRenderClipRect(renderer_, nullptr);
}

void Launcher::Flag(GameRegion region, float x, float y) {
	Fill(x, y, 36, 24, {245, 245, 245, 255});
	if (region == GameRegion::JAPAN) {
		for (int row = -7; row <= 7; ++row) {
			const float half = std::sqrt(49.f - row * row);
			Fill(x + 18 - half, y + 12 + row, half * 2, 1, {191, 32, 55, 255});
		}
	} else if (region == GameRegion::USA) {
		for (int stripe = 0; stripe < 13; stripe += 2) {
			Fill(x, y + stripe * 24.f / 13, 36, 24.f / 13, {178, 34, 52, 255});
		}
		Fill(x, y, 16, 13, {45, 55, 110, 255});
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 4; ++col) {
				Fill(x + 2 + col * 4, y + 2 + row * 4, 1, 1, {255, 255, 255, 255});
			}
		}
	} else if (region == GameRegion::EUROPE) {
		Fill(x, y, 36, 24, {20, 55, 149, 255});
		for (int i = 0; i < 12; ++i) {
			Fill(x + 17 + std::cos(i * 6.2831853f / 12) * 8, y + 11 + std::sin(i * 6.2831853f / 12) * 8, 2, 2, {255, 213, 48, 255});
		}
	} else {
		Fill(x, y, 36, 24, {45, 55, 75, 255});
		const char *code = region == GameRegion::KOREA		? "KR"
						   : region == GameRegion::HONGKONG ? "HK"
						   : region == GameRegion::ASIA		? "AS"
															: "--";
		Text(x + 5, y + 2, code, 17);
	}
}

std::vector<std::string> Launcher::Wrap(UiText rawtext, float width, int size) {
	const std::string_view text = rawtext;
	class Wrapper : public WordWrapper {
	public:
		Wrapper(std::string_view text, float width, std::function<float(std::string_view)> measure)
			: WordWrapper(text, width, FLAG_WRAP_TEXT), measure_(std::move(measure)) {}
		float MeasureWidth(std::string_view text) override { return measure_(text); }

	private:
		std::function<float(std::string_view)> measure_;
	};
	Wrapper wrapper(text, width, [&](std::string_view line) { return TextWidth(line, size); });
	std::vector<std::string> lines;
	SplitString(wrapper.Wrapped(), '\n', lines);
	return lines;
}

void Launcher::Wrapped(float x, float y, float width, std::string_view text, int size, int maxLines, SDL_Color color) {
	const auto lines = Wrap(text, width, size);
	for (int i = 0; i < std::min(maxLines, (int)lines.size()); ++i) {
		Text(x, y + i * (size + 7), lines[i], size, color);
	}
}

void Launcher::Cards(UiText title, const std::function<std::vector<Card>()> &build) {
	int selection = rowSelections_[std::string((std::string_view)title)], top = 0;
	constexpr int visible = 5, rowHeight = 94;
	std::vector<Card> cards;
	uint64_t refreshAt = 0;
	float touchStart = 0;
	int touchSelection = 0;
	bool moved = false;
	stickNavigation_.Reset();
	repeatDirection_ = 0;
	while (Frame()) {
		if (SDL_GetTicks() >= refreshAt) {
			cards = build();
			refreshAt = SDL_GetTicks() + 500;
		}
		if (cards.empty()) {
			Message(title, "No entries available.");
			return;
		}
		selection = std::clamp(selection, 0, (int)cards.size() - 1);
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event)) {
				rowSelections_[std::string((std::string_view)title)] = selection;
				stickNavigation_.Reset();
				repeatDirection_ = 0;
				return;
			}
			const int direction = Navigate(event);
			if (std::abs(direction) == 2) {
				selection = std::clamp(selection + (direction > 0 ? 1 : -1), 0, (int)cards.size() - 1);
			}
			if (event.type == SDL_EVENT_MOUSE_WHEEL) {
				selection = std::clamp(selection - (int)event.wheel.y, 0, (int)cards.size() - 1);
			}
			if (event.type == SDL_EVENT_FINGER_DOWN) {
				touchStart = event.tfinger.y;
				touchSelection = selection;
				moved = false;
			}
			if (event.type == SDL_EVENT_FINGER_MOTION && std::abs(event.tfinger.y - touchStart) > 16) {
				moved = true;
				selection =
					std::clamp(touchSelection - (int)std::round((event.tfinger.y - touchStart) / rowHeight), 0, (int)cards.size() - 1);
			}
			bool activate = Confirm(event);
			if (event.type == SDL_EVENT_MOUSE_BUTTON_UP || (event.type == SDL_EVENT_FINGER_UP && !moved)) {
				const float x = event.type == SDL_EVENT_FINGER_UP ? event.tfinger.x : event.button.x;
				const float y = event.type == SDL_EVENT_FINGER_UP ? event.tfinger.y : event.button.y;
				if (y >= HEIGHT - 52) {
					return;
				}
				if (x >= 40 && x <= 744 && y >= 132 && y < 132 + visible * rowHeight) {
					const int hit = top + (int)((y - 132) / rowHeight);
					if (hit < (int)cards.size()) {
						activate = hit == selection;
						selection = hit;
					}
				}
			}
			if (activate && cards[selection].open) {
				auto open = cards[selection].open;
				open();
				refreshAt = 0;
				stickNavigation_.Reset();
				repeatDirection_ = 0;
				break;
			}
		}
		top = std::clamp(top, std::max(0, selection - visible + 1), selection);
		Background();
		PageHeader(title, "Library & storage", StringFromFormat("%d / %zu", selection + 1, cards.size()));
		auto image = [&](Card &card) {
			return card.image.empty() ? card.icon.info ? Artwork(card.icon) : nullptr : RemoteImage(card.image);
		};
		for (int i = top; i < std::min((int)cards.size(), top + visible); ++i) {
			Card &card = cards[i];
			const float y = 132 + (i - top) * rowHeight;
			Panel(40, y, 704, rowHeight - 8, i == selection);
			if (auto *icon = image(card)) {
				ImageFit(icon, {54, y + 9, 116, 66}, false);
			} else {
				Icon(1, 111, y + 42, 28, palette_.dim);
			}
			SDL_Rect clip{186, (int)y + 5, 534, rowHeight - 18};
			SDL_SetRenderClipRect(renderer_, &clip);
			Text(186, y + 9, card.title, 24, i == selection ? palette_.value : palette_.text);
			Text(186, y + 48, card.subtitle, 18, card.status.a ? card.status : palette_.dim);
			SDL_SetRenderClipRect(renderer_, nullptr);
		}
		Card &current = cards[selection];
		Panel(774, 132, WIDTH - 814, 478);
		if (auto *icon = image(current)) {
			ImageFit(icon, {794, 150, WIDTH - 854, 188}, false);
		} else {
			Icon(1, 1007, 244, 48, palette_.dim);
		}
		Wrapped(794, 350, WIDTH - 854, current.title, 24, 2, palette_.value);
		Wrapped(794, 416, WIDTH - 854, current.metadata, 18, 2, palette_.dim);
		Wrapped(794, 474, WIDTH - 854, current.description, 19, 4, palette_.text);
		if ((int)cards.size() > visible) {
			const int height = visible * rowHeight - 8, thumb = std::max(16, height * visible / (int)cards.size());
			Fill(756, 132, 4, height, {40, 44, 54, 255});
			Fill(756, 132 + (height - thumb) * top / std::max(1, (int)cards.size() - visible), 4, thumb, palette_.accent);
		}
		Footer("B  Back");
		Present();
	}
}

void Launcher::Message(UiText rawtitle, UiText rawtext) {
	const std::string title((std::string_view)rawtitle);
	const std::string text((std::string_view)rawtext);
	class Wrapper : public WordWrapper {
	public:
		Wrapper(std::string_view text, float width, std::function<float(std::string_view)> measure)
			: WordWrapper(text, width, FLAG_WRAP_TEXT), measure_(std::move(measure)) {}
		float MeasureWidth(std::string_view text) override { return measure_(text); }

	private:
		std::function<float(std::string_view)> measure_;
	};
	Wrapper wrapper(text, WIDTH - 380, [&](std::string_view line) { return TextWidth(line, 22); });
	std::vector<std::string> lines;
	SplitString(wrapper.Wrapped(), '\n', lines);
	int top = 0;
	while (Frame()) {
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event) || Confirm(event)) {
				return;
			}
			const int direction = Navigate(event);
			if (direction) {
				top = std::clamp(top + (direction > 0 ? 1 : -1), 0, std::max(0, (int)lines.size() - 10));
			}
		}
		Background();
		Text(40, 9, "PPSSPP", 18);
		Text(40, 31, title, 32);
		Panel(138, 136, WIDTH - 276, 458);
		for (int i = top; i < std::min(top + 10, (int)lines.size()); ++i) {
			Text(178, 160 + (i - top) * 38.f, lines[i], 22);
		}
		Footer("A / B  Back");
		Present();
	}
}

void Launcher::Gradient(SDL_FRect rect, SDL_Color top, SDL_Color bottom) {
	SDL_FColor a{top.r / 255.f, top.g / 255.f, top.b / 255.f, top.a / 255.f};
	SDL_FColor b{bottom.r / 255.f, bottom.g / 255.f, bottom.b / 255.f, bottom.a / 255.f};
	SDL_Vertex vertices[] = {{{rect.x, rect.y}, a, {}},
							 {{rect.x + rect.w, rect.y}, a, {}},
							 {{rect.x + rect.w, rect.y + rect.h}, b, {}},
							 {{rect.x, rect.y + rect.h}, b, {}}};
	const int indices[] = {0, 1, 2, 0, 2, 3};
	SDL_RenderGeometry(renderer_, nullptr, vertices, 4, indices, 6);
}

float Launcher::TextWidth(UiText rawvalue, int size) {
	const std::string_view value = rawvalue;
	const std::string key = StringFromInt(size) + ":" + std::string(value);
	auto found = textWidths_.find(key);
	if (found != textWidths_.end()) {
		return found->second;
	}
	const float width = textEngine_->Measure(value, (int)std::lround(size * fontScale_)) / fontScale_;
	if (textWidths_.size() > 2048) {
		textWidths_.clear();
	}
	textWidths_.emplace(key, width);
	return width;
}

void Launcher::Text(float x, float y, UiText rawvalue, int size, SDL_Color color) {
	const std::string_view value = rawvalue;
	if (value.empty()) {
		return;
	}
	if (!color.a) {
		color = palette_.text;
	}
	const std::string key = StringFromInt(size) + ":" + std::string(value);
	auto it = text_.find(key);
	if (it == text_.end()) {
		const auto bitmap = textEngine_->Render(value, (int)std::lround(size * fontScale_));
		while (!text_.empty() && (textBytes_ + bitmap.rgba.size() > MAX_TEXT_BYTES || text_.size() >= 512)) {
			auto victim =
				std::min_element(text_.begin(), text_.end(), [](const auto &a, const auto &b) { return a.second.use < b.second.use; });
			textBytes_ -= victim->second.bytes;
			SDL_DestroyTexture(victim->second.texture);
			text_.erase(victim);
		}
		SDL_Texture *texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, bitmap.width, bitmap.height);
		if (!texture) {
			return;
		}
		SDL_UpdateTexture(texture, nullptr, bitmap.rgba.data(), bitmap.width * 4);
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
		it = text_.emplace(key, Texture{texture, ++use_, bitmap.rgba.size()}).first;
		textBytes_ += bitmap.rgba.size();
	}
	it->second.use = ++use_;
	SDL_SetTextureColorMod(it->second.texture, color.r, color.g, color.b);
	SDL_SetTextureAlphaMod(it->second.texture, color.a);
	float w = 0, h = 0;
	SDL_GetTextureSize(it->second.texture, &w, &h);
	SDL_FRect destination{x, y, w / fontScale_, h / fontScale_};
	SDL_RenderTexture(renderer_, it->second.texture, nullptr, &destination);
}

void Launcher::Center(float x, float y, UiText rawvalue, int size, SDL_Color color) {
	const std::string_view value = rawvalue;
	Text(x - TextWidth(value, size) / 2, y, value, size, color);
}

void Launcher::Icon(int kind, float x, float y, float size, SDL_Color color) {
	SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
	if (kind == 3) {
		const float offsets[] = {size / 5, -size / 4, size / 10};
		for (int row = 0; row < 3; ++row) {
			const float cy = y + (row - 1) * size / 3;
			Round(x - size / 2, cy - 1, size, 2, 1, color);
			Round(x + offsets[row] - 2, cy - size / 9, 4, size * 2 / 9, 2, color);
		}
		return;
	}
	if (kind == 1) {
		for (int row = 0; row < (kind == 3 ? 3 : 2); ++row) {
			for (int column = 0; column < 3; ++column) {
				Round(x + (column - 1) * size / 3 - 3, y + (row - 0.5f) * size / 3 - 3, 7, 7, 1, color);
			}
		}
		return;
	}
	const int points = kind == 2 ? 10 : 48;
	for (int i = 0; i < points; ++i) {
		auto point = [&](int j) {
			float radius = size / 2 * (kind == 2 && j % 2 ? 0.45f : 1.f);
			float angle = j * 6.2831853f / points - 1.5707963f;
			return SDL_FPoint{x + std::cos(angle) * radius, y + std::sin(angle) * radius};
		};
		SDL_FPoint a = point(i), b = point(i + 1);
		if (kind == 2) {
			const SDL_FColor fill{color.r / 255.f, color.g / 255.f, color.b / 255.f, color.a / 255.f};
			const SDL_Vertex vertices[] = {{{x, y}, fill, {0, 0}}, {a, fill, {0, 0}}, {b, fill, {0, 0}}};
			SDL_RenderGeometry(renderer_, nullptr, vertices, 3, nullptr, 0);
		} else {
			SDL_RenderLine(renderer_, a.x, a.y, b.x, b.y);
		}
	}
	if (kind == 0) {
		SDL_RenderLine(renderer_, x, y - size / 3, x, y);
		SDL_RenderLine(renderer_, x, y, x + size / 4, y);
	}
}

void Launcher::Background() {
	Fill(0, 0, WIDTH, HEIGHT, classic_ ? SDL_Color{8, 12, 24, 255} : SDL_Color{36, 77, 117, 255});
	if (classic_ && !visible_.empty()) {
		SDL_Texture *background = Artwork(games_[visible_[selection_]], true);
		if (background) {
			SDL_SetTextureAlphaMod(background, 120);
			SDL_FRect destination{0, 0, WIDTH, HEIGHT};
			ImageFit(background, destination, true);
			Gradient(destination, {8, 12, 24, 45}, {8, 12, 24, 245});
		}
	} else if (!classic_) {
		const float t = animations_ ? SDL_GetTicks() / 1000.f : 0;
		for (int i = 0; i < WIDTH * HEIGHT / (120 * 120); ++i) {
			const float x = (i * 467 % WIDTH);
			const float y = (i * 239 % HEIGHT) + 40.f * std::cos(i * 7.2f + t * 1.3f);
			const float s = 22.f, angle = std::sin(i + t), c = std::cos(angle), sn = std::sin(angle);
			auto point = [&](float px, float py) { return SDL_FPoint{x + px * c - py * sn, y + px * sn + py * c}; };
			SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 19);
			if (i % 4 == 0) {
				SDL_FPoint a = point(-s, -s), b = point(s, s), d = point(-s, s), e = point(s, -s);
				SDL_RenderLine(renderer_, a.x, a.y, b.x, b.y);
				SDL_RenderLine(renderer_, d.x, d.y, e.x, e.y);
			} else if (i % 4 == 1) {
				Icon(4, x, y, s * 2, {255, 255, 255, 19});
			} else if (i % 4 == 2) {
				SDL_FPoint points[] = {point(-s, -s), point(s, -s), point(s, s), point(-s, s), point(-s, -s)};
				SDL_RenderLines(renderer_, points, 5);
			} else {
				SDL_FPoint points[] = {point(0, -s), point(s, s), point(-s, s), point(0, -s)};
				SDL_RenderLines(renderer_, points, 4);
			}
		}
	}
	Gradient({0, 0, WIDTH, 140}, {0, 0, 0, 110}, {0, 0, 0, 0});
}

void Launcher::Footer(UiText rawvalue) {
	std::string_view value = rawvalue;
	std::string libraryHints;
	if (value.empty()) {
		for (const auto &hint : {std::pair{"A", "Launch"}, {"Y", "Game menu"}, {"L / R", "View"}, {"+", "Settings"}, {"B", "Quit"}}) {
			if (!libraryHints.empty()) {
				libraryHints += "        ";
			}
			libraryHints += std::string(hint.first) + "  " + std::string(Tr(hint.second));
		}
		value = libraryHints;
	}
	struct Hint {
		std::string_view button, action;
		float width;
	};
	std::vector<std::vector<Hint>> lines(1);
	float rowWidth = 0;
	while (!value.empty()) {
		const size_t next = value.find("        ");
		const auto hint = value.substr(0, next);
		const size_t split = hint.find("  ");
		const auto button = hint.substr(0, split);
		const auto action = split == std::string_view::npos ? std::string_view{} : hint.substr(split + 2);
		const float width = std::max(26.f, TextWidth(button, 16) + 16) + 10 + TextWidth(action, 20);
		if (rowWidth && rowWidth + width + 26 > WIDTH - 214) {
			lines.emplace_back();
			rowWidth = 0;
		}
		lines.back().push_back({button, action, width});
		rowWidth += width + 26;
		if (next == std::string_view::npos) {
			break;
		}
		value.remove_prefix(next + 8);
	}
	const float top = HEIGHT - 39 - (lines.size() - 1) * 36;
	Gradient({0, top - 34, WIDTH, HEIGHT - top + 34}, {0, 0, 0, 0}, {0, 0, 0, 160});
	for (size_t line = 0; line < lines.size(); ++line) {
		float width = 0;
		for (const auto &hint : lines[line]) {
			width += hint.width + 26;
		}
		float x = std::max(174.f, WIDTH - 40.f - width + 26);
		const float y = top + line * 36;
		for (const auto &hint : lines[line]) {
			const float buttonWidth = std::max(26.f, TextWidth(hint.button, 16) + 16);
			Round(x, y, buttonWidth, 26, 13, {5, 35, 56, 235});
			Text(x + (buttonWidth - TextWidth(hint.button, 16)) / 2, y + 2, hint.button, 16, palette_.text);
			x += buttonWidth + 10;
			Text(x, y, hint.action, 20, palette_.dim);
			x += TextWidth(hint.action, 20) + 26;
		}
	}
	const auto logo = atlasArt_.find("I_LOGO");
	if (logo != atlasArt_.end()) {
		SDL_SetTextureColorMod(logo->second, palette_.text.r, palette_.text.g, palette_.text.b);
		ImageFit(logo->second, {24, HEIGHT - 34, 126, 22}, false);
	}
}

void Launcher::Present() {
	if (animations_) {
		const float alpha = std::max(0.f, 1.f - (SDL_GetTicks() - startedAt_) / 180.f);
		if (alpha > 0) {
			Fill(0, 0, WIDTH, HEIGHT, {0, 0, 0, (uint8_t)(alpha * 255)});
		}
	}
	if (!SDL_RenderPresent(renderer_)) {
		if (!presentFailureLogged_) {
			const std::string error = StringFromFormat("SDL frame presentation failed: %s", SDL_GetError());
			ERROR_LOG(Log::System, "%s", error.c_str());
			presentFailureLogged_ = true;
		}
		return;
	}
	int width = WIDTH, height = HEIGHT;
	SDL_GetWindowSizeInPixels(window_, &width, &height);
	saltynx_set_resolution(width, height);
	saltynx_frame(2);
	SDL_Delay(1);
}

bool Launcher::Frame() {
	Achievements::Idle();
	g_DownloadManager.Update();
	g_GameManager.Update();
	GamePacker::UpdateOnUIThread();
	if (packerGeneration_ != GamePacker::Generation()) {
		packerGeneration_ = GamePacker::Generation();
		const auto result = GamePacker::LastResult();
		if (result.success && result.oldPath != result.newPath) {
			MoveLibraryState(result.oldPath, result.newPath);
		}
		SwitchLibrary::RequestScan();
	}
	running_ = running_ && appletMainLoop();
	if (appletGetOperationMode() != operationMode_) {
		operationMode_ = appletGetOperationMode();
		SDL_SetWindowFullscreen(window_, true);
		SDL_SetRenderLogicalPresentation(renderer_, WIDTH, HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		LoadFonts();
	}
	PumpArtwork();
	if (SDL_GetTicks() >= metadataAt_) {
		metadataAt_ = SDL_GetTicks() + 500;
		RefreshMetadata();
	}
	if (updateTask_) {
		updateAvailable_ = updateTask_->Snapshot().state == SwitchUpdateState::UpdateAvailable;
	}
	return running_ && result_.action == Action::Unavailable;
}

void Launcher::LoadLibrary() {
	auto paths = SwitchLibrary::GetGames();
	for (const std::string &recent : g_recentFiles.GetRecentFiles()) {
		if (!recent.empty()) {
			paths.emplace_back(recent);
		}
	}
	std::unordered_map<std::string, size_t> identities;
	libraryGeneration_ = SwitchLibrary::Generation();
	std::string previous = !visible_.empty() && selection_ < (int)visible_.size() ? games_[visible_[selection_]].key : "";
	games_.clear();
	visible_.clear();
	for (const Path &path : paths) {
		const std::string identity = StableGameIdentity(path);
		const auto [found, inserted] = identities.emplace(identity, games_.size());
		if (!inserted) {
			Entry &primary = games_[found->second];
			const std::string oldKey = ArtKey(path);
			if (oldKey != primary.key) {
				if (previous == oldKey) {
					previous = primary.key;
				}
				if (favorites_.count(oldKey)) {
					favorites_.insert(primary.key);
				}
				std::replace(recentKeys_.begin(), recentKeys_.end(), oldKey, primary.key);
				std::string alias;
				Section *aliases = ini_.GetOrCreateSection("Aliases");
				if (!aliases->Get(primary.key, &alias) && aliases->Get(oldKey, &alias)) {
					aliases->Set(primary.key, alias);
					primary.title = alias;
				}
				for (const char *suffix : {".cover.png", ".background.png"}) {
					const Path source(std::string(ART_DIRECTORY) + "/" + oldKey + suffix);
					const Path target(std::string(ART_DIRECTORY) + "/" + primary.key + suffix);
					if (File::Exists(source) && !File::Exists(target)) {
						File::Copy(source, target);
					}
				}
			}
			continue;
		}
		Entry game;
		game.path = path;
		game.identity = identity;
		game.key = ArtKey(path);
		game.title = path.GetFilename();
		game.info = g_gameInfoCache->TryGetInfo(path, GameInfoFlags::PARAM_SFO, nullptr, TaskPriority::LOW);
		ini_.GetOrCreateSection("Aliases")->Get(game.key, &game.title);
		games_.push_back(std::move(game));
	}
	std::set<std::string> recent;
	recentKeys_.erase(std::remove_if(recentKeys_.begin(), recentKeys_.end(), [&](const auto &key) { return !recent.insert(key).second; }),
					  recentKeys_.end());
	FilterLibrary();
	for (size_t i = 0; i < visible_.size(); ++i) {
		if (games_[visible_[i]].key == previous) {
			selection_ = (int)i;
			break;
		}
	}
}

bool Launcher::EnsureGameInfo(Entry &entry, bool requireID) {
	entry.info = g_gameInfoCache->GetInfo(nullptr, entry.path, GameInfoFlags::PARAM_SFO | GameInfoFlags::ICON);
	if (!entry.info->Ready(GameInfoFlags::PARAM_SFO)) {
		Busy("Reading game information", [&](const auto &) { entry.info->WaitUntilReady(GameInfoFlags::PARAM_SFO); }, false);
	}
	entry.title = entry.info->GetTitle();
	ini_.GetOrCreateSection("Aliases")->Get(entry.key, &entry.title);
	{
		std::lock_guard<std::mutex> lock(entry.info->lock);
		entry.id = entry.info->id;
		entry.region = entry.info->region;
		entry.customConfig = entry.info->hasConfig;
	}
	if (requireID && entry.id.empty()) {
		Message("Game information unavailable", "This file has no usable game ID for per-game settings.");
		return false;
	}
	return true;
}

void Launcher::UpdateConfigBadge(Entry *entry) {
	if (!entry || !entry->info) {
		return;
	}
	entry->customConfig = g_Config.HasGameConfig(entry->id);
	std::lock_guard<std::mutex> guard(entry->info->lock);
	entry->info->hasConfig = entry->customConfig;
}

void Launcher::RefreshMetadata() {
	bool changed = false;
	for (Entry &entry : games_) {
		auto info = g_gameInfoCache->TryGetInfo(entry.path, GameInfoFlags::PARAM_SFO, nullptr, TaskPriority::LOW);
		GameInfoMetadata metadata;
		if (!info || !info->TryGetMetadata(&metadata)) {
			continue;
		}
		entry.info = std::move(info);
		ini_.GetOrCreateSection("Aliases")->Get(entry.key, &metadata.title);
		changed |= metadata.title != entry.title;
		entry.title = std::move(metadata.title);
		entry.id = std::move(metadata.id);
		entry.customConfig = metadata.hasConfig;
		entry.region = metadata.region;
	}
	if (changed) {
		FilterLibrary();
	}
}

void Launcher::FilterLibrary() {
	const std::string previous = !visible_.empty() && selection_ < (int)visible_.size() ? games_[visible_[selection_]].key : "";
	visible_.clear();
	const auto recentPaths = g_recentFiles.GetRecentFiles();
	std::vector<std::string> recent = recentKeys_;
	std::unordered_map<std::string, std::string> gameKeys;
	for (const Entry &entry : games_) {
		gameKeys.emplace(entry.identity, entry.key);
	}
	for (const std::string &path : recentPaths) {
		const std::string identity = StableGameIdentity(Path(path));
		const auto game = gameKeys.find(identity);
		const std::string key = game != gameKeys.end() ? game->second : ArtKey(Path(path));
		if (std::find(recent.begin(), recent.end(), key) == recent.end()) {
			recent.push_back(key);
		}
	}
	for (size_t i = 0; i < games_.size(); ++i) {
		Entry &game = games_[i];
		if (!search_.empty() && Lower(game.title).find(Lower(search_)) == std::string::npos) {
			continue;
		}
		if (view_ == 2 && !favorites_.count(game.key)) {
			continue;
		}
		if (view_ == 0 && std::find(recent.begin(), recent.end(), game.key) == recent.end()) {
			continue;
		}
		visible_.push_back((int)i);
	}
	std::stable_sort(visible_.begin(), visible_.end(), [&](int a, int b) {
		if (view_ == 0) {
			return std::find(recent.begin(), recent.end(), games_[a].key) < std::find(recent.begin(), recent.end(), games_[b].key);
		}
		return Lower(games_[a].title) < Lower(games_[b].title);
	});
	selection_ = std::clamp(selection_, 0, std::max(0, (int)visible_.size() - 1));
	for (size_t i = 0; i < visible_.size(); ++i) {
		if (games_[visible_[i]].key == previous) {
			selection_ = (int)i;
			break;
		}
	}
}

SDL_FRect Launcher::CoverRect(int index) const {
	if (view_ == 0) {
		const float slot = index - recentPosition_;
		const float focus = std::max(0.f, 1.f - std::abs(slot));
		const float width = 200 + 40 * focus;
		return {40 + slot * 218 + std::clamp(slot, 0.f, 1.f) * 38, 126, width, width * 1.5f};
	}
	const int coverHeight = std::min((HEIGHT - 112 - 88 - (rows_ - 1) * 20 - rows_ * (titles_ ? 28 : 0)) / rows_,
									 (WIDTH - 112 - (columns_ - 1) * 32) / columns_ * 3 / 2);
	const int coverWidth = coverHeight * 2 / 3;
	const int page = selection_ / (columns_ * rows_);
	const int position = index - page * columns_ * rows_;
	const int gridWidth = columns_ * coverWidth + (columns_ - 1) * 32;
	const int gridHeight = rows_ * (coverHeight + (titles_ ? 28 : 0)) + (rows_ - 1) * 20;
	return {(WIDTH - gridWidth) / 2.f + position % columns_ * (coverWidth + 32),
			112 + (HEIGHT - 200 - gridHeight) / 2.f + position / columns_ * (coverHeight + (titles_ ? 28 : 0) + 20), (float)coverWidth,
			(float)coverHeight};
}

void Launcher::QueueArtwork(Entry &entry, bool background) {
	const std::string key = entry.key + (background ? ".background" : ".cover");
	if (pendingArt_.count(key) || artwork_.count(key)) {
		return;
	}
	Decode request;
	request.key = key;
	request.revision = artRevisions_[key];
	request.path = Path(std::string(ART_DIRECTORY) + "/" + key + ".png");
	request.gamePath = entry.path.empty() && entry.info ? entry.info->GetFilePath() : entry.path;
	std::unique_lock<std::mutex> guard(decodeMutex_, std::try_to_lock);
	if (!guard.owns_lock() || pendingArt_.size() >= 48) {
		return;
	}
	pendingArt_.insert(key);
	decodeQueue_.push_back(std::move(request));
	decodeWake_.notify_one();
}

SDL_Texture *Launcher::Artwork(Entry &entry, bool background) {
	const std::string key = entry.key + (background ? ".background" : ".cover");
	QueueArtwork(entry, background);
	auto it = artwork_.find(key);
	if (it == artwork_.end()) {
		return nullptr;
	}
	it->second.use = ++use_;
	return it->second.texture;
}

void Launcher::DecodeWorker() {
	SetCurrentThreadName("LauncherArtwork");
	SetCurrentThreadAffinity(ThreadAffinityRole::BACKGROUND);
	for (;;) {
		Decode request;
		{
			std::unique_lock<std::mutex> guard(decodeMutex_);
			decodeWake_.wait(guard, [&] { return stopWorker_ || (!decodeQueue_.empty() && decoded_.size() < MAX_DECODED); });
			if (stopWorker_) {
				return;
			}
			request = std::move(decodeQueue_.front());
			decodeQueue_.pop_front();
		}
		if (!request.url.empty()) {
			std::vector<uint8_t> data;
			if (SteamGridDB_DownloadImage(request.url, &data, &cancelArtwork_) == SteamGridDBResult::OK) {
				request.data.assign(data.begin(), data.end());
			}
		}
		if (!request.path.empty() && File::GetFileSize(request.path) <= 24 * 1024 * 1024) {
			File::ReadBinaryFileToString(request.path, &request.data);
		}
		if (request.data.empty() && !request.gamePath.empty()) {
			if (!request.info) {
				request.path = Path();
				std::lock_guard<std::mutex> guard(decodeMutex_);
				metadataQueue_.push_back(std::move(request));
				continue;
			}
			std::lock_guard<std::mutex> guard(request.info->lock);
			request.data = request.key.find(".background") != std::string::npos ? request.info->pic1.data : request.info->icon.data;
		}
		Pixels pixels;
		pixels.key = request.key;
		pixels.revision = request.revision;
		uint8_t *decoded = DecodeArtwork(request.data, &pixels.width, &pixels.height);
		if (decoded && pixels.width > 0 && pixels.height > 0 && pixels.width <= 4096 && pixels.height <= 4096) {
			// Bound uploads and residency independently from the downloaded image size.
			const float maxWidth = pixels.key.find(".background") != std::string::npos ? 1280.f : 480.f;
			const float scale = std::min(1.f, std::min(maxWidth / pixels.width, 720.f / pixels.height));
			const int w = std::max(1, (int)(pixels.width * scale)), h = std::max(1, (int)(pixels.height * scale));
			pixels.data.resize((size_t)w * h * 4);
			for (int y = 0; y < h; ++y) {
				for (int x = 0; x < w; ++x) {
					memcpy(&pixels.data[(y * w + x) * 4], decoded + ((y * pixels.height / h) * pixels.width + x * pixels.width / w) * 4, 4);
				}
			}
			pixels.width = w;
			pixels.height = h;
		}
		free(decoded);
		std::lock_guard<std::mutex> guard(decodeMutex_);
		decoded_.push_back(std::move(pixels));
	}
}

SDL_Texture *Launcher::RemoteImage(const std::string &url) {
	if (url.empty()) {
		return nullptr;
	}
	const std::string key = StringFromFormat("preview-%016llx", (unsigned long long)Hash(url));
	auto found = artwork_.find(key);
	if (found != artwork_.end()) {
		found->second.use = ++use_;
		return found->second.texture;
	}
	if (!pendingArt_.count(key)) {
		std::lock_guard<std::mutex> lock(decodeMutex_);
		if (decodeQueue_.size() < 16) {
			Decode request;
			request.key = key;
			if (startsWith(url, "https://") || startsWith(url, "http://")) {
				request.url = url;
			} else {
				request.path = Path(url);
			}
			pendingArt_.insert(key);
			decodeQueue_.push_front(std::move(request));
			cancelArtwork_ = false;
			decodeWake_.notify_one();
		}
	}
	return nullptr;
}

void Launcher::PumpArtwork() {
	size_t requests = 0;
	{
		std::unique_lock<std::mutex> guard(decodeMutex_, std::try_to_lock);
		if (guard.owns_lock()) {
			requests = std::min<size_t>(metadataQueue_.size(), 8);
		}
	}
	for (size_t i = 0; i < requests; ++i) {
		Decode request;
		{
			std::lock_guard<std::mutex> guard(decodeMutex_);
			request = std::move(metadataQueue_.front());
			metadataQueue_.pop_front();
		}
		if (request.revision != artRevisions_[request.key]) {
			continue;
		}
		const bool background = request.key.find(".background") != std::string::npos;
		const auto imageFlag = background ? GameInfoFlags::PIC1 : GameInfoFlags::ICON;
		GameInfoFlags ready = GameInfoFlags::EMPTY;
		request.info = g_gameInfoCache->TryGetInfo(request.gamePath, GameInfoFlags::PARAM_SFO | imageFlag, &ready,
												   background ? TaskPriority::LOW : TaskPriority::NORMAL);
		std::lock_guard<std::mutex> guard(decodeMutex_);
		if (request.info && (ready & imageFlag)) {
			decodeQueue_.push_back(std::move(request));
			decodeWake_.notify_one();
		} else {
			metadataQueue_.push_back(std::move(request));
		}
	}
	if (!uploading_.key.empty() && uploading_.revision != artRevisions_[uploading_.key]) {
		SDL_DestroyTexture(uploadTexture_);
		uploadTexture_ = nullptr;
		uploading_ = {};
		uploadRow_ = 0;
	}
	if (uploading_.key.empty()) {
		std::unique_lock<std::mutex> guard(decodeMutex_, std::try_to_lock);
		if (!guard.owns_lock() || decoded_.empty()) {
			return;
		}
		uploading_ = std::move(decoded_.front());
		decoded_.pop_front();
		decodeWake_.notify_one();
		if (uploading_.revision != artRevisions_[uploading_.key]) {
			uploading_ = {};
			return;
		}
	}
	if (!uploading_.data.empty()) {
		if (!uploadTexture_) {
			uploadTexture_ =
				SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, uploading_.width, uploading_.height);
			if (uploadTexture_) {
				SDL_SetTextureBlendMode(uploadTexture_, SDL_BLENDMODE_BLEND);
			}
		}
		if (uploadTexture_) {
			const int rows = ArtworkUploadRows(uploading_.width, uploading_.height, uploadRow_, ARTWORK_UPLOAD_BYTES);
			const SDL_Rect rect{0, uploadRow_, uploading_.width, rows};
			if (!SDL_UpdateTexture(uploadTexture_, &rect, uploading_.data.data() + (size_t)uploadRow_ * uploading_.width * 4,
								   uploading_.width * 4)) {
				SDL_DestroyTexture(uploadTexture_);
				uploadTexture_ = nullptr;
			} else {
				uploadRow_ += rows;
				if (uploadRow_ < uploading_.height) {
					return;
				}
			}
		}
	}
	pendingArt_.erase(uploading_.key);
	artwork_[uploading_.key] = {uploadTexture_, ++use_};
	uploadTexture_ = nullptr;
	uploading_ = {};
	uploadRow_ = 0;
	while (artwork_.size() > MAX_ARTWORK) {
		auto victim =
			std::min_element(artwork_.begin(), artwork_.end(), [](const auto &a, const auto &b) { return a.second.use < b.second.use; });
		SDL_DestroyTexture(victim->second.texture);
		artwork_.erase(victim);
	}
}

void Launcher::InvalidateArtwork(Entry &entry) {
	for (const char *suffix : {".cover", ".background"}) {
		++artRevisions_[entry.key + suffix];
		pendingArt_.erase(entry.key + suffix);
		auto it = artwork_.find(entry.key + suffix);
		if (it != artwork_.end()) {
			SDL_DestroyTexture(it->second.texture);
			artwork_.erase(it);
		}
	}
}

void Launcher::Cover(Entry &entry, SDL_FRect rect, bool selected) {
	Fill(rect.x + 4, rect.y + 6, rect.w, rect.h, {0, 0, 0, 55});
	Fill(rect.x + 2, rect.y + 3, rect.w, rect.h, {0, 0, 0, 70});
	Fill(rect.x, rect.y, rect.w, rect.h, {22, 30, 49, 214});
	if (selected) {
		Fill(rect.x - 3, rect.y - 3, rect.w + 6, 2, palette_.accent);
		Fill(rect.x - 3, rect.y + rect.h + 1, rect.w + 6, 2, palette_.accent);
		Fill(rect.x - 3, rect.y - 1, 2, rect.h + 2, palette_.accent);
		Fill(rect.x + rect.w + 1, rect.y - 1, 2, rect.h + 2, palette_.accent);
	}
	SDL_Texture *texture = Artwork(entry);
	if (texture) {
		SDL_SetTextureAlphaMod(texture, 255);
		ImageFit(texture, rect, false);
	} else {
		Center(rect.x + rect.w / 2, rect.y + rect.h / 2 - 20, "PSP", 32);
	}
	if (showRegions_) {
		Flag(entry.region, rect.x + 8, rect.y + 8);
	}
	if (favorites_.count(entry.key)) {
		Icon(2, rect.x + rect.w - 18, rect.y + rect.h - 18, 24, palette_.value);
	}
	if (entry.customConfig && badges_) {
		Icon(3, rect.x + rect.w - 18, rect.y + 18, 24, palette_.text);
	}
}

void Launcher::RenderLibrary() {
	Background();
	const char *labels[] = {"Recent", "All games", "Favorites"};
	float x = 40;
	for (int i = 0; i < 3; ++i) {
		const float target = i == view_ ? 1.f : 0.f;
		tabOpen_[i] = animations_ ? tabOpen_[i] + (target - tabOpen_[i]) * .25f : target;
		const float w = 28 + (12 + TextWidth(labels[i], 26)) * tabOpen_[i];
		tabRects_[i] = {x - 12, 22, w + 24, 48};
		if (headerFocus_ && headerSelection_ == i) {
			Panel(x - 12, 22, w + 24, 48, true);
		}
		Icon(i, x + 14, 46, 28, i == view_ ? palette_.text : palette_.dim);
		SDL_Rect clip{(int)x + 40, 22, (int)w - 28, 48};
		SDL_SetRenderClipRect(renderer_, &clip);
		Text(x + 40, 30, labels[i], 26);
		SDL_SetRenderClipRect(renderer_, nullptr);
		x += w + 32;
	}
	if (headerFocus_ && headerSelection_ == 3) {
		Panel(WIDTH - 82, 22, 54, 48, true);
	}
	Icon(3, WIDTH - 55, 46, 28, palette_.text);
	if (visible_.empty()) {
		Center(WIDTH / 2, HEIGHT / 2 - 80,
			   SwitchLibrary::IsScanning() ? "Loading game library..."
			   : view_ == 0				   ? "Nothing played yet"
			   : view_ == 2				   ? "No favorites yet"
										   : "No games found",
			   32);
		Center(WIDTH / 2, HEIGHT / 2 - 22, "Add game folders in Settings to populate your library.", 20);
		Panel(WIDTH / 2 - 148, HEIGHT / 2 + 82, 296, 48, true);
		Center(WIDTH / 2, HEIGHT / 2 + 92, view_ == 1 ? "Settings" : "All games", 20);
	} else {
		if (recentPosition_ < 0 || !animations_) {
			recentPosition_ = (float)selection_;
		} else {
			recentPosition_ += (selection_ - recentPosition_) * .24f;
		}
		const int perPage = columns_ * rows_;
		const int first = view_ == 0 ? std::max(0, (int)std::floor(recentPosition_) - 1) : selection_ / perPage * perPage;
		const int last = std::min((int)visible_.size(), view_ == 0 ? (int)std::ceil(recentPosition_) + 6 : first + perPage);
		for (int i = first; i < last; ++i) {
			Entry &entry = games_[visible_[i]];
			const SDL_FRect rect = CoverRect(i);
			Cover(entry, rect, i == selection_ && !headerFocus_);
			if (titles_ && view_ != 0) {
				SDL_Rect clip{(int)rect.x - 6, (int)(rect.y + rect.h + 4), (int)rect.w + 12, 36};
				SDL_SetRenderClipRect(renderer_, &clip);
				const float overflow = std::max(0.f, TextWidth(entry.title, 20) - rect.w);
				const float scroll = i == selection_ && animations_ ? overflow * (.5f + .5f * std::sin(SDL_GetTicks() / 1500.f)) : 0;
				const float x = rect.x + std::max(0.f, (rect.w - TextWidth(entry.title, 20)) / 2) - scroll;
				Text(x, rect.y + rect.h + 6, entry.title, 20, i == selection_ && !headerFocus_ ? palette_.value : palette_.dim);
				SDL_SetRenderClipRect(renderer_, nullptr);
			}
		}
		if (view_ == 0) {
			const SDL_FRect rect = CoverRect(selection_);
			const std::string &title = games_[visible_[selection_]].title;
			const float x = rect.x + rect.w + 16, width = WIDTH - 40 - x;
			SDL_Rect clip{(int)x, 440, (int)std::max(0.f, width), 50};
			SDL_SetRenderClipRect(renderer_, &clip);
			const float overflow = std::max(0.f, TextWidth(title, 26) - width);
			const float scroll = animations_ ? overflow * (.5f + .5f * std::sin(SDL_GetTicks() / 1500.f)) : 0;
			Text(x - scroll, 440, title, 26);
			SDL_SetRenderClipRect(renderer_, nullptr);
		} else {
			for (int i = first + perPage; i < std::min((int)visible_.size(), first + perPage * 2); ++i) {
				QueueArtwork(games_[visible_[i]], false);
			}
		}
	}
	if (headerFocus_) {
		Footer("A  Open        B  Back");
	} else if (visible_.empty()) {
		Footer("A  Settings        B  Quit");
	} else {
		Footer();
	}
	Present();
}

void Launcher::Sound(int direction) {
	if (!audio_ || SDL_GetAudioStreamQueued(audio_) > 48000) {
		return;
	}
	const int milliseconds = direction < 0 ? 48 : direction > 0 ? 42 : 18;
	const float start = direction < 0 ? 760.f : direction > 0 ? 620.f : 920.f;
	const float end = direction < 0 ? 420.f : direction > 0 ? 980.f : 1040.f;
	const float volume = direction < 0 ? .14f : direction > 0 ? .16f : .10f;
	const int frames = 48000 * milliseconds / 1000;
	std::vector<int16_t> samples((size_t)frames * 2);
	double phase = 0;
	for (int i = 0; i < frames; ++i) {
		const float progress = (float)i / (frames - 1);
		phase += 6.283185307179586 * (start + (end - start) * progress) / 48000;
		const float release = 1.f - progress;
		const float envelope = std::min(1.f, progress * 10.f) * release * release;
		const int16_t sample = (int16_t)(std::sin(phase) * envelope * volume * 32767.f);
		samples[i * 2] = samples[i * 2 + 1] = sample;
	}
	SDL_PutAudioStreamData(audio_, samples.data(), (int)(samples.size() * sizeof(int16_t)));
}

bool Launcher::Poll(SDL_Event *event, bool horizontalStick) {
	pageDirection_ = 0;
	auto stickEvent = [&] {
		const int direction = stickNavigation_.Next(SDL_GetTicks(), horizontalStick);
		if (!direction) {
			return false;
		}
		*event = {};
		event->type = SDL_EVENT_KEY_DOWN;
		event->key.key = direction == -2 ? SDLK_UP : direction == 2 ? SDLK_DOWN : direction == -1 ? SDLK_LEFT : SDLK_RIGHT;
		return true;
	};
	if (!SDL_PollEvent(event)) {
		if (repeatDirection_ && SDL_GetTicks() >= repeatAt_) {
			*event = {};
			event->type = SDL_EVENT_KEY_DOWN;
			event->key.key = repeatKey_;
			event->key.repeat = true;
			repeatAt_ = SDL_GetTicks() + 170;
			return true;
		}
		return stickEvent();
	}
	SDL_ConvertEventToRenderCoordinates(renderer_, event);
	if (event->type == SDL_EVENT_QUIT) {
		running_ = false;
	}
	if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
		pads_[event->gdevice.which] = SDL_OpenGamepad(event->gdevice.which);
	} else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
		auto it = pads_.find(event->gdevice.which);
		if (it != pads_.end()) {
			SDL_CloseGamepad(it->second);
			pads_.erase(it);
		}
		libraryTriggers_.erase(event->gdevice.which);
		repeatDirection_ = 0;
	}
	if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
		(event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
		const size_t trigger = event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0 : 1;
		if (TriggerPressed(event->gaxis.value, libraryTriggers_[event->gaxis.which][trigger])) {
			pageDirection_ = trigger ? 1 : -1;
		}
	}
	if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
		(event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
		stickNavigation_.Axis(event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX, event->gaxis.value);
		stickEvent();
	}
	if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
		const SDL_Keycode keys[] = {SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT};
		if (event->gbutton.button >= SDL_GAMEPAD_BUTTON_DPAD_UP && event->gbutton.button <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
			repeatKey_ = keys[event->gbutton.button - SDL_GAMEPAD_BUTTON_DPAD_UP];
			repeatDirection_ = 1;
			repeatAt_ = SDL_GetTicks() + 400;
		}
	} else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
		repeatDirection_ = 0;
	}
	return true;
}

bool Launcher::Button(const SDL_Event &event, SDL_GamepadButton button, SDL_Keycode key) const {
	return (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == button) ||
		   (event.type == SDL_EVENT_KEY_DOWN && event.key.key == key);
}

bool Launcher::Confirm(const SDL_Event &event) const { return Button(event, SDL_GAMEPAD_BUTTON_EAST, SDLK_RETURN); }
bool Launcher::Back(const SDL_Event &event) const { return Button(event, SDL_GAMEPAD_BUTTON_SOUTH, SDLK_ESCAPE); }
bool Launcher::Menu(const SDL_Event &event) const { return Button(event, SDL_GAMEPAD_BUTTON_WEST, SDLK_Y); }

int Launcher::Navigate(const SDL_Event &event) {
	if (Button(event, SDL_GAMEPAD_BUTTON_DPAD_UP, SDLK_UP)) {
		return -2;
	}
	if (Button(event, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDLK_DOWN)) {
		return 2;
	}
	if (Button(event, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDLK_LEFT)) {
		return -1;
	}
	if (Button(event, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDLK_RIGHT)) {
		return 1;
	}
	return 0;
}

int Launcher::Rows(UiText rawtitleView, const std::function<std::vector<Row>()> &build, int initial, RowPage page) {
	const std::string_view titleView = rawtitleView;
	const std::string title(titleView);
	const bool hasPreview = page.preview || !page.imageUrl.empty();
	constexpr int sectionGap = 56;
	int selection = initial >= 0 ? initial : rowSelections_[std::string(title)], top = 0;
	std::vector<Row> rows;
	uint64_t refreshAt = 0;
	uint64_t enteredAt = SDL_GetTicks();
	float highlight = -1;
	float touchStart = 0;
	int touchSelection = selection;
	bool touchMoved = false;
	auto finish = [&](int result) {
		rowSelections_[std::string(title)] = selection;
		stickNavigation_.Reset();
		repeatDirection_ = 0;
		return result;
	};
	repeatDirection_ = 0;
	while (Frame()) {
		if (page.close && page.close()) {
			return finish(-1);
		}
		if (rows.empty() || SDL_GetTicks() >= refreshAt) {
			rows = build();
			refreshAt = SDL_GetTicks() + 500;
		}
		if (rows.empty()) {
			Message(title, "No entries available.");
			return finish(-1);
		}
		const int selectionCount = (int)rows.size() + (page.updateButton ? 1 : 0);
		selection = std::clamp(selection, 0, selectionCount - 1);
		const int gap = page.sectionStart > 0 ? sectionGap : 0;
		const auto layout = LayoutMenuRows((int)rows.size(),
										   hasPreview				   ? 148
										   : page.firstSection.empty() ? 104
																	   : 144,
										   HEIGHT - (page.updateButton ? 210 : 112), gap, hasPreview && rows.size() <= 12);
		const int listTop = layout.top, rowHeight = layout.height, visible = layout.visible;
		const int columnX = hasPreview ? 360 : 150, columnWidth = hasPreview ? WIDTH - 416 : 980;
		auto rowY = [&](int index) {
			return listTop + (index - top) * rowHeight + (page.sectionStart > 0 && index >= page.sectionStart ? sectionGap : 0);
		};
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event)) {
				Sound(-1);
				return finish(-1);
			}
			const int direction = Navigate(event);
			if (std::abs(direction) == 2) {
				selection = (selection + (direction > 0 ? 1 : -1) + selectionCount) % selectionCount;
				Sound(0);
			}
			if (event.type == SDL_EVENT_MOUSE_WHEEL) {
				selection = std::clamp(selection - (int)event.wheel.y, 0, (int)rows.size() - 1);
			}
			if (event.type == SDL_EVENT_FINGER_DOWN) {
				touchStart = event.tfinger.y;
				touchSelection = selection;
				touchMoved = false;
			}
			if (event.type == SDL_EVENT_FINGER_MOTION && std::abs(event.tfinger.y - touchStart) > 16) {
				touchMoved = true;
				selection =
					std::clamp(touchSelection - (int)std::round((event.tfinger.y - touchStart) / rowHeight), 0, (int)rows.size() - 1);
			}
			bool activate = Confirm(event);
			if (event.type == SDL_EVENT_MOUSE_BUTTON_UP || (event.type == SDL_EVENT_FINGER_UP && !touchMoved)) {
				float x = event.type == SDL_EVENT_FINGER_UP ? event.tfinger.x : event.button.x;
				float y = event.type == SDL_EVENT_FINGER_UP ? event.tfinger.y : event.button.y;
				if (y < 80 || y >= HEIGHT - 40) {
					return finish(-1);
				}
				if (x >= columnX && x < columnX + columnWidth) {
					for (int hit = top; hit < std::min(top + visible, (int)rows.size()); ++hit) {
						if (y >= rowY(hit) && y < rowY(hit) + rowHeight) {
							selection = hit;
							activate = true;
							break;
						}
					}
				}
			}
			if (page.updateButton && (event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_FINGER_UP) &&
				(event.type == SDL_EVENT_FINGER_UP ? event.tfinger.x : event.button.x) >= WIDTH / 2 - 250 &&
				(event.type == SDL_EVENT_FINGER_UP ? event.tfinger.x : event.button.x) <= WIDTH / 2 + 250 &&
				(event.type == SDL_EVENT_FINGER_UP ? event.tfinger.y : event.button.y) >= HEIGHT - 184 &&
				(event.type == SDL_EVENT_FINGER_UP ? event.tfinger.y : event.button.y) < HEIGHT - 128) {
				selection = (int)rows.size();
				activate = true;
			}
			if (selection == (int)rows.size()) {
				if (activate) {
					Updates();
					refreshAt = 0;
				}
				continue;
			}
			if (page.input && page.input(selection, event)) {
				refreshAt = 0;
				break;
			}
			Row &row = rows[selection];
			if (page.hints.empty() && Button(event, SDL_GAMEPAD_BUTTON_NORTH, SDLK_X)) {
				Message(row.label, !row.description.empty() ? row.description
								   : title == row.label		? row.value
															: StringFromFormat(Tr("%s\n\n%s\nCurrent value: %s").data(), title.c_str(),
																			   row.label.c_str(), row.value.c_str()));
			}
			if (Button(event, SDL_GAMEPAD_BUTTON_WEST, SDLK_Y) && row.reset) {
				row.reset();
				refreshAt = 0;
			}
			if ((activate || (page.horizontalChanges && std::abs(direction) == 1)) && row.enabled) {
				if (!row.change) {
					Sound(1);
					return finish(selection);
				}
				const uint64_t changeStarted = SDL_GetTicks();
				row.change(direction);
				refreshAt = 0;
				if (SDL_GetTicks() - changeStarted > 200) {
					enteredAt = SDL_GetTicks();
				}
				Sound(1);
				break;
			}
		}
		const int listSelection = std::min(selection, (int)rows.size() - 1);
		top = std::clamp(top, std::max(0, listSelection - visible + 1), listSelection);
		Background();
		PageHeader(page.preview && page.eyebrow == "Game menu" ? page.preview->title : title, Tr(page.eyebrow),
				   page.preview ? page.preview->id : page.context, page.preview ? "PSP" : "");
		if (hasPreview) {
			Panel(46, 161, 280, 410);
			if (page.preview) {
				Cover(*page.preview, {56, 171, 260, 390}, false);
			} else if (auto *image = RemoteImage(page.imageUrl)) {
				ImageFit(image, {56, 171, 260, 390}, false);
			}
		}
		auto section = [&](std::string_view heading, int begin, int end) {
			const int first = std::max(begin, top), last = std::min(end, top + visible);
			if (first >= last) {
				return;
			}
			if (first == begin && !heading.empty()) {
				SectionHeading(Tr(heading), columnX, rowY(first) - 44, columnWidth);
			}
			Panel(columnX - 12, rowY(first) - 10, columnWidth + 24, rowY(last - 1) - rowY(first) + rowHeight + 18);
		};
		if (page.sectionStart > 0) {
			section(page.firstSection, 0, page.sectionStart);
			section(page.secondSection, page.sectionStart, (int)rows.size());
		} else {
			section(page.firstSection, 0, (int)rows.size());
		}
		const float target = rowY(selection) + 1.f;
		highlight = !animations_ || highlight < 0 ? target : highlight + (target - highlight) * .3f;
		if (selection < (int)rows.size()) {
			Round(columnX, highlight, columnWidth, rowHeight - 2, 4, {28, 69, 92, 255});
			Fill(columnX, highlight + rowHeight / 5, 3, rowHeight * 3 / 5, palette_.accent);
		}
		const int labelX = columnX + (hasPreview ? 24 : 40), valueX = columnX + columnWidth - (hasPreview ? 24 : 40);
		for (int i = top; i < std::min(top + visible, (int)rows.size()); ++i) {
			const float y = rowY(i);
			const SDL_Color color = !rows[i].enabled	   ? SDL_Color{100, 110, 125, 255}
									: rows[i].labelColor.a ? rows[i].labelColor
														   : palette_.text;
			const int labelSize = std::min(24, rowHeight * 3 / 5), valueSize = std::min(22, labelSize);
			const int valueWidth =
				rows[i].value.empty() ? 0 : std::min(columnWidth / 2, (int)std::ceil(TextWidth(rows[i].value, valueSize))) + 20;
			const int labelWidth = std::max(1, valueX - labelX - valueWidth);
			SDL_Rect labelClip{labelX, (int)y, labelWidth, rowHeight};
			SDL_SetRenderClipRect(renderer_, &labelClip);
			const float overflow = i == selection ? std::max(0.f, TextWidth(rows[i].label, labelSize) - labelWidth) : 0;
			const float scroll = animations_ && overflow ? (std::sin(SDL_GetTicks() / 2200.f - 1.5707963f) + 1) * overflow / 2 : 0;
			Text(labelX - scroll, y + (rowHeight - labelSize * 4 / 3) / 2, rows[i].label, labelSize,
				 i == selection ? palette_.value : color);
			SDL_SetRenderClipRect(renderer_, nullptr);
			if (rows[i].value.empty()) {
				continue;
			}
			SDL_Rect valueClip{labelX + labelWidth + 20, (int)y, valueX - labelX - labelWidth - 20, rowHeight};
			SDL_SetRenderClipRect(renderer_, &valueClip);
			Text(valueX - TextWidth(rows[i].value, valueSize), y + (rowHeight - valueSize * 4 / 3) / 2, rows[i].value, valueSize,
				 rows[i].valueColor.a ? rows[i].valueColor
				 : i == selection	  ? palette_.value
									  : palette_.dim);
			SDL_SetRenderClipRect(renderer_, nullptr);
		}
		if ((int)rows.size() > visible) {
			const int track = visible * rowHeight, thumb = std::max(16, track * visible / (int)rows.size());
			Fill(columnX + columnWidth + 16, listTop, 4, track, {40, 44, 54, 255});
			Fill(columnX + columnWidth + 16, listTop + (track - thumb) * top / std::max(1, (int)rows.size() - visible), 4, thumb,
				 palette_.accent);
		}
		if (page.updateButton) {
			const bool selected = selection == (int)rows.size();
			Panel(WIDTH / 2 - 250, HEIGHT - 184, 500, 56, selected);
			Center(WIDTH / 2, HEIGHT - 172, "Check for updates", 26);
			const char *status = !updateTask_ ? "Check for the latest release" : updateAvailable_ ? "Update available" : "PPSSPP";
			Center(WIDTH / 2, HEIGHT - 122, status, 18);
		}
		if (!page.hints.empty()) {
			Footer(page.hints);
		} else if (selection >= (int)rows.size()) {
			Footer("A  Open        B  Back");
		} else {
			const Row &row = rows[selection];
			std::string hints = "A  " + std::string(Tr(row.change ? "Choose" : "Select"));
			if (row.change && row.enabled) {
				hints += "        Left / Right  " + std::string(Tr("Change"));
			}
			hints += "        X  " + std::string(Tr("Info"));
			if (row.reset) {
				hints += "        Y  " + std::string(Tr("Reset"));
			}
			hints += "        B  " + std::string(Tr("Back"));
			Footer(hints);
		}
		if (animations_) {
			const float fade = std::max(0.f, 1.f - (SDL_GetTicks() - enteredAt) / 180.f);
			if (fade > 0) {
				Fill(0, 0, WIDTH, HEIGHT, {0, 0, 0, (uint8_t)(fade * 255)});
			}
		}
		Present();
	}
	return finish(-1);
}

bool Launcher::Prompt(UiText rawtitle, std::string *value, bool password) {
	const std::string_view title = rawtitle;
	SwkbdConfig keyboard{};
	if (R_FAILED(swkbdCreate(&keyboard, 0))) {
		return false;
	}
	if (password) {
		swkbdConfigMakePresetPassword(&keyboard);
	} else {
		swkbdConfigMakePresetDefault(&keyboard);
	}
	swkbdConfigSetHeaderText(&keyboard, std::string(title).c_str());
	swkbdConfigSetInitialText(&keyboard, value->c_str());
	std::array<char, 1024> text{};
	::Result rc = swkbdShow(&keyboard, text.data(), text.size());
	swkbdClose(&keyboard);
	repeatDirection_ = 0;
	if (R_FAILED(rc)) {
		return false;
	}
	*value = text.data();
	return true;
}

bool Launcher::Ask(UiText rawtitle, UiText rawmessage) {
	const std::string title((std::string_view)rawtitle), message((std::string_view)rawmessage);
	constexpr int width = WIDTH * 3 / 4, buttonWidth = 210, buttonHeight = 56;
	const auto lines = Wrap(message, width - 64, 24);
	const int lineCount = std::min(6, (int)lines.size());
	const int height = 220 + lineCount * 36, x = (WIDTH - width) / 2, y = (HEIGHT - height) / 2;
	const SDL_FRect yes{WIDTH / 2 - buttonWidth - 12.f, (float)(y + height - buttonHeight - 22), buttonWidth, buttonHeight};
	const SDL_FRect no{WIDTH / 2 + 12.f, yes.y, buttonWidth, buttonHeight};
	auto finish = [&](bool result) {
		stickNavigation_.Reset();
		repeatDirection_ = 0;
		return result;
	};
	while (Frame()) {
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event)) {
				return finish(false);
			}
			if (Confirm(event)) {
				return finish(true);
			}
			if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
				const SDL_FPoint point{event.type == SDL_EVENT_FINGER_UP ? event.tfinger.x : event.button.x,
									   event.type == SDL_EVENT_FINGER_UP ? event.tfinger.y : event.button.y};
				if (SDL_PointInRectFloat(&point, &yes)) {
					return finish(true);
				}
				if (SDL_PointInRectFloat(&point, &no)) {
					return finish(false);
				}
			}
		}
		Background();
		Fill(0, 0, WIDTH, HEIGHT, {0, 0, 0, 100});
		Panel(x, y, width, height);
		SDL_Rect clip{x + 28, y + 18, width - 56, 48};
		SDL_SetRenderClipRect(renderer_, &clip);
		Text(x + 28, y + 22, title, 30, {238, 135, 135, 255});
		SDL_SetRenderClipRect(renderer_, nullptr);
		Fill(x + 28, y + 76, width - 56, 1, {255, 255, 255, 24});
		for (int i = 0; i < lineCount; ++i) {
			Center(WIDTH / 2, y + 96 + i * 36, lines[i], 24);
		}
		auto choice = [&](SDL_FRect rect, UiText button, UiText label, SDL_Color face, SDL_Color edge) {
			Round(rect.x, rect.y, rect.w, rect.h, 6, edge);
			Round(rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2, 5, face);
			const float left = rect.x + (rect.w - TextWidth(label, 24) - 40) / 2;
			Round(left, rect.y + 15, 26, 26, 13, {5, 35, 56, 235});
			Center(left + 13, rect.y + 17, button, 16);
			Text(left + 38, rect.y + 12, label, 24);
		};
		choice(yes, "A", "Yes", {115, 44, 51, 255}, {235, 125, 125, 255});
		choice(no, "B", "No", {28, 69, 92, 255}, palette_.accent);
		Present();
	}
	return finish(false);
}

int Launcher::Choose(UiText rawtitle, const std::vector<std::string> &choices, int initial) {
	if (choices.empty()) {
		return -1;
	}
	const std::string title((std::string_view)rawtitle);
	constexpr int width = WIDTH * 3 / 4, rowHeight = 44;
	const int visible = std::min(8, (int)choices.size()), height = 152 + visible * rowHeight;
	const int x = (WIDTH - width) / 2, y = (HEIGHT - height) / 2, listTop = y + 76;
	int selection = std::clamp(initial, 0, (int)choices.size() - 1), top = 0;
	auto finish = [&](int result) {
		stickNavigation_.Reset();
		repeatDirection_ = 0;
		return result;
	};
	while (Frame()) {
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event)) {
				return finish(-1);
			}
			if (Confirm(event)) {
				return finish(selection);
			}
			const int direction = Navigate(event);
			if (direction) {
				selection = (selection + (direction > 0 ? 1 : -1) + (int)choices.size()) % (int)choices.size();
			}
			if (event.type == SDL_EVENT_MOUSE_WHEEL) {
				selection = std::clamp(selection - (int)event.wheel.y, 0, (int)choices.size() - 1);
			}
			if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
				const float px = event.type == SDL_EVENT_FINGER_UP ? event.tfinger.x : event.button.x;
				const float py = event.type == SDL_EVENT_FINGER_UP ? event.tfinger.y : event.button.y;
				if (py >= HEIGHT - 40) {
					return finish(-1);
				}
				if (px >= x + 20 && px < x + width - 20 && py >= listTop && py < listTop + visible * rowHeight) {
					return finish(top + (int)((py - listTop) / rowHeight));
				}
			}
		}
		top = std::clamp(top, std::max(0, selection - visible + 1), selection);
		Background();
		Fill(0, 0, WIDTH, HEIGHT, {0, 0, 0, 100});
		Panel(x, y, width, height);
		Wrapped(x + 28, y + 22, width - 56, title, 30, 1, palette_.text);
		for (int i = top; i < std::min(top + visible, (int)choices.size()); ++i) {
			const int rowY = listTop + (i - top) * rowHeight;
			if (i == selection) {
				Round(x + 20, rowY, width - 40, rowHeight - 2, 4, {28, 69, 92, 255});
			}
			SDL_Rect clip{x + 32, rowY, width - 64, rowHeight};
			SDL_SetRenderClipRect(renderer_, &clip);
			Text(x + 32, rowY + 7, choices[i], 24, i == selection ? palette_.value : palette_.text);
			SDL_SetRenderClipRect(renderer_, nullptr);
		}
		Footer("A  Select        B  Back");
		Present();
	}
	return finish(-1);
}

bool Launcher::Busy(UiText rawtitle, const std::function<void(const std::atomic<bool> &)> &work, bool cancelable) {
	const std::string title((std::string_view)rawtitle);
	transferred_ = 0;
	operationProgress_ = -1.f;
	{
		std::lock_guard<std::mutex> lock(progressMutex_);
		progress_.clear();
	}
	std::atomic<bool> done{false}, cancel{false};
	std::thread thread([&] {
		SetCurrentThreadAffinity(ThreadAffinityRole::IO);
		work(cancel);
		done.store(true, std::memory_order_release);
	});
	while (!done.load(std::memory_order_acquire)) {
		if (!Frame()) {
			cancel.store(true, std::memory_order_relaxed);
		}
		SDL_Event event{};
		while (Poll(&event)) {
			if (cancelable && Back(event)) {
				cancel.store(true, std::memory_order_relaxed);
			}
		}
		Background();
		Center(WIDTH / 2, HEIGHT / 2 - 40, title, 32);
		std::string detail;
		{
			std::lock_guard<std::mutex> lock(progressMutex_);
			detail = progress_;
		}
		Center(WIDTH / 2, HEIGHT / 2 + 10,
			   cancel			? Tr("Cancelling...")
			   : detail.empty() ? Tr("Please wait...")
								: std::string_view(detail),
			   22);
		if (transferred_) {
			Center(WIDTH / 2, HEIGHT / 2 + 54, NiceSizeFormat(transferred_.load()), 20);
		}
		const float progress = operationProgress_.load();
		if (progress >= 0) {
			Round(220, 410, WIDTH - 440, 12, 6, {45, 55, 75, 255});
			Round(220, 410, (WIDTH - 440) * std::clamp(progress, 0.f, 1.f), 12, 6, palette_.accent);
		}
		Footer(cancelable ? "B  Cancel" : "Please wait...");
		Present();
	}
	thread.join();
	repeatDirection_ = 0;
	return !cancel;
}

bool Launcher::Number(UiText title, float *value, float minimum, float maximum, float step, float defaultValue,
					  const std::function<std::string(float)> &format) {
	if (!(maximum > minimum) || !(step > 0)) {
		return false;
	}
	float current = std::clamp(std::isfinite(*value) ? *value : defaultValue, minimum, maximum);
	auto text = [&](float v) { return format ? format(v) : StringFromFormat("%.2f", v); };
	auto adjust = [&](float v) { current = SliderValue(v, minimum, maximum, step); };
	bool dragging = false;
	repeatDirection_ = 0;
	while (Frame()) {
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event)) {
				repeatDirection_ = 0;
				return false;
			}
			if (Confirm(event)) {
				*value = current;
				repeatDirection_ = 0;
				return true;
			}
			const int direction = Navigate(event);
			if (direction) {
				adjust(current + (direction > 0 ? 1 : -1) * (std::abs(direction) == 1 ? step : std::max(step, (maximum - minimum) / 20)));
			}
			if (Button(event, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDLK_PAGEUP)) {
				adjust(current - std::max(step, (maximum - minimum) / 20));
			}
			if (Button(event, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDLK_PAGEDOWN)) {
				adjust(current + std::max(step, (maximum - minimum) / 20));
			}
			if (Menu(event)) {
				current = std::clamp(defaultValue, minimum, maximum);
			}
			if (Button(event, SDL_GAMEPAD_BUTTON_NORTH, SDLK_X)) {
				std::string input = StringFromFormat("%.2f", current);
				float parsed;
				if (Prompt(title, &input) && TryParse(input, &parsed) && std::isfinite(parsed)) {
					// Typed values may use finer increments than the slider (for example a network port).
					current = std::clamp(parsed, minimum, maximum);
				}
			}
			const bool down = event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			const bool motion = event.type == SDL_EVENT_FINGER_MOTION || event.type == SDL_EVENT_MOUSE_MOTION;
			const bool up = event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP;
			if (down || motion || up) {
				const bool finger =
					event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION || event.type == SDL_EVENT_FINGER_UP;
				const float x = finger ? event.tfinger.x : motion ? event.motion.x : event.button.x;
				const float y = finger ? event.tfinger.y : motion ? event.motion.y : event.button.y;
				if (down) {
					dragging = x >= 150 && x <= WIDTH - 150 && y >= 340 && y <= 430;
				}
				if (dragging) {
					adjust(minimum + std::clamp((x - 180) / (WIDTH - 360), 0.f, 1.f) * (maximum - minimum));
				}
				if (up) {
					dragging = false;
					if (y >= 490 && y <= 550) {
						if (x >= WIDTH / 2 + 20 && x <= WIDTH / 2 + 240) {
							*value = current;
							repeatDirection_ = 0;
							return true;
						}
						if (x >= WIDTH / 2 - 240 && x <= WIDTH / 2 - 20) {
							repeatDirection_ = 0;
							return false;
						}
					}
				}
			}
		}
		Background();
		PageHeader(title, "Settings");
		Panel(110, 220, WIDTH - 220, 350);
		Center(WIDTH / 2, 260, text(current), 36, palette_.value);
		const float width = WIDTH - 360;
		const float position = (current - minimum) / (maximum - minimum) * width;
		Round(180, 378, width, 8, 4, palette_.dim);
		Round(180, 378, position, 8, 4, palette_.accent);
		Round(168 + position, 370, 24, 24, 12, palette_.value);
		Text(180, 414, text(minimum), 20, palette_.dim);
		Text(WIDTH - 180 - TextWidth(text(maximum), 20), 414, text(maximum), 20, palette_.dim);
		Panel(WIDTH / 2 - 240, 490, 220, 54);
		Center(WIDTH / 2 - 130, 502, "Cancel", 24);
		Panel(WIDTH / 2 + 20, 490, 220, 54, true);
		Center(WIDTH / 2 + 130, 502, "Apply", 24);
		Footer("Left / Right  Change        L / R  +/- 5%        X  Enter value        Y  Reset        A  Apply        B  Cancel");
		Present();
	}
	return false;
}

static Path StorageParent(const Path &path) {
	const std::string &text = path.ToString();
	const size_t colon = text.find(':');
	if (text == "/" || (colon != std::string::npos && text.find_first_not_of('/', colon + 1) == std::string::npos)) {
		return {};
	}
	return path.NavigateUp();
}

Path Launcher::StorageLocation() {
	while (Frame()) {
		SwitchStorage::InitializeUsb();
		std::vector<SwitchStorage::Location> usb;
		std::vector<SwitchStorage::SmbShare> shares;
		RowPage page;
		page.horizontalChanges = false;
		page.hints = "A  Open        X  Actions        B  Back";
		page.input = [&](int selected, const SDL_Event &event) {
			if (!Button(event, SDL_GAMEPAD_BUTTON_NORTH, SDLK_X)) {
				return false;
			}
			if (selected > 0 && selected <= (int)usb.size()) {
				const auto location = usb[selected - 1];
				if (Ask("Safely eject USB?", location.label)) {
					std::string error;
					if (!SwitchStorage::SafelyEjectUsb(location.id, &error)) {
						Message("Could not eject USB", error);
					}
				}
			} else if (selected > (int)usb.size() && selected <= (int)(usb.size() + shares.size())) {
				const auto share = shares[selected - usb.size() - 1];
				const int action = Rows(share.name, [] { return std::vector<Row>{{"Edit", ">"}, {"Disconnect", ">"}, {"Remove", ">"}}; });
				if (action == 0) {
					EditShare(share);
				} else if (action == 1) {
					SwitchStorage::UnmountSmb(share.id);
				} else if (action == 2 && Ask("Remove share?", share.name)) {
					SwitchLibrary::RemoveSmbShare(share.id);
				}
			}
			return true;
		};
		const int selected = Rows(
			"Storage locations",
			[&] {
				usb = SwitchStorage::ListUsbLocations();
				shares = SwitchLibrary::GetSmbShares();
				std::vector<Row> rows{{"SD card", ">"}};
				for (const auto &location : usb) {
					rows.push_back({location.label, "USB"});
				}
				for (const auto &share : shares) {
					rows.push_back({share.name, SwitchStorage::IsSmbMounted(share.id) ? "SMB" : "Disconnected"});
				}
				rows.push_back({"Add SMB share", ">"});
				return rows;
			},
			-1, page);
		if (selected < 0) {
			return {};
		}
		if (selected == 0) {
			return Path("/");
		}
		if (selected <= (int)usb.size()) {
			return Path(usb[selected - 1].path);
		}
		if (selected <= (int)(usb.size() + shares.size())) {
			const auto &share = shares[selected - usb.size() - 1];
			if (!SwitchStorage::IsSmbMounted(share.id)) {
				std::string error;
				bool ok = false;
				Busy("Connecting to SMB", [&](const auto &cancel) { ok = SwitchStorage::ReconnectSmb(share.id, &error, &cancel); });
				if (!ok) {
					Message("Could not connect", error);
					continue;
				}
			}
			return Path(SwitchStorage::SmbBrowsePath(share));
		}
		EditShare({});
	}
	return {};
}

std::string Launcher::Browse(Path directory, bool folder, bool images, bool manage) {
	while (Frame()) {
		if (directory.empty()) {
			directory = StorageLocation();
			if (directory.empty()) {
				return {};
			}
		}
		std::vector<File::FileInfo> files;
		std::vector<Row> rows;
		bool reload = true;
		const int fileStart = folder ? 2 : 1;
		auto build = [&] {
			if (!reload) {
				return rows;
			}
			reload = false;
			files.clear();
			rows.clear();
			File::GetFilesInDir(directory, &files);
			if (!manage) {
				RemoveDescribedDiscImages(&files);
			}
			files.erase(std::remove_if(files.begin(), files.end(),
									   [&](const auto &file) {
										   if (file.name == "." || file.name == "..") {
											   return true;
										   }
										   if (file.isDirectory || manage) {
											   return false;
										   }
										   const auto ext = Lower(file.fullName.GetFileExtension());
										   if (folder) {
											   return true;
										   }
										   if (images) {
											   return ext != ".png" && ext != ".jpg" && ext != ".jpeg";
										   }
										   return ext != ".iso" && ext != ".cso" && ext != ".chd" && ext != ".pbp" && ext != ".elf" &&
												  ext != ".prx" && ext != ".ppst" && ext != ".ppdmp" && ext != ".cue" && ext != ".ccd" &&
												  ext != ".mds" && ext != ".bin" && ext != ".img" && ext != ".zip" && ext != ".7z" &&
												  ext != ".pkg";
									   }),
						files.end());
			std::stable_sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
				return a.isDirectory != b.isDirectory ? a.isDirectory : Lower(a.name) < Lower(b.name);
			});
			if (folder) {
				rows.push_back({"Use this folder", directory.ToVisualString()});
			}
			rows.push_back({"Parent folder", ".."});
			for (const auto &file : files) {
				rows.push_back({file.name, file.isDirectory ? ">" : NiceSizeFormat(file.size)});
			}
			return rows;
		};
		RowPage page;
		page.horizontalChanges = false;
		page.hints =
			manage ? "A  Open        X  Actions        Y  Paste        +  New folder        B  Parent" : "A  Select        B  Parent";
		page.input = [&](int selected, const SDL_Event &event) {
			if (manage && Button(event, SDL_GAMEPAD_BUTTON_NORTH, SDLK_X)) {
				FileActions(selected >= fileStart ? files[selected - fileStart].fullName : directory);
				reload = true;
				return true;
			}
			if (manage && Button(event, SDL_GAMEPAD_BUTTON_WEST, SDLK_Y)) {
				Paste(directory);
				reload = true;
				return true;
			}
			if (manage && Button(event, SDL_GAMEPAD_BUTTON_START, SDLK_PLUS)) {
				reload = true;
				std::string name;
				if (Prompt("New folder", &name) && !name.empty() && name != "." && name != ".." &&
					name.find_first_of("/\\:") == std::string::npos && !File::CreateDir(directory / name)) {
					Message("Could not create folder", name);
				}
				return true;
			}
			return false;
		};
		// Return to the same folder entry after going up, and start at the top on first visit.
		const int selected = Rows(directory.ToVisualString(), build, -1, page);
		if (selected < 0 || selected == (folder ? 1 : 0)) {
			directory = StorageParent(directory);
			continue;
		}
		if (folder && selected == 0) {
			return directory.ToString();
		}
		const auto &file = files[selected - fileStart];
		if (file.isDirectory) {
			directory = file.fullName;
		} else if (manage) {
			FileActions(file.fullName);
		} else {
			return file.fullName.ToString();
		}
	}
	return {};
}

void Launcher::Updates() {
	if (!updateTask_) {
		updateTask_ = std::make_unique<SwitchUpdateTask>();
		updateTask_->StartCheck();
	}
	while (Frame()) {
		auto snapshot = updateTask_->Snapshot();
		if (snapshot.state == SwitchUpdateState::ReadyToInstall) {
			if (updateTask_->Install()) {
				result_.action = Action::Restart;
				return;
			}
			snapshot = updateTask_->Snapshot();
		}
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event)) {
				if (snapshot.state == SwitchUpdateState::Downloading) {
					updateTask_->Cancel();
				}
				return;
			}
			if (Confirm(event)) {
				if (snapshot.state == SwitchUpdateState::UpdateAvailable) {
					updateTask_->StartDownload();
				} else if (snapshot.state != SwitchUpdateState::Downloading && snapshot.state != SwitchUpdateState::Checking) {
					updateTask_->StartCheck();
				}
			}
			if (Button(event, SDL_GAMEPAD_BUTTON_NORTH, SDLK_X)) {
				Message("Release notes", snapshot.release.notes);
			}
		}
		Background();
		Text(40, 9, "PPSSPP", 18);
		Text(40, 31, "Updates", 32);
		Panel(138, 156, WIDTH - 276, 420);
		std::string status;
		switch (snapshot.state) {
		case SwitchUpdateState::Checking:
			status = Tr("Checking GitHub for the latest release...");
			break;
		case SwitchUpdateState::UpdateAvailable:
			status = StringFromFormat(Tr("New version available: %s").data(), snapshot.release.tag.c_str());
			break;
		case SwitchUpdateState::UpToDate:
			status = Tr("PPSSPP is up to date");
			break;
		case SwitchUpdateState::Downloading:
			status = StringFromFormat(Tr("Downloading %s").data(), snapshot.release.assetName.c_str());
			break;
		case SwitchUpdateState::Cancelled:
			status = Tr("Update cancelled");
			break;
		case SwitchUpdateState::Error:
			status = snapshot.error;
			break;
		default:
			status = Tr("Installing update...");
			break;
		}
		Center(WIDTH / 2, 204, status, 26);
		Center(WIDTH / 2, 260, snapshot.release.name, 24);
		if (snapshot.total) {
			const float ratio = std::clamp((float)((double)snapshot.downloaded / snapshot.total), 0.f, 1.f);
			Round(220, 348, WIDTH - 440, 12, 6, {45, 55, 75, 255});
			Round(220, 348, (WIDTH - 440) * ratio, 12, 6, palette_.accent);
			Center(WIDTH / 2, 388, NiceSizeFormat(snapshot.downloaded) + " / " + NiceSizeFormat(snapshot.total), 22);
		}
		Footer(snapshot.state == SwitchUpdateState::UpdateAvailable ? "A  Download and install        X  Release notes        B  Back"
																	: "A  Check again        X  Release notes        B  Cancel / Back");
		Present();
	}
}

void Launcher::MoveLibraryState(const Path &source, const Path &target) {
	Section *aliases = ini_.GetOrCreateSection("Aliases");
	for (Entry &entry : games_) {
		std::string relative;
		if (!source.ComputePathTo(entry.path, relative)) {
			continue;
		}
		const Path next = relative.empty() ? target : target / relative;
		const std::string oldKey = entry.key, newKey = ArtKey(next);
		if (favorites_.erase(oldKey)) {
			favorites_.insert(newKey);
		}
		for (std::string &key : recentKeys_) {
			if (key == oldKey) {
				key = newKey;
			}
		}
		std::string alias;
		if (aliases->Get(oldKey, &alias)) {
			aliases->Set(newKey, alias);
			aliases->Delete(oldKey);
		}
		for (const char *suffix : {".cover.png", ".background.png"}) {
			Path oldPath(std::string(ART_DIRECTORY) + "/" + oldKey + suffix), newPath(std::string(ART_DIRECTORY) + "/" + newKey + suffix);
			if (File::Exists(oldPath) && !File::Exists(newPath)) {
				File::Rename(oldPath, newPath);
			}
		}
		InvalidateArtwork(entry);
		if (g_recentFiles.ContainsFile(entry.path.ToString())) {
			g_recentFiles.Remove(entry.path.ToString());
			g_recentFiles.Add(next.ToString());
		}
		entry.path = next;
		entry.identity = StableGameIdentity(next);
		entry.key = newKey;
		entry.info = g_gameInfoCache->GetInfo(nullptr, next, GameInfoFlags::PARAM_SFO);
	}
	const auto sources = SwitchLibrary::GetSources();
	for (size_t i = sources.size(); i > 0; --i) {
		const Path resolved(SwitchLibrary::ResolveSource(sources[i - 1]));
		std::string relative;
		if (source.ComputePathTo(resolved, relative)) {
			SwitchLibrary::RemoveSource(i - 1);
			SwitchLibrary::AddSource(relative.empty() ? target : target / relative);
		}
	}
	Save();
}

void Launcher::ChangeLanguage() {
	std::vector<File::FileInfo> files;
	g_VFS.GetFileListing("lang", &files, "ini");
	std::vector<std::string> languages;
	std::vector<Row> rows;
	const auto &names = GetLangValuesMapping();
	for (const auto &file : files) {
		std::string code = file.name.substr(0, file.name.find('.'));
		auto found = names.find(code);
		languages.push_back(code);
		rows.push_back({found == names.end() ? code : found->second.first, code == g_Config.sLanguageIni ? "Selected" : ""});
	}
	int choice = Rows("PPSSPP language", [&] { return rows; });
	if (choice >= 0 && g_i18nrepo.LoadIni(languages[choice])) {
		g_Config.sLanguageIni = languages[choice];
		LoadFonts();
	}
}

void Launcher::Shaders() {
	ReloadAllPostShaderInfo(nullptr);
	Rows("Post-processing and texture shaders", [&] {
		std::vector<Row> rows;
		for (size_t index = 0; index < g_Config.vPostShaderNames.size(); ++index) {
			const std::string name = g_Config.vPostShaderNames[index];
			rows.push_back(
				{name, ">", [&, name, index](int) {
					 Rows(name, [&] {
						 std::vector<Row> options;
						 for (const auto *shader : GetPostShaderChain(name)) {
							 for (int i = 0; i < 4; ++i) {
								 const auto setting = shader->settings[i];
								 if (setting.name.empty()) {
									 continue;
								 }
								 const std::string key = StringFromFormat("%sSettingCurrentValue%d", shader->section.c_str(), i + 1);
								 if (!g_Config.mPostShaderSetting.count(key)) {
									 g_Config.mPostShaderSetting[key] = setting.value;
								 }
								 options.push_back({setting.name, StringFromFormat("%.2f", g_Config.mPostShaderSetting[key]),
													[this, key, setting](int d) {
														float &value = g_Config.mPostShaderSetting[key];
														if (!d) {
															Number(setting.name, &value, setting.minValue, setting.maxValue,
																   std::max(.001f, setting.step), setting.value);
														} else {
															value =
																std::clamp(value + d * setting.step, setting.minValue, setting.maxValue);
														}
													},
													[key, setting] { g_Config.mPostShaderSetting[key] = setting.value; }});
							 }
						 }
						 options.push_back({"Move earlier", "", [&, index](int) {
												if (index && index < g_Config.vPostShaderNames.size()) {
													std::swap(g_Config.vPostShaderNames[index], g_Config.vPostShaderNames[index - 1]);
												}
												FixPostShaderOrder(&g_Config.vPostShaderNames);
											}});
						 options.push_back({"Move later", "", [&, index](int) {
												if (index + 1 < g_Config.vPostShaderNames.size()) {
													std::swap(g_Config.vPostShaderNames[index], g_Config.vPostShaderNames[index + 1]);
												}
												FixPostShaderOrder(&g_Config.vPostShaderNames);
											}});
						 options.push_back({"Remove", "", [&, name](int) {
												const auto it =
													std::find(g_Config.vPostShaderNames.begin(), g_Config.vPostShaderNames.end(), name);
												if (it != g_Config.vPostShaderNames.end()) {
													g_Config.vPostShaderNames.erase(it);
												}
												FixPostShaderOrder(&g_Config.vPostShaderNames);
											}});
						 return options;
					 });
				 }});
		}
		rows.push_back({"Add post-processing shader", "+", [&](int) {
							std::vector<Row> choices;
							std::vector<std::string> names;
							for (const auto &shader : GetAllPostShaderInfo()) {
								if (!shader.visible || shader.isStereo ||
									std::find(g_Config.vPostShaderNames.begin(), g_Config.vPostShaderNames.end(), shader.section) !=
										g_Config.vPostShaderNames.end()) {
									continue;
								}
								names.push_back(shader.section);
								choices.push_back({std::string(GetI18NCategory(I18NCat::POSTSHADERS)->T(shader.section, shader.name)), ""});
							}
							const int selected = Rows("Post-processing shader", [&] { return choices; });
							if (selected >= 0) {
								g_Config.vPostShaderNames.push_back(names[selected]);
								FixPostShaderOrder(&g_Config.vPostShaderNames);
							}
						}});
		rows.push_back({"Texture scaling shader", g_Config.sTextureShaderName, [&](int) {
							std::vector<Row> choices{{"Off", ""}};
							std::vector<std::string> names{"Off"};
							for (const auto &shader : GetAllTextureShaderInfo()) {
								if (shader.hidden) {
									continue;
								}
								names.push_back(shader.section);
								choices.push_back({shader.name, ""});
							}
							const int selected = Rows("Texture scaling shader", [&] { return choices; });
							if (selected >= 0) {
								g_Config.sTextureShaderName = names[selected];
								g_Config.bTexHardwareScaling = selected != 0;
							}
						}});
		rows.push_back({"Clear post-processing shaders", ">", [&](int) { g_Config.vPostShaderNames.clear(); }});
		g_Config.bShaderChainRequires60FPS = PostShaderChainRequires60FPS(GetFullPostShadersChain(g_Config.vPostShaderNames));
		return rows;
	});
}

void Launcher::FrameGeneration(Entry *entry) {
	const bool owns = entry && EnsureGameInfo(*entry) && !entry->id.empty() && !g_Config.IsGameSpecific();
	if (entry && !owns && !g_Config.IsGameSpecific()) {
		return;
	}
	if (owns) {
		g_Config.Save("BeforeFrameGeneration");
		g_Config.CreateGameConfig(entry->id);
		if (!g_Config.LoadGameConfig(entry->id)) {
			return;
		}
	}
	Rows("Frame Generation", [&] {
		std::vector<Row> rows{
			{"LSFG 2x", g_Config.bSwitchFrameGeneration ? "On" : "Off",
			 [&](int) {
				 if (!g_Config.bSwitchFrameGeneration && !File::Exists(Path("/switch/ppsspp-nx/lsfg/Lossless.dll"))) {
					 Message("LSFG is not ready",
							 "Copy your compatible Lossless.dll to /switch/ppsspp-nx/lsfg/ before enabling frame generation.");
					 return;
				 }
				 g_Config.bSwitchFrameGeneration = !g_Config.bSwitchFrameGeneration;
				 if (g_Config.bSwitchFrameGeneration && g_Config.iGPUBackend != (int)GPUBackend::DEKO3D) {
					 g_Config.iGPUBackend = (int)GPUBackend::VULKAN;
				 }
			 },
			 [&] { g_Config.bSwitchFrameGeneration = false; }},
			{"Flow resolution", g_Config.fSwitchFrameGenerationFlowScale == 0.5f ? "Half resolution" : "Quarter resolution",
			 [&](int) { g_Config.fSwitchFrameGenerationFlowScale = g_Config.fSwitchFrameGenerationFlowScale == 0.5f ? 0.25f : 0.5f; },
			 [&] { g_Config.fSwitchFrameGenerationFlowScale = 0.25f; }},
			{"Performance mode", g_Config.bSwitchFrameGenerationPerformance ? "On" : "Off",
			 [&](int) { g_Config.bSwitchFrameGenerationPerformance = !g_Config.bSwitchFrameGenerationPerformance; },
			 [&] { g_Config.bSwitchFrameGenerationPerformance = true; }},
			{"Lossless.dll", File::Exists(Path("/switch/ppsspp-nx/lsfg/Lossless.dll")) ? "Installed" : "Not installed",
			 [&](int) {
				 Message("Lossless Scaling", "Vulkan: doubles supported 30 FPS output to 60 FPS.\nPlace your compatible Lossless.dll in "
											 "/switch/ppsspp-nx/lsfg/.\nThe library is supplied separately.");
			 }},
		};
		const bool installed = File::Exists(Path("/switch/ppsspp-nx/lsfg/Lossless.dll"));
		rows[0].valueColor = SdlColor(SwitchUI::StatusColor(g_Config.bSwitchFrameGeneration));
		rows.back().valueColor = SdlColor(SwitchUI::StatusColor(installed));
		return rows;
	});
	const bool enabled = g_Config.bSwitchFrameGeneration;
	SaveGameSettings("FrameGeneration");
	UpdateConfigBadge(entry);
	if (owns) {
		g_Config.UnloadGameConfig();
		if (enabled && g_Config.iGPUBackend != (int)GPUBackend::DEKO3D) {
			// The graphics backend is global even when LSFG quality is per-game.
			g_Config.iGPUBackend = (int)GPUBackend::VULKAN;
			g_Config.Save("FrameGenerationBackend");
		}
	}
}

void Launcher::Appearance() {
	Rows("Launcher",
		 [&] {
			 return std::vector<Row>{
				 {"Language", g_Config.sLanguageIni, [&](int) { ChangeLanguage(); }},
				 {"Theme", classic_ ? "Classic" : "PPSSPP", [&](int) { classic_ = !classic_; }},
				 {"Animations", animations_ ? "On" : "Off", [&](int) { animations_ = !animations_; }},
				 {"Show region flags", showRegions_ ? "On" : "Off", [&](int) { showRegions_ = !showRegions_; }},
				 {"Show game titles", titles_ ? "On" : "Off", [&](int) { titles_ = !titles_; }},
				 {"Custom settings badges", badges_ ? "On" : "Off", [&](int) { badges_ = !badges_; }},
				 {"Check updates at boot", updateAtBoot_ ? "On" : "Off", [&](int) { updateAtBoot_ = !updateAtBoot_; }},
				 {"Grid columns", StringFromInt(columns_), [&](int d) { columns_ = std::clamp(columns_ + (d ? d : 1), 3, 8); }},
				 {"Grid rows", StringFromInt(rows_), [&](int d) { rows_ = std::clamp(rows_ + (d ? d : 1), 1, 3); }},
				 {"SteamGridDB API key", g_Config.sSteamGridDBAPIKey.empty() ? "Not set" : "Configured",
				  [&](int) { Prompt("SteamGridDB API key", &g_Config.sSteamGridDBAPIKey, true); }},
			 };
		 },
		 -1, {nullptr, -1, {}, {}, "PPSSPP", {}, true});
	SaveAppearance();
}

void Launcher::LibrarySettings() {
	Rows("Library & storage", [&] {
		return std::vector<Row>{
			{"Game folders", StringFromFormat("%zu", SwitchLibrary::GetSources().size()), [&](int) { GameFolders(); }},
			{"File manager", "SD / USB / SMB", [&](int) { FileManager(Path()); }},
			{"Download missing covers", "SteamGridDB", [&](int) { BatchCovers(); }},
			{"Savedata manager", ">", [&](int) { Savedata(); }},
			{"Installed PSP games", ">", [&](int) { FileManager(GetSysDirectory(DIRECTORY_GAME)); }},
			{"Install ZIP / PKG / PBP", ">",
			 [&](int) {
				 const std::string selected = Browse(Path(), false);
				 if (!selected.empty()) {
					 LaunchFile(Path(selected));
				 }
			 }},
			{"Remote disc streaming", ">", [&](int) { RemoteDiscs(); }},
			{"Homebrew store", ">", [&](int) { Store(); }},
		};
	});
}

void Launcher::GameFolders() {
	Rows("Game folders", [&] {
		std::vector<Row> rows{{"Add game folder", ">", [&](int) {
								   const std::string path = Browse(Path(), true);
								   if (!path.empty() && !SwitchLibrary::AddSource(Path(path))) {
									   Message("Game folder", "This folder is already in the library.");
								   }
							   }}};
		const auto sources = SwitchLibrary::GetSources();
		for (size_t i = 0; i < sources.size(); ++i) {
			rows.push_back({SwitchLibrary::DescribeSource(sources[i]), ">", [&, i](int) {
								const auto sources = SwitchLibrary::GetSources();
								if (i >= sources.size()) {
									return;
								}
								const int action = Rows(
									"Game folder", [] { return std::vector<Row>{{"Change folder", ">"}, {"Remove from library", ">"}}; });
								if (action == 0) {
									const std::string path = Browse(Path(SwitchLibrary::ResolveSource(sources[i])), true);
									if (!path.empty()) {
										if (SwitchLibrary::AddSource(Path(path))) {
											SwitchLibrary::RemoveSource(i);
										} else {
											Message("Game folder", "This folder is already in the library.");
										}
									}
								} else if (action == 1 && Ask("Remove game folder?", "Files remain on storage")) {
									SwitchLibrary::RemoveSource(i);
								}
							}});
		}
		return rows;
	});
}

void Launcher::ConfigSettings(std::string_view section, Entry *entry) {
	if (entry && !EnsureGameInfo(*entry)) {
		return;
	}
	const bool ownsGameConfig = entry && !entry->id.empty() && !g_Config.IsGameSpecific();
	if (ownsGameConfig) {
		g_Config.Save("SwitchLauncher::BeforeGameSettings");
		g_Config.CreateGameConfig(entry->id);
		if (!g_Config.LoadGameConfig(entry->id)) {
			Message("Could not open game settings", "Check available storage and the permissions of the PSP configuration folder.");
			return;
		}
	}
	const std::string_view configSection = section == "Textures" || section == "Performance" || section == "Speed" ? "Graphics" : section;
	std::vector<ConfigSettingRef> settings;
	const auto all = Config::GetSettings();
	if (section == "TouchControls.Landscape") {
		for (const auto &ref : all) {
			if (ref.section == section && HasSettingUI(ref.setting)) {
				settings.push_back(ref);
			}
		}
	} else {
		for (const auto key : LauncherSettingKeys(section)) {
			for (const auto &ref : all) {
				if (ref.section == configSection && ref.setting->IniKey() == key) {
					settings.push_back(ref);
				}
			}
		}
	}
	Rows(SectionLabel(section), [&] {
		std::vector<Row> rows;
		for (const ConfigSettingRef &ref : settings) {
			const ConfigSetting *setting = ref.setting;
			const auto &info = setting->Info();
			const auto key = setting->IniKey();
			if ((key == "MultiSampleLevel" || key == "LowLatencyPresent" || key == "InflightFrames") &&
				g_Config.iGPUBackend != (int)GPUBackend::VULKAN) {
				continue;
			}
			if (g_Config.IsGameSpecific() && !setting->PerGame()) {
				continue;
			}
			void *ptr = const_cast<void *>(setting->GetVoidPtr(ref.block));
			Row row;
			row.label = SettingLabel(setting);
			if (key == "CPUSpeed") {
				row.label = Tr("Change emulated PSP CPU clock");
			}
			if (key == "proAdhocServer") {
				row.label = Tr("Ad hoc server address");
			}
			if (key == "NickName") {
				row.label = Tr("Nickname");
			}
			if (key == "PrimaryDNSServer") {
				row.label = Tr("DNS server");
			}
			if (key == "InfrastructureUsername") {
				row.label = Tr("Infrastructure username");
			}
			row.description = row.label + std::string(Tr(g_Config.IsGameSpecific() ? "\n\nApplies to this game."
																				   : "\n\nApplies to games using the global settings."));
			if (info.HasRange()) {
				row.description += StringFromFormat(Tr("\nRange: %.2f to %.2f").data(), info.minValue, info.maxValue);
			}
			if (setting->type_ == ConfigSetting::Type::TYPE_INT) {
				row.description += StringFromFormat(Tr("\nDefault: %d").data(), setting->GetDefaultInt());
			} else if (setting->type_ == ConfigSetting::Type::TYPE_FLOAT) {
				row.description += StringFromFormat(Tr("\nDefault: %.2f").data(), setting->GetDefaultFloat());
			}
			row.reset = [this, ptr, info] {
				Config::RestoreSettingToDefault(ptr);
				if (info.onChanged) {
					info.onChanged();
				}
				if (ptr == &g_Config.iGPUBackend) {
					SelectGraphicsBackend(g_Config.iGPUBackend);
				}
			};
			if (setting->type_ == ConfigSetting::Type::TYPE_BOOL) {
				row.value = Tr(*(bool *)ptr ? "On" : "Off");
				row.change = [ptr, info](int) {
					*(bool *)ptr = !*(bool *)ptr;
					if (info.onChanged) {
						info.onChanged();
					}
				};
			} else if (setting->type_ == ConfigSetting::Type::TYPE_INT && (info.HasChoices() || info.HasRange())) {
				int value = *(int *)ptr;
				auto format = [info](float v) {
					const int value = (int)std::round(v);
					if (info.HasChoices() && value >= info.choiceMin && value < info.choiceMin + info.choiceCount) {
						return std::string(PlainTranslation(g_i18nrepo.T(info.cat, info.choices[value - info.choiceMin])));
					}
					if (value == 0 && info.zeroLabel) {
						return std::string(g_i18nrepo.T(info.cat, info.zeroLabel));
					}
					if (value < 0 && info.negativeLabel) {
						return std::string(g_i18nrepo.T(info.cat, info.negativeLabel));
					}
					return StringFromInt(value) + (info.units ? " " + std::string(g_i18nrepo.T(info.cat, info.units)) : "");
				};
				row.value = format(value);
				row.change = [this, ptr, info, setting, format, label = row.label](int d) {
					int &value = *(int *)ptr;
					if (d == 0 && info.HasRange() && !info.HasChoices()) {
						float next = value;
						if (!Number(label, &next, info.negativeLabel ? -1 : info.minValue, info.maxValue, std::max(1.f, info.step),
									setting->GetDefaultInt(), format)) {
							return;
						}
						value = (int)std::round(next);
					} else if (d == 0 && info.HasChoices()) {
						std::vector<Row> choices;
						std::vector<int> values;
						int current = 0;
						for (int i = 0; i < info.choiceCount; ++i) {
							const int candidate = info.choiceMin + i;
							if (ptr == &g_Config.iGPUBackend && !g_Config.IsBackendEnabled((GPUBackend)candidate)) {
								continue;
							}
							if (candidate == value) {
								current = (int)choices.size();
							}
							values.push_back(candidate);
							choices.push_back({std::string(PlainTranslation(g_i18nrepo.T(info.cat, info.choices[i]))),
											   candidate == value ? "Selected" : ""});
						}
						if (ptr == &g_Config.iGPUBackend) {
							auto first = std::find(values.begin(), values.end(), (int)GPUBackend::DEKO3D);
							if (first != values.end()) {
								const auto offset = first - values.begin();
								std::rotate(values.begin(), first, first + 1);
								std::rotate(choices.begin(), choices.begin() + offset, choices.begin() + offset + 1);
								current = (int)(std::find(values.begin(), values.end(), value) - values.begin());
							}
						}
						const int selected = Rows(label, [&] { return choices; }, current);
						if (selected < 0) {
							return;
						}
						value = values[selected];
					} else {
						if (!d) {
							d = 1;
						}
						if (ptr == &g_Config.iGPUBackend) {
							for (int tries = 0; tries < info.choiceCount; ++tries) {
								value = (value + d + info.choiceCount) % info.choiceCount;
								if (g_Config.IsBackendEnabled((GPUBackend)value)) {
									break;
								}
							}
						} else if (info.HasChoices()) {
							value = info.choiceMin + (value - info.choiceMin + d + info.choiceCount) % info.choiceCount;
						} else {
							value = std::clamp(value + d * std::max(1, (int)info.step), info.negativeLabel ? -1 : (int)info.minValue,
											   (int)info.maxValue);
						}
					}
					if (info.onChanged) {
						info.onChanged();
					}
					if (ptr == &g_Config.iGPUBackend) {
						SelectGraphicsBackend(value);
					}
				};
			} else if (setting->type_ == ConfigSetting::Type::TYPE_FLOAT && info.HasRange()) {
				row.value = StringFromFormat("%.2f", *(float *)ptr);
				row.change = [this, ptr, info, setting, label = row.label](int d) {
					const float step = info.step > info.maxValue - info.minValue ? .01f : std::max(.01f, info.step);
					if (d == 0) {
						if (!Number(label, (float *)ptr, info.minValue, info.maxValue, step, setting->GetDefaultFloat())) {
							return;
						}
					} else {
						*(float *)ptr = std::clamp(*(float *)ptr + d * step, info.minValue, info.maxValue);
					}
					if (info.onChanged) {
						info.onChanged();
					}
				};
			} else if (setting->type_ == ConfigSetting::Type::TYPE_STRING) {
				const std::string key = Lower(std::string(setting->IniKey()));
				const bool secret = key.find("password") != std::string::npos || key.find("token") != std::string::npos ||
									key.find("apikey") != std::string::npos;
				row.value = secret ? std::string(Tr(((std::string *)ptr)->empty() ? "Not set" : "Configured")) : *(std::string *)ptr;
				row.change = [this, ptr, info, label = row.label, secret](int) {
					if (Prompt(label, (std::string *)ptr, secret) && info.onChanged) {
						info.onChanged();
					}
				};
			} else if (setting->type_ == ConfigSetting::Type::TYPE_TOUCH_POS || setting->type_ == ConfigSetting::Type::TYPE_CUSTOM_BUTTON) {
				row.value = ">";
				row.change = [this, ref](int) { StructuredSetting(ref); };
			} else if (setting->type_ == ConfigSetting::Type::TYPE_PATH) {
				row.value = ((Path *)ptr)->ToVisualString();
				row.change = [this, ptr](int) {
					const auto folder = Browse(*(Path *)ptr, true);
					if (!folder.empty()) {
						*(Path *)ptr = Path(folder);
					}
				};
			} else if (setting->type_ == ConfigSetting::Type::TYPE_INT || setting->type_ == ConfigSetting::Type::TYPE_FLOAT ||
					   setting->type_ == ConfigSetting::Type::TYPE_UINT32 || setting->type_ == ConfigSetting::Type::TYPE_UINT64 ||
					   setting->type_ == ConfigSetting::Type::TYPE_STRING_VECTOR) {
				Section value;
				setting->WriteToIniSection(ref.block, &value);
				value.Get(setting->IniKey(), &row.value);
				row.change = [this, ref, label = row.label](int) {
					Section value;
					ref.setting->WriteToIniSection(ref.block, &value);
					std::string text;
					value.Get(ref.setting->IniKey(), &text);
					if (!Prompt(label, &text)) {
						return;
					}
					bool valid = true;
					if (ref.setting->type_ == ConfigSetting::Type::TYPE_INT) {
						int v;
						valid = TryParse(text, &v);
					} else if (ref.setting->type_ == ConfigSetting::Type::TYPE_UINT32) {
						uint32_t v;
						valid = TryParse(text, &v);
					} else if (ref.setting->type_ == ConfigSetting::Type::TYPE_UINT64) {
						uint64_t v;
						valid = TryParse(text, &v);
					} else if (ref.setting->type_ == ConfigSetting::Type::TYPE_FLOAT) {
						float v;
						valid = TryParse(text, &v) && std::isfinite(v);
					}
					if (!valid) {
						Message("Invalid value", "Enter a valid number.");
						return;
					}
					value.Set(ref.setting->IniKey(), text);
					ref.setting->ReadFromIniSection(ref.block, &value, false);
					if (ref.setting->Info().onChanged) {
						ref.setting->Info().onChanged();
					}
				};
			} else {
				continue;
			}
			rows.push_back(std::move(row));
		}
		if (section == "Graphics") {
			rows.push_back({"Post-processing and texture shaders", ">", [&](int) { Shaders(); }});
			rows.push_back({"Texture scaling", ">", [&](int) { ConfigSettings("Textures", entry); }});
			rows.push_back({"Speed hacks", ">", [&](int) { ConfigSettings("Performance", entry); }});
		}
		if (section == "SystemParam" && !entry) {
			rows.push_back({"PSP firmware", ">", [&](int) { FirmwareSettings(); }});
		}
		if (section == "CPU") {
			rows.push_back({"Alternative speed", ">", [&](int) { ConfigSettings("Speed", entry); }});
		}
		return rows;
	});
	SaveGameSettings("SwitchLauncher::ConfigSettings");
	if (entry) {
		std::lock_guard<std::mutex> guard(entry->info->lock);
		entry->info->hasConfig = true;
	}
	if (ownsGameConfig) {
		g_Config.UnloadGameConfig();
	}
}

void Launcher::AchievementSettings(Entry *entry) {
	if (entry && !EnsureGameInfo(*entry)) {
		return;
	}
	if (entry) {
		g_Config.Save("SwitchLauncher::BeforeAchievements");
		g_Config.CreateGameConfig(entry->id);
		if (!g_Config.LoadGameConfig(entry->id)) {
			Message("Could not open game settings", entry->title);
			return;
		}
	}
	Achievements::UpdateSettings();
	Rows("RetroAchievements", [&] {
		std::vector<Row> rows;
		auto boolean = [&](const char *label, bool *value) {
			rows.push_back({label, *value ? "On" : "Off", [value](int) { *value = !*value; }});
		};
		rows.push_back({"Achievements enabled", g_Config.bAchievementsEnable ? "On" : "Off", [&](int) {
							g_Config.bAchievementsEnable = !g_Config.bAchievementsEnable;
							Achievements::UpdateSettings();
						}});
		std::string error;
		const bool loggedIn = Achievements::IsLoggedIn();
		std::string account(Tr(loggedIn ? "Logged in" : Achievements::IsBlockingExecution() ? "Signing in..." : "Logged out"));
		if (Achievements::LoginProblems(&error)) {
			account = Tr("Sign-in error");
		}
		rows.push_back(
			{"Account", account, [this, error, account](int) { Message("RetroAchievements account", error.empty() ? account : error); }});
		if (loggedIn) {
			const rc_client_user_t *user = rc_client_get_user_info(Achievements::GetClient());
			rows.push_back({"Username", user && user->username ? user->username : g_Config.sAchievementsUserName});
			rows.push_back({"Log out", ">", [&](int) {
								if (Ask("Log out?", "RetroAchievements")) {
									Achievements::Logout();
								}
							}});
		} else {
			rows.push_back({"Username", g_Config.sAchievementsUserName,
							[&](int) { Prompt("RetroAchievements username", &g_Config.sAchievementsUserName); }});
			rows.push_back(
				{"Log in", ">", [&](int) {
					 if (!g_Config.bAchievementsEnable) {
						 Message("Achievements are disabled", "Enable achievements before signing in.");
						 return;
					 }
					 std::string password;
					 if (g_Config.sAchievementsUserName.empty() && !Prompt("RetroAchievements username", &g_Config.sAchievementsUserName)) {
						 return;
					 }
					 if (!Prompt("RetroAchievements password", &password, true) || password.empty()) {
						 return;
					 }
					 const bool started =
						 Achievements::GetClient() && Achievements::LoginAsync(g_Config.sAchievementsUserName.c_str(), password.c_str());
					 std::fill(password.begin(), password.end(), '\0');
					 password.clear();
					 if (!started) {
						 Message("Could not start sign-in", "Check that achievements are enabled and another sign-in is not in progress.");
						 return;
					 }
					 bool cancelled = false;
					 while (Frame() && Achievements::IsBlockingExecution()) {
						 SDL_Event event{};
						 while (Poll(&event)) {
							 if (Back(event)) {
								 Achievements::Logout();
								 cancelled = true;
							 }
						 }
						 Background();
						 Panel(180, 230, WIDTH - 360, 180);
						 Center(WIDTH / 2, 285, "Signing in to RetroAchievements...", 26);
						 Footer("B  Cancel");
						 Present();
					 }
					 if (!running_ && Achievements::IsBlockingExecution()) {
						 Achievements::Logout();
					 }
					 if (!cancelled && running_) {
						 Message(Achievements::IsLoggedIn() ? "Signed in" : "Sign-in failed",
								 Achievements::IsLoggedIn() ? g_Config.sAchievementsUserName
															: "Check your username, password and network connection.");
					 }
				 }});
		}
		boolean("Hardcore Mode (no savestates)", &g_Config.bAchievementsHardcoreMode);
		boolean("Sound effects", &g_Config.bAchievementsSoundEffects);
		rows.push_back({"Achievement sound volume", StringFromInt(g_Config.iAchievementVolume) + "%", [&](int d) {
							if (!d) {
								float volume = g_Config.iAchievementVolume;
								if (Number("Achievement sound volume", &volume, VOLUME_OFF, VOLUMEHI_FULL, 1,
										   Config::GetDefaultValueInt(&g_Config.iAchievementVolume),
										   [](float value) { return StringFromInt((int)std::round(value)) + "%"; })) {
									g_Config.iAchievementVolume = (int)std::round(volume);
								}
							} else {
								g_Config.iAchievementVolume = std::clamp(g_Config.iAchievementVolume + d * 5, VOLUME_OFF, VOLUMEHI_FULL);
							}
						}});
		boolean("Encore Mode", &g_Config.bAchievementsEncoreMode);
		boolean("Unofficial achievements", &g_Config.bAchievementsUnofficial);
		rows.push_back({"Notification positions", ">", [&](int) {
							Rows("Notification positions", [&] {
								static const char *positions[] = {"None",		"Bottom Left", "Bottom Center", "Bottom Right", "Top Left",
																  "Top Center", "Top Right",   "Center Left",	"Center Right"};
								std::vector<Row> notifications;
								const std::pair<const char *, int *> values[] = {
									{"Achievement unlocked", &g_Config.iAchievementsUnlockedPos},
									{"Challenge indicator", &g_Config.iAchievementsChallengePos},
									{"Achievement progress", &g_Config.iAchievementsProgressPos},
									{"Leaderboard tracker", &g_Config.iAchievementsLeaderboardTrackerPos},
									{"Leaderboard attempt", &g_Config.iAchievementsLeaderboardStartedOrFailedPos},
									{"Leaderboard submitted", &g_Config.iAchievementsLeaderboardSubmittedPos},
								};
								for (const auto &[label, value] : values) {
									notifications.push_back({label, positions[std::clamp(*value + 1, 0, 8)], [&, value, label](int d) {
																 if (d) {
																	 *value = (*value + 1 + d + 9) % 9 - 1;
																 } else {
																	 std::vector<Row> choices;
																	 for (const char *position : positions) {
																		 choices.push_back({position, ""});
																	 }
																	 const int selected =
																		 Rows(label, [&] { return choices; }, std::clamp(*value + 1, 0, 8));
																	 if (selected >= 0) {
																		 *value = selected - 1;
																	 }
																 }
															 }});
								}
								return notifications;
							});
						}});
		rows.push_back(
			{"Website / register", ">", [&](int) { System_LaunchUrl(LaunchUrlType::BROWSER_URL, "https://retroachievements.org/"); }});
		rows.push_back({"Achievements and leaderboards", ">", [&](int) { AchievementBrowser(entry); }});
		return rows;
	});
	SaveGameSettings("SwitchLauncher::Achievements");
	if (entry) {
		{
			std::lock_guard<std::mutex> guard(entry->info->lock);
			entry->info->hasConfig = true;
		}
		g_Config.UnloadGameConfig();
		Achievements::UpdateSettings();
	}
}

void Launcher::FirmwareSettings() {
	const Path root = GetSysDirectory(DIRECTORY_NAND);
	InstalledFirmwareInfo info;
	auto scan = [&] { Busy("Reading installed firmware", [&](const auto &) { ReadInstalledFirmwareInfo(root, &info); }); };
	scan();
	Rows("PSP firmware", [&] {
		return std::vector<Row>{
			{"Installed firmware",
			 info.version.empty() ? std::string(Tr(info.anythingInstalled ? "Partial firmware" : "None")) : info.version,
			 [&](int) {
				 Message("Installed firmware",
						 StringFromFormat(
							 Tr("Version: %s\nBuild: %s\nTarget: %s\nFonts: %d\nKernel modules: %d\nFiles: %d\nSize: %s\n\n%s").data(),
							 info.version.c_str(), info.buildDate.c_str(), info.target.c_str(), info.fontCount, info.kernelModuleCount,
							 info.fileCount, NiceSizeFormat(info.totalSize).c_str(), root.ToVisualString().c_str()));
			 }},
			{"Launch XMB", info.hasVsh ? info.version : std::string(Tr("Not installed")),
			 [&](int) {
				 if (!info.hasVsh || !FirmwareVersionSupportsVSH(info.version)) {
					 Message("XMB unavailable", "Install supported firmware 6.61 to launch the PSP XMB.");
					 return;
				 }
				 result_ = {Action::Launch, (root / "flash0/vsh/module/vshmain.prx").ToString()};
			 }},
			{"Install PSP firmware update", ">",
			 [&](int) {
				 const std::string selected = Browse(Path(), false);
				 if (selected.empty() ||
					 !Ask("Install firmware?", info.anythingInstalled ? "Replace the installed firmware with the selected updater"
																	  : "Install the selected PSP updater")) {
					 return;
				 }
				 bool ok = false;
				 std::string error;
				 PSARUnpackStats stats;
				 Busy(
					 "Installing PSP firmware",
					 [&](const auto &) {
						 PSARUnpackOptions options;
						 options.model = EmulatedModelGeneration();
						 options.progress = [&](float value) { operationProgress_ = value; };
						 ok = InstallFirmware(Path(selected), root, options, &stats, &error);
					 },
					 false);
				 Message(ok ? "Firmware installed" : "Firmware installation failed",
						 ok ? stats.firmwareVersion + "\n" + StringFromFormat(Tr("%d files").data(), stats.written) : error);
				 scan();
			 }},
			{"Install or upgrade firmware from game discs", g_Config.bAutoUpgradeFirmware ? "On" : "Off",
			 [&](int) { g_Config.bAutoUpgradeFirmware = !g_Config.bAutoUpgradeFirmware; }},
			{"Back up firmware", ">",
			 [&](int) {
				 const std::string selected = Browse(Path(), true);
				 if (selected.empty()) {
					 return;
				 }
				 const Path target = Path(selected) / "PPSSPP-NAND-backup";
				 if (File::Exists(target) && !Ask("Replace firmware backup files?", target.ToVisualString())) {
					 return;
				 }
				 bool ok = false;
				 Busy("Backing up firmware", [&](const auto &cancel) { ok = CopyTree(root, target, cancel); });
				 if (!ok) {
					 Message("Backup incomplete", "Check the destination folder and free space.");
				 }
			 }},
			{"Browse firmware files", ">", [&](int) { FileManager(root); }},
			{"Uninstall firmware", ">",
			 [&](int) {
				 if (!info.anythingInstalled || !Ask("Uninstall firmware?", "Delete the installed flash0, flash1 and IPL files")) {
					 return;
				 }
				 bool ok = false;
				 std::string error;
				 Busy("Uninstalling firmware", [&](const auto &) { ok = EraseInstalledFirmware(root, &error); }, false);
				 if (!ok) {
					 Message("Could not uninstall firmware", error);
				 }
				 scan();
			 }},
		};
	});
}

void Launcher::TexturePacks(Entry &entry) {
	if (!EnsureGameInfo(entry)) {
		return;
	}
	g_Config.Save("SwitchLauncher::BeforeTextures");
	g_Config.CreateGameConfig(entry.id);
	if (!g_Config.LoadGameConfig(entry.id)) {
		Message("Could not open game settings", entry.title);
		return;
	}
	const Path directory = GetSysDirectory(DIRECTORY_TEXTURES) / entry.id;
	Rows("Texture packs", [&] {
		return std::vector<Row>{
			{"Replace textures", g_Config.bReplaceTextures ? "On" : "Off",
			 [&](int) { g_Config.bReplaceTextures = !g_Config.bReplaceTextures; }},
			{"Save new textures", g_Config.bSaveNewTextures ? "On" : "Off",
			 [&](int) { g_Config.bSaveNewTextures = !g_Config.bSaveNewTextures; }},
			{"Browse texture pack", File::Exists(directory / "textures.ini") ? "Installed" : "No textures.ini",
			 [&](int) {
				 File::CreateFullPath(directory);
				 FileManager(directory);
			 }},
			{"Import texture pack ZIP", ">",
			 [&](int) {
				 const std::string path = Browse(Path(), false);
				 if (!path.empty()) {
					 LaunchFile(Path(path));
				 }
			 }},
			{"Delete texture pack", ">",
			 [&](int) {
				 if (!Ask("Delete texture pack?", entry.title)) {
					 return;
				 }
				 bool ok = false;
				 Busy("Deleting texture pack", [&](const auto &cancel) { ok = DeleteTree(directory, cancel); });
				 if (!ok) {
					 Message("Delete incomplete", directory.ToVisualString());
				 }
			 }},
		};
	});
	g_Config.Save("SwitchLauncher::Textures");
	{
		std::lock_guard<std::mutex> guard(entry.info->lock);
		entry.info->hasConfig = true;
	}
	g_Config.UnloadGameConfig();
}

void Launcher::Compression(Entry &entry) {
	const auto container = GamePacker::DetectContainer(entry.path, nullptr);
	const auto options = GamePacker::ListOptions(container);
	if (options.empty()) {
		Message("Compression unavailable", "Select an ISO, CSO, CHD or PSN EBOOT image.");
		return;
	}
	bool keepOriginal = true;
	Rows("Compress / extract game", [&] {
		std::vector<Row> rows{{"Keep original", keepOriginal ? "Yes" : "No", [&](int) { keepOriginal = !keepOriginal; }}};
		for (const auto &option : options) {
			rows.push_back({option.title,
							">",
							[&, option](int) {
								if (!GamePacker::CanStart()) {
									Message("Another operation is active", "Wait for the current compression or installation to finish.");
									return;
								}
								if (!keepOriginal && !Ask("Replace original game?", entry.title)) {
									return;
								}
								if (!GamePacker::Start(entry.path, option, keepOriginal)) {
									Message("Could not start compression", entry.path.ToVisualString());
									return;
								}
								bool cancelled = false;
								while (Frame() && GamePacker::IsBusy()) {
									SDL_Event event{};
									while (Poll(&event)) {
										if (Back(event)) {
											GamePacker::Abort();
											cancelled = true;
										}
									}
									GamePacker::Status status{};
									GamePacker::GetStatus(&status);
									Background();
									Panel(160, 210, WIDTH - 320, 260);
									Center(WIDTH / 2, 250, status.label, 26);
									Round(220, 330, WIDTH - 440, 12, 6, {45, 55, 75, 255});
									Round(220, 330, (WIDTH - 440) * std::clamp(status.progress, 0.f, 1.f), 12, 6, palette_.accent);
									Center(WIDTH / 2, 365, StringFromFormat("%.1f%%", status.progress * 100.f), 22);
									Footer("B  Cancel");
									Present();
								}
								if (running_ && !cancelled) {
									const auto result = GamePacker::LastResult();
									Message(result.success ? "Compression complete" : "Compression failed",
											result.newPath.ToVisualString());
								}
							},
							{},
							true,
							option.description});
		}
		return rows;
	});
}

void Launcher::Settings() {
	Rows("Settings",
		 [&] {
			 return std::vector<Row>{
				 {"Launcher", ">", [&](int) { Appearance(); }},
				 {"Library & storage", ">", [&](int) { LibrarySettings(); }},
				 {"RetroAchievements", ">", [&](int) { AchievementSettings(); }},
				 {"Frame Generation", ">", [&](int) { FrameGeneration(); }},
				 {"General", ">", [&](int) { ConfigSettings("General"); }},
				 {"CPU / Emulation", ">", [&](int) { ConfigSettings("CPU"); }},
				 {"Graphics", ">", [&](int) { ConfigSettings("Graphics"); }},
				 {"Audio", ">", [&](int) { ConfigSettings("Sound"); }},
				 {"PSP system", ">", [&](int) { ConfigSettings("SystemParam"); }},
				 {"Controller / Input", ">", [&](int) { Controls(); }},
				 {"Networking", ">", [&](int) { ConfigSettings("Network"); }},
			 };
		 },
		 -1, {nullptr, 4, "General", "Emulator", "PPSSPP", "Global settings"});
	// Commit one settings session when returning to the library, not each submenu.
	Save();
}

int Launcher::FinishArtworkPicker(int result) {
	cancelArtwork_ = true;
	std::lock_guard<std::mutex> lock(decodeMutex_);
	decodeQueue_.erase(std::remove_if(decodeQueue_.begin(), decodeQueue_.end(),
									  [&](const Decode &request) {
										  if (!request.url.empty()) {
											  pendingArt_.erase(request.key);
											  return true;
										  }
										  return false;
									  }),
					   decodeQueue_.end());
	return result;
}

int Launcher::ArtworkPicker(const std::vector<SteamGridDBArtwork> &artwork, bool background, bool icons, UiText rawgameName) {
	if (artwork.empty()) {
		Message("No artwork", "SteamGridDB returned no matching images.");
		return FinishArtworkPicker(-1);
	}
	const std::string gameName((std::string_view)rawgameName);
	cancelArtwork_ = false;
	int selected = 0, top = 0;
	constexpr int columns = 5, perPage = 10, rowHeight = 52, visible = 8;
	const SDL_FRect preview = background ? SDL_FRect{40, 180, 680, 382} : SDL_FRect{56, 171, 260, 390};
	const int listX = background ? 768 : 360, listWidth = WIDTH - listX - 56;
	while (Frame()) {
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event)) {
				return FinishArtworkPicker(-1);
			}
			if (icons && Button(event, SDL_GAMEPAD_BUTTON_WEST, SDLK_Y)) {
				return FinishArtworkPicker(-2);
			}
			if (Confirm(event)) {
				return FinishArtworkPicker(selected);
			}
			const int direction = Navigate(event);
			if (direction) {
				const int delta = icons			  ? (std::abs(direction) == 2 ? (direction > 0 ? columns : -columns) : direction)
								  : direction > 0 ? 1
												  : -1;
				selected = std::clamp(selected + delta, 0, (int)artwork.size() - 1);
			}
			if (Button(event, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDLK_PAGEUP)) {
				selected = std::max(0, selected - (icons ? perPage : visible));
			}
			if (Button(event, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDLK_PAGEDOWN)) {
				selected = std::min((int)artwork.size() - 1, selected + (icons ? perPage : visible));
			}
			if (event.type == SDL_EVENT_MOUSE_WHEEL) {
				selected = std::clamp(selected - (int)event.wheel.y, 0, (int)artwork.size() - 1);
			}
			if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
				const float x = event.type == SDL_EVENT_FINGER_UP ? event.tfinger.x : event.button.x;
				const float y = event.type == SDL_EVENT_FINGER_UP ? event.tfinger.y : event.button.y;
				if (y >= HEIGHT - 40) {
					return FinishArtworkPicker(-1);
				}
				if (icons) {
					const int first = selected / perPage * perPage;
					for (int i = first; i < std::min(first + perPage, (int)artwork.size()); ++i) {
						const SDL_FRect rect{92.f + (i - first) % columns * 224, 156.f + (i - first) / columns * 224, 200, 200};
						const SDL_FPoint point{x, y};
						if (SDL_PointInRectFloat(&point, &rect)) {
							return FinishArtworkPicker(i);
						}
					}
				} else if (x >= listX && x < listX + listWidth && y >= 148 && y < 148 + visible * rowHeight) {
					const int hit = top + (int)((y - 148) / rowHeight);
					if (hit < (int)artwork.size()) {
						return FinishArtworkPicker(hit);
					}
				}
			}
		}
		top = std::clamp(top, std::max(0, selected - visible + 1), selected);
		Background();
		PageHeader(icons ? "Choose an icon" : background ? "Choose Classic background" : "Choose cover", "SteamGridDB", gameName);
		auto image = [&](int index, SDL_FRect rect) {
			const auto &item = artwork[index];
			const std::string &url = item.thumbnailUrl.empty() ? item.url : item.thumbnailUrl;
			if (auto *texture = RemoteImage(url)) {
				SDL_SetTextureAlphaMod(texture, 255);
				ImageFit(texture, rect, icons);
			} else {
				Center(rect.x + rect.w / 2, rect.y + rect.h / 2 - 16, "Loading...", 20, palette_.dim);
			}
		};
		if (icons) {
			const int first = selected / perPage * perPage;
			for (int i = first; i < std::min(first + perPage, (int)artwork.size()); ++i) {
				const SDL_FRect rect{92.f + (i - first) % columns * 224, 156.f + (i - first) / columns * 224, 200, 200};
				Panel(rect.x - 8, rect.y - 8, rect.w + 16, rect.h + 16, i == selected);
				image(i, rect);
			}
		} else {
			Panel(preview.x - 10, preview.y - 10, preview.w + 20, preview.h + 20);
			image(selected, preview);
			Panel(listX - 8, 140, listWidth + 16, std::min(visible, (int)artwork.size()) * rowHeight + 16);
			for (int i = top; i < std::min(top + visible, (int)artwork.size()); ++i) {
				const int y = 148 + (i - top) * rowHeight;
				if (i == selected) {
					Round(listX, y, listWidth, rowHeight - 4, 4, {28, 69, 92, 255});
				}
				Text(listX + 20, y + 9, StringFromFormat(Tr("Artwork %zu").data(), (size_t)i + 1), 24,
					 i == selected ? palette_.value : palette_.text);
				const std::string size = StringFromFormat("%d x %d", artwork[i].width, artwork[i].height);
				Text(listX + listWidth - 20 - TextWidth(size, 20), y + 12, size, 20, palette_.dim);
			}
		}
		Footer(icons ? "A  Use icon        Y  Image file        B  Back" : "A  Choose        B  Back");
		Present();
	}
	return FinishArtworkPicker(-1);
}

void Launcher::DownloadArtwork(Entry &entry, bool background) {
	if (g_Config.sSteamGridDBAPIKey.empty() && !Prompt("SteamGridDB API key", &g_Config.sSteamGridDBAPIKey, true)) {
		return;
	}
	std::string query = entry.title;
	std::vector<SteamGridDBGame> matches;
	int game = -1;
	for (;;) {
		SteamGridDBResult status = SteamGridDBResult::Cancelled;
		if (!Busy("Searching SteamGridDB",
				  [&](const auto &cancel) { status = SteamGridDB_Search(g_Config.sSteamGridDBAPIKey, query, &matches, &cancel); })) {
			return;
		}
		if (status != SteamGridDBResult::OK && status != SteamGridDBResult::NotFound) {
			Message("SteamGridDB", SteamGridDB_ResultMessage(status));
			return;
		}
		std::vector<std::string> labels{std::string(Tr("Custom search..."))};
		for (const auto &match : matches) {
			labels.push_back(match.name);
		}
		game = Choose("Choose game", labels);
		if (game < 0) {
			return;
		}
		if (game > 0) {
			--game;
			break;
		}
		if (!Prompt("Search SteamGridDB", &query)) {
			return;
		}
		matches.clear();
	}
	for (int pass = 0; pass < (background ? 1 : 2); ++pass) {
		const bool backdrop = background || pass == 1;
		std::vector<SteamGridDBArtwork> artwork;
		SteamGridDBResult status = SteamGridDBResult::Cancelled;
		if (!Busy("Fetching artwork", [&](const auto &cancel) {
				status = SteamGridDB_GetArtwork(g_Config.sSteamGridDBAPIKey, matches[game].id, backdrop, &artwork, &cancel);
			})) {
			return;
		}
		if (status != SteamGridDBResult::OK) {
			if (!backdrop || status != SteamGridDBResult::NotFound) {
				Message("SteamGridDB", SteamGridDB_ResultMessage(status));
			}
			return;
		}
		const int choice = ArtworkPicker(artwork, backdrop, false, matches[game].name);
		if (choice < 0) {
			return;
		}
		bool saved = false;
		if (!Busy("Downloading artwork", [&](const auto &cancel) {
				std::vector<uint8_t> data;
				status = SteamGridDB_DownloadImage(artwork[choice].url, &data, &cancel);
				if (status != SteamGridDBResult::OK || cancel) {
					return;
				}
				const Path output(std::string(ART_DIRECTORY) + "/" + entry.key + (backdrop ? ".background.png" : ".cover.png"));
				const Path temporary(output.ToString() + ".tmp");
				saved = File::WriteDataToFile(false, data.data(), data.size(), temporary) && !cancel && File::Rename(temporary, output);
				if (!saved) {
					File::Delete(temporary);
				}
			})) {
			return;
		}
		if (!saved) {
			Message("Could not save artwork",
					status == SteamGridDBResult::OK ? "Check available storage space." : SteamGridDB_ResultMessage(status));
			return;
		}
		InvalidateArtwork(entry);
	}
}

void Launcher::ImportCover(Entry &entry) {
	const std::string selected = Browse(entry.path.NavigateUp(), false, true);
	if (selected.empty()) {
		return;
	}
	bool imported = false;
	const bool completed = Busy("Importing local cover", [&](const auto &cancel) {
		std::string data;
		if (File::GetFileSize(Path(selected)) > 24 * 1024 * 1024 || !File::ReadBinaryFileToString(Path(selected), &data) || cancel) {
			return;
		}
		int width = 0, height = 0;
		uint8_t *pixels = DecodeArtwork(data, &width, &height);
		const Path output(std::string(ART_DIRECTORY) + "/" + entry.key + ".cover.png"), temporary(output.ToString() + ".tmp");
		if (pixels && !cancel && pngSave(temporary, pixels, width, height, 4)) {
			imported = !cancel && File::Rename(temporary, output);
		}
		free(pixels);
		if (!imported) {
			File::Delete(temporary);
		}
	});
	if (imported) {
		InvalidateArtwork(entry);
	} else if (completed) {
		Message("Cover import failed", "Choose a valid PNG or JPEG image up to 4096 x 4096 pixels, and check available storage space.");
	}
}

void Launcher::CoverSettings(Entry &entry) {
	int selection = 0;
	bool hasCustom = File::Exists(Path(std::string(ART_DIRECTORY) + "/" + entry.key + ".cover.png"));
	const SDL_FRect cards[] = {{360, 128, WIDTH - 416, 230}, {360, 378, WIDTH - 416, 230}};
	const char *titles[] = {"Download cover from SteamGridDB", "Choose local cover"};
	const char *kinds[] = {"Online artwork", "Local image"};
	const char *descriptions[] = {"Choose a game cover, then select optional background artwork for the Classic theme.",
								  "Choose a PNG or JPEG image from SD, USB or SMB storage. It is validated and saved as PNG."};
	repeatDirection_ = 0;
	stickNavigation_.Reset();
	while (Frame()) {
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event)) {
				return;
			}
			const int direction = Navigate(event);
			if (direction) {
				selection = direction > 0 ? 1 : 0;
			}
			bool activate = Confirm(event);
			if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
				const SDL_FPoint point{event.type == SDL_EVENT_FINGER_UP ? event.tfinger.x : event.button.x,
									   event.type == SDL_EVENT_FINGER_UP ? event.tfinger.y : event.button.y};
				if (point.y >= HEIGHT - 40) {
					return;
				}
				for (int i = 0; i < 2; ++i) {
					if (SDL_PointInRectFloat(&point, &cards[i])) {
						selection = i;
						activate = true;
					}
				}
			}
			if (hasCustom && Button(event, SDL_GAMEPAD_BUTTON_WEST, SDLK_Y) && Ask("Remove custom cover?", entry.title)) {
				if (!File::Delete(Path(std::string(ART_DIRECTORY) + "/" + entry.key + ".cover.png"))) {
					Message("Cover removal failed", entry.path.GetFilename());
				}
				InvalidateArtwork(entry);
				hasCustom = File::Exists(Path(std::string(ART_DIRECTORY) + "/" + entry.key + ".cover.png"));
			}
			if (activate) {
				if (selection == 0) {
					DownloadArtwork(entry, false);
				} else {
					ImportCover(entry);
				}
				hasCustom = File::Exists(Path(std::string(ART_DIRECTORY) + "/" + entry.key + ".cover.png"));
				repeatDirection_ = 0;
				stickNavigation_.Reset();
				break;
			}
		}
		Background();
		PageHeader("Cover settings", "Artwork", entry.title);
		Panel(46, 161, 280, 410);
		Cover(entry, {56, 171, 260, 390}, false);
		for (int i = 0; i < 2; ++i) {
			const auto &card = cards[i];
			Panel(card.x, card.y, card.w, card.h);
			if (selection == i) {
				Round(card.x + 8, card.y + 8, card.w - 16, card.h - 16, 4, {28, 69, 92, 255});
			}
			Text(card.x + 28, card.y + 24, kinds[i], 18, palette_.accent);
			Wrapped(card.x + 28, card.y + 62, card.w - 56, Tr(titles[i]), 26, 1, selection == i ? palette_.value : palette_.text);
			Wrapped(card.x + 28, card.y + 112, card.w - 56, Tr(descriptions[i]), 18, 4, palette_.dim);
		}
		Footer(hasCustom ? "A  Choose        Y  Remove custom cover        B  Back" : "A  Choose        B  Back");
		Present();
	}
}

void Launcher::GameMenu(Entry &entry) {
	bool deleted = false;
	RowPage page{&entry, 6, "General", "Manage game", "Game menu"};
	page.hints = "A  Select        B  Back";
	page.horizontalChanges = false;
	page.close = [&] { return deleted; };
	Rows(
		entry.title,
		[&] {
			std::vector<Row> rows{
				{"Launch", ">", [&](int) { LaunchFile(entry.path); }},
				{"Game settings", ">", [&](int) { PerGameSettings(entry); }},
				{"Rename game", ">",
				 [&](int) {
					 std::string title = entry.title;
					 if (Prompt("Rename game", &title) && !title.empty()) {
						 entry.title = title;
						 ini_.GetOrCreateSection("Aliases")->Set(entry.key, title);
					 }
				 }},
				{"Favorite", favorites_.count(entry.key) ? "Yes" : "No",
				 [&](int) {
					 if (!favorites_.erase(entry.key)) {
						 favorites_.insert(entry.key);
					 }
				 }},
				{"Cover settings", ">", [&](int) { CoverSettings(entry); }},
				{"Create HOME shortcut", ">", [&](int) { CreateShortcut(entry); }},
				{"Manage installed content and savedata", ">", [&](int) { GameContent(entry); }},
				{"Clear shader caches", "",
				 [&](int) {
					 if (Ask("Clear shader caches?", entry.title) && EnsureGameInfo(entry)) {
						 bool cleared = true;
						 const std::string id = entry.id;
						 Busy(
							 "Clearing shader caches",
							 [&](const auto &) {
								 std::vector<File::FileInfo> files;
								 cleared = File::GetFilesInDir(GetSysDirectory(DIRECTORY_APP_CACHE), &files);
								 for (const auto &file : files) {
									 if (!file.isDirectory && file.name.compare(0, id.size() + 1, id + ".") == 0) {
										 cleared = File::Delete(file.fullName) && cleared;
									 }
								 }
							 },
							 false);
						 if (!cleared) {
							 Message("Could not clear all shader caches", entry.title);
						 }
					 }
				 }},
				{"Clear game settings", "",
				 [&](int) {
					 if (Ask("Clear game settings?", entry.title) && EnsureGameInfo(entry)) {
						 g_Config.DeleteGameConfig(entry.id);
						 entry.customConfig = false;
						 std::lock_guard<std::mutex> guard(entry.info->lock);
						 entry.info->hasConfig = false;
					 }
				 }},
				{"Delete game", "",
				 [&](int) {
					 if (Ask("Permanently delete game?", entry.title) && EnsureGameInfo(entry, false)) {
						 bool ok = false;
						 Busy("Deleting game", [&](const auto &) {
							 entry.info->DisposeFileLoader();
							 ok = entry.info->Delete();
						 });
						 if (!ok) {
							 Message("Could not delete game", entry.path.ToVisualString());
						 }
						 deleted = ok;
						 SwitchLibrary::RequestScan();
					 }
				 }},
			};
			rows.back().labelColor = {238, 135, 135, 255};
			return rows;
		},
		0, page);
}

bool Launcher::DeleteTree(const Path &path, const std::atomic<bool> &cancel) {
	if (cancel || !path.CanNavigateUp()) {
		return false;
	}
	struct stat st {};
	if (lstat(path.c_str(), &st) != 0) {
		return false;
	}
	if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
		return File::Delete(path);
	}
	std::vector<File::FileInfo> children;
	if (!File::GetFilesInDir(path, &children)) {
		return false;
	}
	for (const auto &child : children) {
		if (!DeleteTree(child.fullName, cancel)) {
			return false;
		}
	}
	return !cancel && File::DeleteDir(path);
}

bool Launcher::CopyTree(const Path &source, const Path &target, const std::atomic<bool> &cancel) {
	if (cancel || SameOrBelow(source, target)) {
		return false;
	}
	struct stat st {};
	if (lstat(source.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) {
		return false;
	}
	struct stat destinationStat {};
	if (lstat(target.c_str(), &destinationStat) == 0) {
		if (S_ISLNK(destinationStat.st_mode) || (st.st_ino && st.st_dev == destinationStat.st_dev && st.st_ino == destinationStat.st_ino)) {
			return false;
		}
	}
	if (S_ISDIR(st.st_mode)) {
		if (!File::CreateFullPath(target)) {
			return false;
		}
		std::vector<File::FileInfo> children;
		if (!File::GetFilesInDir(source, &children)) {
			return false;
		}
		for (const auto &child : children) {
			if (!CopyTree(child.fullName, target / child.name, cancel)) {
				return false;
			}
		}
		return true;
	}
	{
		std::lock_guard<std::mutex> lock(progressMutex_);
		progress_ = source.GetFilename();
	}
	if (!S_ISREG(st.st_mode)) {
		return false;
	}
	std::string temporaryName = target.ToString() + ".ppsspp-copy-XXXXXX";
	FILE *input = File::OpenCFile(source, "rb");
	if (!input) {
		return false;
	}
	const int temporaryFd = mkstemp(temporaryName.data());
	const Path temporary(temporaryName);
	FILE *output = temporaryFd >= 0 ? fdopen(temporaryFd, "wb") : nullptr;
	if (!output) {
		if (temporaryFd >= 0) {
			close(temporaryFd);
			File::Delete(temporary);
		}
		fclose(input);
		return false;
	}
	std::vector<uint8_t> bytes(1024 * 1024);
	bool ok = true;
	while (!cancel) {
		const size_t count = fread(bytes.data(), 1, bytes.size(), input);
		if (!count) {
			ok = !ferror(input);
			break;
		}
		if (fwrite(bytes.data(), 1, count, output) != count) {
			ok = false;
			break;
		}
		transferred_.fetch_add(count, std::memory_order_relaxed);
	}
	ok = fclose(output) == 0 && ok && !cancel;
	fclose(input);
	if (ok) {
		ok = File::Rename(temporary, target);
	}
	if (!ok) {
		File::Delete(temporary);
	}
	return ok;
}

void Launcher::Paste(const Path &directory) {
	if (clipboard_.empty()) {
		return;
	}
	const Path target = directory / clipboard_.GetFilename();
	if (SameOrBelow(clipboard_, target)) {
		Message("Cannot paste here", "Choose a different destination folder.");
		return;
	}
	if (File::Exists(target) && !Ask("Replace existing files?", target.ToVisualString())) {
		return;
	}
	bool ok = false;
	Busy(clipboardCut_ ? "Moving files" : "Copying files", [&](const auto &cancel) {
		if (clipboardCut_ && !File::Exists(target) && File::MoveIfFast(clipboard_, target)) {
			ok = true;
			return;
		}
		ok = CopyTree(clipboard_, target, cancel);
		if (ok && clipboardCut_) {
			ok = DeleteTree(clipboard_, cancel);
		}
	});
	if (!ok) {
		Message("Transfer incomplete",
				"Check the destination before retrying. The storage connection may have changed or the destination may be full.");
	}
	if (ok && clipboardCut_) {
		MoveLibraryState(clipboard_, target);
		clipboard_ = Path();
	}
	SwitchLibrary::RequestScan();
}

void Launcher::FileActions(const Path &path) {
	Rows(path.GetFilename(), [&] {
		return std::vector<Row>{
			{"Add to game library",
			 ">",
			 [&](int) {
				 if (File::IsDirectory(path)) {
					 SwitchLibrary::AddSource(path);
				 }
			 },
			 {},
			 File::IsDirectory(path)},
			{"Open", ">",
			 [&](int) {
				 if (File::IsDirectory(path)) {
					 FileManager(path);
				 } else {
					 result_ = {Action::Launch, path.ToString()};
				 }
			 }},
			{"Copy", ">",
			 [&](int) {
				 clipboard_ = path;
				 clipboardCut_ = false;
				 Message("Copied to clipboard", path.ToVisualString());
			 }},
			{"Cut", ">",
			 [&](int) {
				 clipboard_ = path;
				 clipboardCut_ = true;
				 Message("Ready to move", "Browse to the destination folder, then choose Paste.");
			 }},
			{"Rename", ">",
			 [&](int) {
				 std::string name = path.GetFilename();
				 if (!Prompt("Rename", &name) || name.empty() || name == "." || name == ".." ||
					 name.find_first_of("/\\:") != std::string::npos) {
					 return;
				 }
				 Path target = path.NavigateUp() / name;
				 if (File::Exists(target) || !File::Rename(path, target)) {
					 Message("Could not rename", "A file with that name may already exist, or the storage is read-only.");
				 }
				 if (File::Exists(target) && !File::Exists(path)) {
					 MoveLibraryState(path, target);
				 }
				 SwitchLibrary::RequestScan();
			 }},
			{"File information", ">",
			 [&](int) { Message(path.GetFilename(), path.ToVisualString() + "\n\n" + NiceSizeFormat(File::GetFileSize(path))); }},
			{"Delete", ">",
			 [&](int) {
				 if (Ask("Permanently delete?", path.ToVisualString())) {
					 bool ok = false;
					 Busy("Deleting files", [&](const auto &cancel) { ok = DeleteTree(path, cancel); });
					 if (!ok) {
						 Message("Delete incomplete", "Some files could not be removed.");
					 }
					 SwitchLibrary::RequestScan();
				 }
			 }},
		};
	});
}

void Launcher::FileManager(Path directory) { Browse(directory, false, false, true); }

void Launcher::EditShare(SwitchStorage::SmbShare share) {
	if (share.name.empty()) {
		share.name = "Network share";
	}
	if (share.id.empty()) {
		share.id = StringFromFormat("share_%08x", (unsigned)SDL_GetTicks());
	}
	Rows("SMB share", [&] {
		return std::vector<Row>{
			{"Name", share.name, [&](int) { Prompt("Share name", &share.name); }},
			{"Server", share.server, [&](int) { Prompt("Server address", &share.server); }},
			{"Share", share.share, [&](int) { Prompt("SMB share name", &share.share); }},
			{"Subfolder", share.path, [&](int) { Prompt("Subfolder", &share.path); }},
			{"Username", share.user, [&](int) { Prompt("Username", &share.user); }},
			{"Password", share.password.empty() ? "Not set" : "Configured", [&](int) { Prompt("Password", &share.password, true); }},
			{"Domain", share.domain, [&](int) { Prompt("Domain", &share.domain); }},
			{"Connect at startup", share.autoMount ? "On" : "Off", [&](int) { share.autoMount = !share.autoMount; }},
			{"Save share", ">",
			 [&](int) {
				 std::string error;
				 if (!SwitchLibrary::SaveSmbShare(share, &error, false)) {
					 Message("Could not save share", error);
				 } else {
					 Message("Share saved", share.name);
				 }
			 }},
			{"Test connection", ">",
			 [&](int) {
				 std::string error;
				 bool ok = false;
				 if (SwitchLibrary::SaveSmbShare(share, &error, false)) {
					 Busy("Connecting to SMB", [&](const auto &cancel) { ok = SwitchStorage::MountSmb(share, &error, &cancel); });
				 }
				 Message(ok ? "Connected" : "Could not connect", ok ? share.name : error);
			 }},
		};
	});
}

void Launcher::BatchCovers() {
	if (g_Config.sSteamGridDBAPIKey.empty() && !Prompt("SteamGridDB API key", &g_Config.sSteamGridDBAPIKey, true)) {
		return;
	}
	if (!Ask("Download missing covers?", "Search SteamGridDB for every game without a custom cover")) {
		return;
	}
	int downloaded = 0, failed = 0;
	std::vector<std::pair<std::string, std::string>> requests;
	for (const Entry &entry : games_) {
		requests.emplace_back(entry.key, entry.title);
	}
	Busy("Downloading covers", [&](const auto &cancel) {
		for (const auto &[key, title] : requests) {
			if (cancel) {
				break;
			}
			const Path target(std::string(ART_DIRECTORY) + "/" + key + ".cover.png");
			if (File::Exists(target)) {
				continue;
			}
			{
				std::lock_guard<std::mutex> lock(progressMutex_);
				progress_ = title;
			}
			std::vector<SteamGridDBGame> matches;
			std::vector<SteamGridDBArtwork> covers;
			std::vector<uint8_t> data;
			bool ok =
				SteamGridDB_Search(g_Config.sSteamGridDBAPIKey, title, &matches, &cancel) == SteamGridDBResult::OK && !matches.empty();
			if (ok) {
				ok = SteamGridDB_GetArtwork(g_Config.sSteamGridDBAPIKey, matches[0].id, false, &covers, &cancel) == SteamGridDBResult::OK &&
					 !covers.empty();
			}
			if (ok) {
				ok = SteamGridDB_DownloadImage(covers[0].url, &data, &cancel) == SteamGridDBResult::OK;
			}
			if (ok) {
				const Path temporary(target.ToString() + ".tmp");
				ok = File::WriteDataToFile(false, data.data(), data.size(), temporary) && File::Rename(temporary, target);
			}
			if (ok) {
				++downloaded;
			} else {
				++failed;
			}
		}
	});
	for (Entry &entry : games_) {
		InvalidateArtwork(entry);
	}
	Message("Cover download finished", StringFromFormat(Tr("Downloaded: %d\nNot found or failed: %d").data(), downloaded, failed));
}

void Launcher::CaptureBinding(int key, bool replace, int index) {
	repeatDirection_ = 0;
	SDL_FlushEvents(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_EVENT_GAMEPAD_BUTTON_UP);
	KeyMap::MultiInputMapping captured;
	uint64_t padCancelAt = 0, keyboardCancelAt = 0;
	const uint64_t acceptAt = SDL_GetTicks() + 200;
	bool comboDisabled = false;
	const SDL_FRect cancel{WIDTH / 2 - 140.f, 474, 280, 50};
	auto inCancel = [&](float x, float y) { return x >= cancel.x && x < cancel.x + cancel.w && y >= cancel.y && y < cancel.y + cancel.h; };
	auto add = [&](InputMapping mapping) {
		if (!mapping.keyCode || (key == VIRTKEY_SPEED_ANALOG && !mapping.IsAxis()) || captured.mappings.contains(mapping)) {
			return;
		}
		if (!captured.empty() && !g_Config.bAllowMappingCombos) {
			comboDisabled = true;
			return;
		}
		if (captured.mappings.size() < captured.mappings.capacity()) {
			captured.mappings.push_back(mapping);
		}
	};
	auto save = [&] {
		if (captured.empty()) {
			return;
		}
		if (index >= 0) {
			KeyMap::ReplaceSingleKeyMapping(key, index, captured);
		} else {
			KeyMap::SetInputMapping(key, captured, replace);
		}
		KeyMap::UpdateNativeMenuKeys();
	};
	while (Frame()) {
		const uint64_t now = SDL_GetTicks();
		if ((padCancelAt && now - padCancelAt >= 1500) || (keyboardCancelAt && now - keyboardCancelAt >= 1500)) {
			return;
		}
		SDL_Event event{};
		while (SDL_PollEvent(&event)) {
			SDL_ConvertEventToRenderCoordinates(renderer_, &event);
			if (event.type == SDL_EVENT_QUIT) {
				running_ = false;
				return;
			}
			if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
				pads_[event.gdevice.which] = SDL_OpenGamepad(event.gdevice.which);
			} else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
				const auto it = pads_.find(event.gdevice.which);
				if (it != pads_.end()) {
					SDL_CloseGamepad(it->second);
					pads_.erase(it);
				}
				return;
			}
			if (event.type == SDL_EVENT_FINGER_UP && inCancel(event.tfinger.x, event.tfinger.y)) {
				return;
			}
			if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
				event.button.which == SDL_TOUCH_MOUSEID) {
				continue;
			}
			if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT &&
				inCancel(event.button.x, event.button.y)) {
				return;
			}
			if (SDL_GetTicks() < acceptAt) {
				continue;
			}
			InputMapping mapping;
			bool down = false, up = false;
			if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
				mapping = InputMapping(DEVICE_ID_PAD_0, SDLJoystick::getKeycodeForButton((SDL_GamepadButton)event.gbutton.button));
				down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
				up = !down;
				if (event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
					padCancelAt = down ? SDL_GetTicks() : 0;
				}
			} else if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) && !event.key.repeat) {
				const auto found = KeyMapRawSDLtoNative.find(event.key.key);
				if (found != KeyMapRawSDLtoNative.end()) {
					mapping = InputMapping(DEVICE_ID_KEYBOARD, found->second);
				}
				down = event.type == SDL_EVENT_KEY_DOWN;
				up = !down;
				if (event.key.key == SDLK_ESCAPE) {
					keyboardCancelAt = down ? SDL_GetTicks() : 0;
				}
			} else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
				int axis = event.gaxis.axis;
				if (axis == SDL_GAMEPAD_AXIS_RIGHTX) {
					axis = JOYSTICK_AXIS_Z;
				} else if (axis == SDL_GAMEPAD_AXIS_RIGHTY) {
					axis = JOYSTICK_AXIS_RZ;
				} else if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
					axis = JOYSTICK_AXIS_LTRIGGER;
				} else if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
					axis = JOYSTICK_AXIS_RTRIGGER;
				}
				if (std::abs(event.gaxis.value) > 24575) {
					mapping = InputMapping(DEVICE_ID_PAD_0, axis, event.gaxis.value > 0 ? 1 : -1);
					down = true;
				} else if (std::abs(event.gaxis.value) < 11468 && (captured.mappings.contains(InputMapping(DEVICE_ID_PAD_0, axis, 1)) ||
																   captured.mappings.contains(InputMapping(DEVICE_ID_PAD_0, axis, -1)))) {
					save();
					return;
				}
			} else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
				int button = 0;
				switch (event.button.button) {
				case SDL_BUTTON_LEFT:
					button = NKCODE_EXT_MOUSEBUTTON_1;
					break;
				case SDL_BUTTON_RIGHT:
					button = NKCODE_EXT_MOUSEBUTTON_2;
					break;
				case SDL_BUTTON_MIDDLE:
					button = NKCODE_EXT_MOUSEBUTTON_3;
					break;
				case SDL_BUTTON_X1:
					button = NKCODE_EXT_MOUSEBUTTON_4;
					break;
				case SDL_BUTTON_X2:
					button = NKCODE_EXT_MOUSEBUTTON_5;
					break;
				}
				mapping = InputMapping(DEVICE_ID_MOUSE, button);
				down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
				up = !down;
			} else if (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.y != 0) {
				const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
				add(InputMapping(DEVICE_ID_MOUSE, direction > 0 ? NKCODE_EXT_MOUSEWHEEL_UP : NKCODE_EXT_MOUSEWHEEL_DOWN));
				if (!captured.empty()) {
					save();
					return;
				}
			}
			if (down) {
				add(mapping);
			}
			if (up && captured.mappings.contains(mapping)) {
				save();
				return;
			}
		}
		Background();
		Panel(150, 170, WIDTH - 300, 380);
		Center(WIDTH / 2, 200, KeyMap::GetPspButtonName(key), 30);
		Center(WIDTH / 2, 260, key == VIRTKEY_SPEED_ANALOG ? "Move a stick / trigger" : "Press a button or move a stick / trigger", 24);
		Center(WIDTH / 2, 310, BindingName(captured), 24, palette_.value);
		Center(WIDTH / 2, 362, "Hold controls together, then release to save", 22);
		if (comboDisabled) {
			Center(WIDTH / 2, 408, "Combo mappings are not enabled", 20, palette_.accent);
		}
		Round(cancel.x, cancel.y, cancel.w, cancel.h, 8, {28, 69, 92, 255});
		Center(WIDTH / 2, cancel.y + 10, "Cancel", 24);
		Footer("Hold Minus / Escape  Cancel");
		Present();
	}
}

void Launcher::Controls(Entry *entry) {
	if (entry && !EnsureGameInfo(*entry)) {
		return;
	}
	const bool game = entry && !entry->id.empty();
	if (game) {
		g_Config.CreateGameConfig(entry->id);
		g_Config.Save("SwitchLauncher::BeforeGameSettings");
		if (!g_Config.LoadGameConfig(entry->id)) {
			Message("Could not open game settings", "Check available storage and the permissions of the PSP configuration folder.");
			return;
		}
	}
	auto bindings = [&](int key) {
		Rows(KeyMap::GetPspButtonName(key), [&] {
			std::vector<Row> rows{{"Replace binding", ">", [&](int) { CaptureBinding(key, true); }},
								  {"Add binding", ">", [&](int) { CaptureBinding(key, false); }},
								  {"Clear bindings", ">", [key](int) {
									   KeyMap::SetInputMapping(key, {}, true);
									   KeyMap::UpdateNativeMenuKeys();
								   }}};
			std::vector<KeyMap::MultiInputMapping> current;
			KeyMap::InputMappingsFromPspButton(key, &current, false);
			for (size_t i = 0; i < current.size(); ++i) {
				rows.push_back({BindingName(current[i]), ">", [&, i](int) {
									const int action = Rows("Binding", [] { return std::vector<Row>{{"Change", ">"}, {"Delete", ">"}}; });
									if (action == 0) {
										CaptureBinding(key, false, (int)i);
									} else if (action == 1) {
										KeyMap::DeleteNthMapping(key, (int)i);
										KeyMap::UpdateNativeMenuKeys();
									}
								}});
			}
			return rows;
		});
	};
	auto mappingRows = [&](const std::vector<int> &keys) {
		std::vector<Row> rows;
		for (int key : keys) {
			std::vector<KeyMap::MultiInputMapping> current;
			KeyMap::InputMappingsFromPspButton(key, &current, false);
			std::string value;
			for (const auto &mapping : current) {
				if (!value.empty()) {
					value += " / ";
				}
				value += BindingName(mapping);
			}
			rows.push_back({GetI18NCategory(I18NCat::MAPPABLECONTROLS)->T(KeyMap::GetPspButtonName(key)),
							value.empty() ? std::string(Tr("Unmapped")) : value, [&, key](int) { CaptureBinding(key, true); },
							[key] {
								KeyMap::SetInputMapping(key, {}, true);
								KeyMap::UpdateNativeMenuKeys();
							}});
		}
		return rows;
	};
	const std::vector<int> buttons{CTRL_CROSS,
								   CTRL_CIRCLE,
								   CTRL_SQUARE,
								   CTRL_TRIANGLE,
								   CTRL_LTRIGGER,
								   CTRL_RTRIGGER,
								   CTRL_UP,
								   CTRL_DOWN,
								   CTRL_LEFT,
								   CTRL_RIGHT,
								   CTRL_START,
								   CTRL_SELECT,
								   VIRTKEY_AXIS_Y_MAX,
								   VIRTKEY_AXIS_Y_MIN,
								   VIRTKEY_AXIS_X_MIN,
								   VIRTKEY_AXIS_X_MAX,
								   VIRTKEY_AXIS_RIGHT_Y_MAX,
								   VIRTKEY_AXIS_RIGHT_Y_MIN,
								   VIRTKEY_AXIS_RIGHT_X_MIN,
								   VIRTKEY_AXIS_RIGHT_X_MAX};
	RowPage page;
	page.horizontalChanges = false;
	page.hints = "A  Assign        X  Bindings        Y  Clear        B  Back";
	page.firstSection = "Controller";
	page.secondSection = "PSP controls";
	page.sectionStart = 3;
	page.input = [&](int selected, const SDL_Event &event) {
		if (selected >= 3 && Button(event, SDL_GAMEPAD_BUTTON_NORTH, SDLK_X)) {
			bindings(buttons[selected - 3]);
			return true;
		}
		return false;
	};
	Rows(
		"Controller / Input",
		[&] {
			std::vector<Row> rows{
				{"Controller profiles", ">", [&](int) { ControllerProfiles(); }},
				{"Input options", ">",
				 [&](int) {
					 Rows("Input options", [&] {
						 return std::vector<Row>{
							 {"Stick deadzone", StringFromFormat("%.0f%%", g_Config.fAnalogDeadzone * 100),
							  [&](int) { Number("Stick deadzone", &g_Config.fAnalogDeadzone, 0, .95f, .01f, .15f); }},
							 {"Stick sensitivity", StringFromFormat("%.2f", g_Config.fAnalogSensitivity),
							  [&](int) { Number("Stick sensitivity", &g_Config.fAnalogSensitivity, .1f, 3, .05f, 1); }},
							 {"Trigger threshold", StringFromFormat("%.0f%%", g_Config.fAnalogTriggerThreshold * 100),
							  [&](int) { Number("Trigger threshold", &g_Config.fAnalogTriggerThreshold, .05f, 1, .05f, .75f); }},
							 {"Right stick maps to face buttons", g_Config.bRightStickFaceButtons ? "On" : "Off",
							  [&](int) { g_Config.bRightStickFaceButtons = !g_Config.bRightStickFaceButtons; }},
							 {"Allow combo mappings", g_Config.bAllowMappingCombos ? "On" : "Off",
							  [&](int) { g_Config.bAllowMappingCombos = !g_Config.bAllowMappingCombos; }},
							 {"Strict combo input order", g_Config.bStrictComboOrder ? "On" : "Off",
							  [&](int) { g_Config.bStrictComboOrder = !g_Config.bStrictComboOrder; }},
							 {"Restore default mapping", ">",
							  [&](int) {
								  if (Ask("Restore controls?", "Replace the current controller bindings")) {
									  KeyMap::RestoreDefault();
									  KeyMap::UpdateNativeMenuKeys();
								  }
							  }},
						 };
					 });
				 }},
				{"Emulator shortcuts", ">",
				 [&](int) {
					 const std::vector<int> shortcuts{
						 VIRTKEY_PAUSE,		  VIRTKEY_FASTFORWARD, VIRTKEY_SPEED_ANALOG,   VIRTKEY_SPEED_TOGGLE,	VIRTKEY_SAVE_STATE,
						 VIRTKEY_LOAD_STATE,  VIRTKEY_NEXT_SLOT,   VIRTKEY_PREVIOUS_SLOT,  VIRTKEY_REWIND,			VIRTKEY_SCREENSHOT,
						 VIRTKEY_MUTE_TOGGLE, VIRTKEY_RAPID_FIRE,  VIRTKEY_ANALOG_LIGHTLY, VIRTKEY_AXIS_SWAP_TOGGLE};
					 RowPage shortcutPage;
					 shortcutPage.horizontalChanges = false;
					 shortcutPage.hints = page.hints;
					 shortcutPage.input = [&](int selected, const SDL_Event &event) {
						 if (Button(event, SDL_GAMEPAD_BUTTON_NORTH, SDLK_X)) {
							 bindings(shortcuts[selected]);
							 return true;
						 }
						 return false;
					 };
					 Rows("Emulator shortcuts", [&] { return mappingRows(shortcuts); }, -1, shortcutPage);
				 }},
			};
			auto mapped = mappingRows(buttons);
			rows.insert(rows.end(), mapped.begin(), mapped.end());
			return rows;
		},
		-1, page);
	SaveGameSettings("SwitchLauncher::Controls");
	const bool allowCombos = g_Config.bAllowMappingCombos, strictOrder = g_Config.bStrictComboOrder;
	if (game) {
		std::lock_guard<std::mutex> guard(entry->info->lock);
		entry->info->hasConfig = true;
	}
	if (game) {
		g_Config.UnloadGameConfig();
		// Mapping preferences are global; the bindings can be per-game.
		if (allowCombos != g_Config.bAllowMappingCombos || strictOrder != g_Config.bStrictComboOrder) {
			g_Config.bAllowMappingCombos = allowCombos;
			g_Config.bStrictComboOrder = strictOrder;
		}
	}
}

void Launcher::PerGameSettings(Entry &entry) {
	if (!EnsureGameInfo(entry)) {
		return;
	}
	Rows("Game settings",
		 [&] {
			 return std::vector<Row>{
				 {"CPU / Emulation", ">", [&](int) { ConfigSettings("CPU", &entry); }},
				 {"Graphics", ">", [&](int) { ConfigSettings("Graphics", &entry); }},
				 {"Frame Generation", ">", [&](int) { FrameGeneration(&entry); }},
				 {"Audio", ">", [&](int) { ConfigSettings("Sound", &entry); }},
				 {"PSP system", ">", [&](int) { ConfigSettings("SystemParam", &entry); }},
				 {"Controller / Input", ">", [&](int) { Controls(&entry); }},
				 {"Networking", ">", [&](int) { ConfigSettings("Network", &entry); }},
				 {"Cheats", ">", [&](int) { Cheats(entry); }},
				 {"RetroAchievements", ">", [&](int) { AchievementSettings(&entry); }},
				 {"Texture packs", ">", [&](int) { TexturePacks(entry); }},
			 };
		 },
		 -1, {nullptr, -1, "Emulator", {}, "PPSSPP", entry.title});
}

void Launcher::ControllerProfiles() {
	const Path directory("/switch/ppsspp-nx/controller-profiles");
	File::CreateFullPath(directory);
	Rows("Controller profiles", [&] {
		std::vector<Row> rows{{"Save current profile", ">", [&](int) {
								   std::string name;
								   if (!Prompt("Profile name", &name) || name.empty() || name.find_first_of("/\\:") != std::string::npos ||
									   name == "." || name == "..") {
									   return;
								   }
								   const Path target = directory / (name + ".ini");
								   if (File::Exists(target) && !Ask("Replace controller profile?", name)) {
									   return;
								   }
								   IniFile profile;
								   KeyMap::SaveToIni(profile);
								   if (!profile.Save(target)) {
									   Message("Could not save controller profile", name);
								   }
							   }}};
		std::vector<File::FileInfo> files;
		File::GetFilesInDir(directory, &files, "ini:");
		for (const auto &file : files) {
			if (file.isDirectory) {
				continue;
			}
			rows.push_back({file.name, ">", [&, file](int) {
								const int action =
									Rows(file.name, [] { return std::vector<Row>{{"Load profile", ">"}, {"Delete profile", ">"}}; });
								if (action == 0) {
									IniFile profile;
									if (profile.Load(file.fullName)) {
										KeyMap::LoadFromIni(profile);
									}
								} else if (action == 1 && Ask("Delete controller profile?", file.name)) {
									File::Delete(file.fullName);
								}
							}});
		}
		return rows;
	});
}

void Launcher::Cheats(Entry &entry) {
	if (!EnsureGameInfo(entry)) {
		return;
	}
	g_Config.Save("SwitchLauncher::BeforeCheats");
	g_Config.CreateGameConfig(entry.id);
	if (!g_Config.LoadGameConfig(entry.id)) {
		Message("Could not open game settings", entry.title);
		return;
	}
	CWCheatEngine engine(entry.id);
	engine.CreateCheatFile();
	std::vector<std::string> errors;
	auto cheats = engine.FileInfo(&errors);
	Rows("Cheats", [&] {
		std::vector<Row> rows{
			{"Enable cheats", g_Config.bEnableCheats ? "On" : "Off",
			 [&](int) {
				 g_Config.bEnableCheats = !g_Config.bEnableCheats;
				 g_Config.bReloadCheats = true;
			 }},
			{"Cheat database", ">", [&](int) { EditCheats(entry); }},
		};
		for (const CheatFileInfo &cheat : cheats) {
			std::string_view title;
			if (cheat.IsTitle(&title) || cheat.IsPostComment(&title)) {
				continue;
			}
			rows.push_back({cheat.name, cheat.enabled ? "On" : "Off", [&, cheat](int) {
								std::string data;
								if (!File::ReadBinaryFileToString(engine.CheatFilename(), &data)) {
									return;
								}
								size_t offset = 0;
								for (int line = 1; line < cheat.lineNum; ++line) {
									size_t next = data.find('\n', offset);
									if (next == std::string::npos) {
										return;
									}
									offset = next + 1;
								}
								const size_t end = data.find('\n', offset);
								const size_t definition = data.find("_C", offset);
								if (definition == std::string::npos || (end != std::string::npos && definition >= end) ||
									definition + 4 > data.size() || data.compare(definition + 4, cheat.name.size(), cheat.name) != 0) {
									return;
								}
								data[definition + 2] = cheat.enabled ? '0' : '1';
								Path temporary(engine.CheatFilename().ToString() + ".tmp");
								if (!File::WriteStringToFile(false, data, temporary) || !File::Rename(temporary, engine.CheatFilename())) {
									Message("Could not save cheat", cheat.name);
								}
								g_Config.bReloadCheats = true;
								errors.clear();
								cheats = engine.FileInfo(&errors);
							}});
		}
		if (!errors.empty()) {
			rows.push_back({"Cheat file errors", StringFromInt((int)errors.size()), [&, errors](int) {
								std::string text;
								for (const std::string &error : errors) {
									text += error + "\n";
								}
								Message("Cheat file errors", text);
							}});
		}
		return rows;
	});
	g_Config.Save("SwitchLauncher::Cheats");
	{
		std::lock_guard<std::mutex> guard(entry.info->lock);
		entry.info->hasConfig = true;
	}
	g_Config.UnloadGameConfig();
}

void Launcher::Savedata(Entry *entry) {
	Cards("Savedata manager", [&] {
		std::vector<Card> cards;
		std::vector<Row> tools{
			{"Import savedata folder", ">",
			 [&](int) {
				 std::string selected = Browse(Path(), true);
				 if (selected.empty()) {
					 return;
				 }
				 Path source(selected);
				 if (!source.CanNavigateUp() || !File::Exists(source / "PARAM.SFO")) {
					 Message("Select a savedata folder", "Choose the folder containing PARAM.SFO and the game's save files.");
					 return;
				 }
				 Path target = GetSysDirectory(DIRECTORY_SAVEDATA) / source.GetFilename();
				 if (File::Exists(target) && !Ask("Replace existing savedata?", target.GetFilename())) {
					 return;
				 }
				 bool ok = false;
				 Busy("Importing savedata", [&](const auto &cancel) { ok = CopyTree(source, target, cancel); });
				 if (!ok) {
					 Message("Import incomplete", "Check the selected folder and available storage.");
				 }
			 }},
			{"Savestates", ">", [&](int) { Savestates(entry); }},
		};
		std::vector<Path> directories;
		if (entry) {
			std::lock_guard<std::mutex> guard(entry->info->lock);
			directories = entry->info->GetSaveDataDirectories();
		} else {
			std::vector<File::FileInfo> files;
			File::GetFilesInDir(GetSysDirectory(DIRECTORY_SAVEDATA), &files);
			for (const auto &file : files) {
				if (file.isDirectory) {
					directories.push_back(file.fullName);
				}
			}
		}
		for (const Path &directory : directories) {
			Card card;
			card.icon.path = directory;
			card.icon.key = "savedata-" + ArtKey(directory);
			card.icon.info =
				g_gameInfoCache->GetInfo(nullptr, directory, GameInfoFlags::PARAM_SFO | GameInfoFlags::SIZE | GameInfoFlags::ICON);
			card.title = card.icon.info->GetTitle();
			if (card.title.empty()) {
				card.title = directory.GetFilename();
			}
			card.subtitle = directory.GetFilename();
			if (card.icon.info->Ready(GameInfoFlags::PARAM_SFO | GameInfoFlags::SIZE)) {
				std::lock_guard<std::mutex> guard(card.icon.info->lock);
				const auto &sfo = card.icon.info->GetParamSFO();
				card.subtitle = SanitizeString(sfo.GetValueString("SAVEDATA_TITLE"), StringRestriction::NoLineBreaksOrSpecials);
				card.description = SanitizeString(sfo.GetValueString("SAVEDATA_DETAIL"), StringRestriction::ConvertToUnixEndings);
				card.metadata =
					NiceSizeFormat(card.icon.info->gameSizeOnDisk) + "  |  " + card.icon.info->GetMTime() + "\n" + directory.GetFilename();
			}
			card.open = [&, directory] {
				const int action = Rows(directory.GetFilename(), [] {
					return std::vector<Row>{{"Browse files", ">"}, {"Export / back up", ">"}, {"Delete savedata", ">"}};
				});
				if (action == 0) {
					FileManager(directory);
				} else if (action == 1) {
					std::string selected = Browse(Path(), true);
					if (selected.empty()) {
						return;
					}
					Path target = Path(selected) / directory.GetFilename();
					if (File::Exists(target) && !Ask("Replace savedata backup?", target.GetFilename())) {
						return;
					}
					bool ok = false;
					Busy("Exporting savedata", [&](const auto &cancel) { ok = CopyTree(directory, target, cancel); });
					if (!ok) {
						Message("Export incomplete", "Check the destination and available storage.");
					}
				} else if (action == 2 && Ask("Permanently delete savedata?", directory.GetFilename())) {
					bool ok = false;
					Busy("Deleting savedata", [&](const auto &cancel) { ok = DeleteTree(directory, cancel); });
					if (!ok) {
						Message("Delete incomplete", directory.GetFilename());
					}
				}
			};
			cards.push_back(std::move(card));
		}
		Card management;
		management.title = Tr("Import / savestates");
		management.subtitle = Tr("Savedata manager");
		management.description = Tr("Import a PSP savedata folder or manage emulator savestates.");
		management.open = [&, tools] { Rows("Savedata manager", [&] { return tools; }); };
		cards.push_back(std::move(management));
		return cards;
	});
}

void Launcher::GameContent(Entry &entry) {
	Rows("Manage game",
		 [&] {
			 return std::vector<Row>{
				 {"Savedata / install data", ">", [&](int) { Savedata(&entry); }},
				 {"Compress / extract game", ">", [&](int) { Compression(entry); }},
				 {"Texture packs", ">", [&](int) { TexturePacks(entry); }},
				 {"Game information", ">", [&](int) { GameInformation(entry); }},
			 };
		 },
		 -1, {&entry, -1, "Manage game", {}, "PPSSPP"});
}

bool Launcher::PickShortcutIcon(Entry &entry, std::string *path) {
	std::vector<SteamGridDBArtwork> icons;
	auto localFile = [&] {
		std::string selected = Browse(entry.path.NavigateUp(), false, true);
		if (selected.empty()) {
			return false;
		}
		*path = std::move(selected);
		return true;
	};
	auto info = g_gameInfoCache->GetInfo(nullptr, entry.path, GameInfoFlags::ICON);
	const Path embedded = GetSysDirectory(DIRECTORY_APP_CACHE) / (entry.key + ".shortcut-icon.png");
	const std::string cover = std::string(ART_DIRECTORY) + "/" + entry.key + ".cover.png";
	Busy(
		"Reading game icon",
		[&](const auto &) {
			info->WaitUntilReady(GameInfoFlags::ICON);
			std::string data;
			{
				std::lock_guard<std::mutex> guard(info->lock);
				data = info->icon.data;
			}
			if (!data.empty() && File::WriteDataToFile(false, data.data(), data.size(), embedded)) {
				icons.push_back({embedded.ToString(), {}, 144, 80});
			}
			if (File::Exists(Path(cover))) {
				icons.push_back({cover, {}, 0, 0});
			}
		},
		false);
	if (!g_Config.sSteamGridDBAPIKey.empty()) {
		const std::string query = entry.title;
		if (!Busy("Fetching icons", [&](const auto &cancel) {
				std::vector<SteamGridDBGame> matches;
				std::vector<SteamGridDBArtwork> online;
				if (SteamGridDB_Search(g_Config.sSteamGridDBAPIKey, query, &matches, &cancel) == SteamGridDBResult::OK &&
					!matches.empty() &&
					SteamGridDB_GetIcons(g_Config.sSteamGridDBAPIKey, matches[0].id, &online, &cancel) == SteamGridDBResult::OK) {
					icons.insert(icons.end(), online.begin(), online.end());
				}
			})) {
			return false;
		}
	}
	if (icons.empty()) {
		return localFile();
	}
	const int selected = ArtworkPicker(icons, false, true, entry.title);
	if (selected == -2) {
		return localFile();
	}
	if (selected < 0) {
		return false;
	}
	*path = icons[selected].url;
	return true;
}

void Launcher::CreateShortcut(Entry &entry) {
	std::string name = entry.title, author = "naga", iconPath;
	appearance_->Get("ShortcutAuthor", &author);
	int selection = 0;
	const SDL_FRect iconRect{56, 236, 260, 260};
	const SDL_FRect fields[] = {{374, 231, WIDTH - 444, 82}, {374, 329, WIDTH - 444, 82}, {374, 431, WIDTH - 444, 64}};
	stickNavigation_.Reset();
	repeatDirection_ = 0;
	while (Frame()) {
		SDL_Event event{};
		while (Poll(&event)) {
			if (Back(event)) {
				return;
			}
			const int direction = Navigate(event);
			if (direction == -1) {
				selection = 0;
			} else if (direction == 1 && selection == 0) {
				selection = 1;
			} else if (direction == -2) {
				selection = selection <= 1 ? 3 : selection - 1;
			} else if (direction == 2) {
				selection = selection == 0 || selection == 3 ? 1 : selection + 1;
			}
			bool activate = Confirm(event);
			if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
				const SDL_FPoint point{event.type == SDL_EVENT_FINGER_UP ? event.tfinger.x : event.button.x,
									   event.type == SDL_EVENT_FINGER_UP ? event.tfinger.y : event.button.y};
				if (point.y >= HEIGHT - 40) {
					return;
				}
				if (SDL_PointInRectFloat(&point, &iconRect)) {
					selection = 0;
					activate = true;
				}
				for (int i = 0; i < 3; ++i) {
					if (SDL_PointInRectFloat(&point, &fields[i])) {
						selection = i + 1;
						activate = true;
					}
				}
			}
			if (!activate) {
				continue;
			}
			if (selection == 0) {
				PickShortcutIcon(entry, &iconPath);
			} else if (selection < 3) {
				std::string next = selection == 1 ? name : author;
				if (Prompt(selection == 1 ? "HOME shortcut name" : "Author", &next) && !next.empty()) {
					(selection == 1 ? name : author) = std::move(next);
				}
			} else {
				std::vector<uint8_t> icon;
				std::string error;
				bool ok = false;
				auto info = g_gameInfoCache->GetInfo(nullptr, entry.path, GameInfoFlags::ICON);
				Busy(
					"Installing HOME shortcut",
					[&](const auto &cancel) {
						const std::string source =
							iconPath.empty() ? std::string(ART_DIRECTORY) + "/" + entry.key + ".cover.png" : iconPath;
						if (startsWith(source, "https://") || startsWith(source, "http://")) {
							const auto status = SteamGridDB_DownloadImage(source, &icon, &cancel);
							if (status != SteamGridDBResult::OK) {
								error = SteamGridDB_ResultMessage(status);
								return;
							}
						} else {
							std::string data;
							if (File::GetFileSize(Path(source)) <= 24 * 1024 * 1024 && File::ReadBinaryFileToString(Path(source), &data)) {
								icon.assign(data.begin(), data.end());
							} else if (!iconPath.empty()) {
								error = "The selected icon could not be read.";
								return;
							}
						}
						if (icon.empty()) {
							info->WaitUntilReady(GameInfoFlags::ICON);
							std::lock_guard<std::mutex> guard(info->lock);
							icon.assign(info->icon.data.begin(), info->icon.data.end());
						}
						ok = SwitchForwarder_Create(entry.path.ToString(), name, author, icon, &error);
					},
					false);
				Message(ok ? "HOME shortcut installed" : "Could not install shortcut", ok ? name : error);
				if (ok) {
					appearance_->Set("ShortcutAuthor", author);
					return;
				}
			}
			stickNavigation_.Reset();
			repeatDirection_ = 0;
			break;
		}
		Background();
		PageHeader("Create HOME shortcut", "PPSSPP", entry.title);
		Panel(iconRect.x - 10, iconRect.y - 10, iconRect.w + 20, iconRect.h + 20, selection == 0);
		SDL_Texture *image = iconPath.empty() ? Artwork(entry) : RemoteImage(iconPath);
		if (image) {
			SDL_SetTextureAlphaMod(image, 255);
			ImageFit(image, iconRect, true);
		} else {
			Center(iconRect.x + iconRect.w / 2, iconRect.y + iconRect.h / 2 - 22, "PSP", 32, palette_.dim);
		}
		Center(iconRect.x + iconRect.w / 2, iconRect.y + iconRect.h + 20, "Icon", 20, selection == 0 ? palette_.value : palette_.dim);
		Panel(360, 205, WIDTH - 416, 330);
		for (int i = 0; i < 3; ++i) {
			const auto &field = fields[i];
			Round(field.x, field.y, field.w, field.h, 4, i + 1 == selection ? SDL_Color{28, 69, 92, 255} : SDL_Color{22, 30, 49, 214});
			if (i == 2) {
				Center(field.x + field.w / 2, field.y + 16, "Create shortcut", 26, selection == 3 ? palette_.value : palette_.text);
				continue;
			}
			Text(field.x + 10, field.y + 8, i == 0 ? "Name" : "Author", 18, palette_.accent);
			const std::string &value = i == 0 ? name : author;
			SDL_Rect clip{(int)field.x + 10, (int)field.y + 34, (int)field.w - 20, 44};
			SDL_SetRenderClipRect(renderer_, &clip);
			Text(field.x + 10, field.y + 36, value, 24, i + 1 == selection ? palette_.value : palette_.text);
			SDL_SetRenderClipRect(renderer_, nullptr);
		}
		Footer(selection == 3 ? "A  Create shortcut        B  Back" : "A  Edit        B  Back");
		Present();
	}
}

Result Launcher::Run() {
	File::CreateFullPath(Path(CONFIG_PATH).NavigateUp());
	if (!Init()) {
		const std::string error = StringFromFormat("SDL launcher startup failed: %s", SDL_GetError());
		ERROR_LOG(Log::System, "%s", error.c_str());
		ErrorApplicationConfig config{};
		errorApplicationCreate(&config, error.c_str(), nullptr);
		errorApplicationShow(&config);
		return {Action::Quit};
	}
	while (Frame()) {
		if (libraryGeneration_ != SwitchLibrary::Generation()) {
			LoadLibrary();
		}
		SDL_Event event{};
		while (Poll(&event, true)) {
			auto pageLibrary = [&](int direction) {
				selection_ = LibraryPage(selection_, direction, columns_, rows_, (int)visible_.size(), view_ == 0);
				headerFocus_ = false;
			};
			if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
				(event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
				if (pageDirection_) {
					pageLibrary(pageDirection_);
				}
				continue;
			}
			if (event.type == SDL_EVENT_KEY_DOWN && (event.key.key == SDLK_PAGEUP || event.key.key == SDLK_PAGEDOWN)) {
				pageLibrary(event.key.key == SDLK_PAGEUP ? -1 : 1);
				continue;
			}
			const int direction = Navigate(event);
			bool activate = Confirm(event);
			bool menu = Menu(event);
			if (Button(event, SDL_GAMEPAD_BUTTON_START, SDLK_S)) {
				Settings();
			}
			if (Button(event, SDL_GAMEPAD_BUTTON_BACK, SDLK_F)) {
				Prompt("Search library", &search_);
				FilterLibrary();
			}
			if (Button(event, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDLK_Q)) {
				view_ = (view_ + 2) % 3;
				FilterLibrary();
			}
			if (Button(event, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDLK_E)) {
				view_ = (view_ + 1) % 3;
				FilterLibrary();
			}
			if (Back(event)) {
				if (headerFocus_) {
					headerFocus_ = false;
				} else if (Ask("Quit PPSSPP?", "Exit to the HOME Menu")) {
					running_ = false;
				}
			}
			if (headerFocus_) {
				if (std::abs(direction) == 1) {
					headerSelection_ = (headerSelection_ + direction + 4) % 4;
				}
				if (direction == 2) {
					headerFocus_ = false;
				}
			} else if (direction && !visible_.empty()) {
				if (direction == -2 && (view_ == 0 || selection_ % (columns_ * rows_) < columns_)) {
					headerFocus_ = true;
					headerSelection_ = view_;
				} else {
					selection_ = view_ == 0 ? std::clamp(selection_ + (direction > 0 ? 1 : -1), 0, (int)visible_.size() - 1)
											: LibraryMove(selection_, direction, columns_, rows_, (int)visible_.size());
					Sound(0);
				}
			}
			if (event.type == SDL_EVENT_FINGER_DOWN) {
				touchX_ = event.tfinger.x;
				touchY_ = event.tfinger.y;
				touchAt_ = SDL_GetTicks();
				touchDelta_ = 0;
			}
			if (event.type == SDL_EVENT_FINGER_MOTION) {
				touchDelta_ = event.tfinger.x - touchX_;
			}
			if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
				const float x = event.type == SDL_EVENT_FINGER_UP ? event.tfinger.x : event.button.x;
				const float y = event.type == SDL_EVENT_FINGER_UP ? event.tfinger.y : event.button.y;
				if (std::abs(touchDelta_) > 80 && event.type == SDL_EVENT_FINGER_UP && !visible_.empty()) {
					pageLibrary(touchDelta_ > 0 ? -1 : 1);
				} else if (y < 90) {
					if (x > WIDTH - 100) {
						Settings();
					} else {
						SDL_FPoint point{x, y};
						for (int tab = 0; tab < 3; ++tab) {
							if (SDL_PointInRectFloat(&point, &tabRects_[tab])) {
								view_ = tab;
								FilterLibrary();
								break;
							}
						}
					}
				} else {
					for (int i = 0; i < (int)visible_.size(); ++i) {
						if (view_ != 0 && i / (columns_ * rows_) != selection_ / (columns_ * rows_)) {
							continue;
						}
						SDL_FPoint point{x, y};
						SDL_FRect rect = CoverRect(i);
						if (!SDL_PointInRectFloat(&point, &rect)) {
							continue;
						}
						selection_ = i;
						headerFocus_ = false;
						menu = event.type == SDL_EVENT_FINGER_UP && SDL_GetTicks() - touchAt_ > 650;
						activate = tapSelection_ == i && SDL_GetTicks() - tapAt_ < 400;
						tapSelection_ = i;
						tapAt_ = SDL_GetTicks();
						break;
					}
				}
			}
			if (activate && headerFocus_) {
				if (headerSelection_ == 3) {
					Settings();
				} else {
					view_ = headerSelection_;
					FilterLibrary();
				}
			} else if (activate && visible_.empty()) {
				if (view_ == 1) {
					Settings();
				} else {
					view_ = 1;
					FilterLibrary();
				}
			} else if (!visible_.empty()) {
				Entry &entry = games_[visible_[selection_]];
				if (menu) {
					GameMenu(entry);
					FilterLibrary();
				} else if (activate) {
					LaunchFile(entry.path);
				}
			}
		}
		RenderLibrary();
	}
	if (result_.action == Action::Launch) {
		const std::string key = ArtKey(Path(result_.path));
		recentKeys_.erase(std::remove(recentKeys_.begin(), recentKeys_.end(), key), recentKeys_.end());
		recentKeys_.insert(recentKeys_.begin(), key);
		if (recentKeys_.size() > 30) {
			recentKeys_.resize(30);
		}
	}
	Save();
	if (result_.action == Action::Unavailable) {
		result_.action = Action::Quit;
	}
	return result_;
}

} // namespace

Result Run() {
	Result result = Launcher().Run();
	return result;
}

void RecordLaunch(const Path &path) {
	IniFile config;
	LoadLauncherConfig(config);
	Section *section = config.GetOrCreateSection("Recent");
	const std::string key = ArtKey(path);
	std::vector<std::string> recent{key};
	for (int i = 0; i < 30; ++i) {
		std::string previous;
		section->Get(StringFromInt(i), &previous);
		if (!previous.empty() && previous != key) {
			recent.push_back(std::move(previous));
		}
	}
	section->Clear();
	for (size_t i = 0; i < std::min(recent.size(), (size_t)30); ++i) {
		section->Set(StringFromInt((int)i), recent[i]);
	}
	if (!WriteLauncherConfig(config)) {
		ERROR_LOG(Log::System, "Could not save launch history");
	}
}

} // namespace SwitchLauncher
