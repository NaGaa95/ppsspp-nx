// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(SWITCH)

#include "UI/SwitchForwarderScreen.h"
#include "Common/File/DirListing.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "Common/File/FileUtil.h"
#include "Common/Net/HTTPRequest.h"
#include "Common/Render/ManagedTexture.h"
#include "Common/StringUtils.h"
#include "Common/System/OSD.h"
#include "Common/System/Request.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/UI/Context.h"
#include "Common/UI/ScreenManager.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Core/Config.h"
#include "UI/GameInfoCache.h"
#include "UI/SwitchForwarderInstaller.h"
#include "UI/SwitchLibraryScreen.h"
#include "UI/SwitchSteamGridDB.h"

namespace {

constexpr size_t MAX_ICON_SIZE = 24 * 1024 * 1024;

enum class TaskState {
	Idle,
	Searching,
	GamesReady,
	LoadingArtwork,
	ArtworkReady,
	Downloading,
	ImageReady,
	Installing,
	Installed,
	Error,
};

class ForwarderIconView : public UI::InertView {
public:
	ForwarderIconView(const std::vector<uint8_t> *data, UI::LayoutParams *layoutParams) : UI::InertView(layoutParams), data_(data) {}
	~ForwarderIconView() override { ReleaseTexture(); }

	void DeviceLost() override {
		ReleaseTexture();
		attempted_ = false;
	}

	void Draw(UIContext &dc) override {
		if (!attempted_ && data_ && !data_->empty()) {
			attempted_ = true;
			texture_ = CreateTextureFromFileData(dc.GetDrawContext(), data_->data(), data_->size(), ImageFileType::DETECT, false,
												 "forwarder_icon", 4096, 4096);
		}
		dc.Draw()->DrawImageCenterTexel(dc.GetTheme().whiteImage, bounds_.x, bounds_.y, bounds_.x2(), bounds_.y2(), 0xFF101010);
		if (!texture_)
			return;
		dc.Flush();
		dc.GetDrawContext()->BindTexture(0, texture_);
		float u1 = 0.0f;
		float v1 = 0.0f;
		float u2 = 1.0f;
		float v2 = 1.0f;
		if (texture_->Width() > texture_->Height()) {
			const float margin = (1.0f - (float)texture_->Height() / texture_->Width()) * 0.5f;
			u1 = margin;
			u2 = 1.0f - margin;
		} else if (texture_->Height() > texture_->Width()) {
			const float margin = (1.0f - (float)texture_->Width() / texture_->Height()) * 0.5f;
			v1 = margin;
			v2 = 1.0f - margin;
		}
		dc.Draw()->DrawTexRect(bounds_, u1, v1, u2, v2, 0xFFFFFFFF);
		dc.Flush();
		dc.RebindTexture();
	}

	std::string DescribeText() const override { return "Forwarder icon"; }

private:
	void ReleaseTexture() {
		if (texture_) {
			texture_->Release();
			texture_ = nullptr;
		}
	}

	const std::vector<uint8_t> *data_ = nullptr;
	Draw::Texture *texture_ = nullptr;
	bool attempted_ = false;
};

class RemoteArtworkView : public UI::InertView {
public:
	RemoteArtworkView(std::string url, UI::LayoutParams *layoutParams) : UI::InertView(layoutParams), url_(std::move(url)) {}
	~RemoteArtworkView() override {
		if (download_)
			download_->Cancel();
		ReleaseTexture();
	}

	void DeviceLost() override { ReleaseTexture(); }

	void Draw(UIContext &dc) override {
		if (!download_ && encoded_.empty() && !failed_) {
			download_ = g_DownloadManager.StartDownload(url_, Path(), http::RequestFlags::Default, "image/png, image/jpeg",
														"SteamGridDB artwork", [this](http::Request &download) {
															if (download.IsCancelled())
																return;
															if (download.ResultCode() == 200 && download.buffer().size() <= MAX_ICON_SIZE) {
																std::string data;
																download.buffer().TakeAll(&data);
																encoded_.assign(data.begin(), data.end());
															} else {
																failed_ = true;
															}
														});
		}
		if (!texture_ && !encoded_.empty()) {
			texture_ = CreateTextureFromFileData(dc.GetDrawContext(), encoded_.data(), encoded_.size(), ImageFileType::DETECT, false,
												 "steamgriddb_artwork", 4096, 4096);
			if (!texture_)
				failed_ = true;
		}

		dc.Draw()->DrawImageCenterTexel(dc.GetTheme().whiteImage, bounds_.x, bounds_.y, bounds_.x2(), bounds_.y2(), 0xFF101010);
		if (!texture_) {
			if (failed_)
				dc.DrawText(dc.GetTheme().uiFont, "Preview unavailable", bounds_.centerX(), bounds_.centerY(), dc.GetTheme().infoStyle.fgColor, ALIGN_CENTER);
			return;
		}
		float x = bounds_.x;
		float y = bounds_.y;
		float width = bounds_.w;
		float height = bounds_.h;
		if ((float)texture_->Width() / texture_->Height() < width / height) {
			const float scaledWidth = height * texture_->Width() / texture_->Height();
			x += (width - scaledWidth) * 0.5f;
			width = scaledWidth;
		} else {
			const float scaledHeight = width * texture_->Height() / texture_->Width();
			y += (height - scaledHeight) * 0.5f;
			height = scaledHeight;
		}
		dc.Flush();
		dc.GetDrawContext()->BindTexture(0, texture_);
		dc.Draw()->DrawTexRect(x, y, x + width, y + height, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF);
		dc.Flush();
		dc.RebindTexture();
	}

	std::string DescribeText() const override { return "SteamGridDB artwork preview"; }

private:
	void ReleaseTexture() {
		if (texture_) {
			texture_->Release();
			texture_ = nullptr;
		}
	}

	std::string url_;
	std::shared_ptr<http::Request> download_;
	std::vector<uint8_t> encoded_;
	Draw::Texture *texture_ = nullptr;
	bool failed_ = false;
};

class SteamGridArtworkScreen : public UI::PopupScreen {
public:
	SteamGridArtworkScreen(std::vector<SteamGridDBArtwork> artwork, std::function<void(SteamGridDBArtwork)> selected)
		: UI::PopupScreen("Choose artwork"), artwork_(std::move(artwork)), selected_(std::move(selected)) {}

	const char *tag() const override { return "SteamGridArtwork"; }

protected:
	void CreatePopupContents(UI::ViewGroup *parent) override {
		using namespace UI;
		const SteamGridDBArtwork &current = artwork_[index_];
		parent->Add(new RemoteArtworkView(current.thumbnailUrl, new LinearLayoutParams(320.0f, 320.0f, Gravity::G_HCENTER, Margins(8))));
		std::string details = "Artwork " + std::to_string(index_ + 1) + " of " + std::to_string(artwork_.size());
		if (current.width > 0 && current.height > 0)
			details += " (" + std::to_string(current.width) + "x" + std::to_string(current.height) + ")";
		parent->Add(new TextView(details, ALIGN_CENTER, false, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(8))));

		LinearLayout *actions =
			parent->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(4))));
		Choice *previous = actions->Add(new Choice("Previous", ImageID("I_ARROW_LEFT"), new LinearLayoutParams(1.0f, Margins(4))));
		previous->SetEnabled(index_ > 0);
		previous->OnClick.Add([this](EventParams &) {
			--index_;
			RecreateViews();
		});
		Choice *use = actions->Add(new Choice("Use artwork", ImageID("I_CHECKMARK"), new LinearLayoutParams(1.0f, Margins(4))));
		use->OnClick.Add([this](EventParams &) {
			SteamGridDBArtwork artwork = artwork_[index_];
			TriggerFinish(DR_OK);
			selected_(std::move(artwork));
		});
		Choice *next = actions->Add(new Choice("Next", ImageID("I_ARROW_RIGHT"), new LinearLayoutParams(1.0f, Margins(4))));
		next->SetEnabled(index_ + 1 < artwork_.size());
		next->OnClick.Add([this](EventParams &) {
			++index_;
			RecreateViews();
		});
		parent->Add(new Choice("Close", ImageID("I_EXIT")))->OnClick.Add([this](EventParams &) { TriggerFinish(DR_CANCEL); });
	}

	bool ShowButtons() const override { return false; }
	UI::Size PopupWidth() const override { return 620; }

private:
	std::vector<SteamGridDBArtwork> artwork_;
	std::function<void(SteamGridDBArtwork)> selected_;
	size_t index_ = 0;
};

}  // namespace

struct SwitchForwarderScreen::Impl {
	~Impl() {
		cancel.store(true, std::memory_order_relaxed);
		if (worker.joinable())
			worker.join();
	}

	TaskState GetState() {
		std::scoped_lock guard(mutex);
		return state;
	}

	void PrepareTask(TaskState nextState) {
		cancel.store(true, std::memory_order_relaxed);
		if (worker.joinable())
			worker.join();
		cancel.store(false, std::memory_order_relaxed);
		std::scoped_lock guard(mutex);
		state = nextState;
		error.clear();
	}

	void SetError(SteamGridDBResult result) {
		std::scoped_lock guard(mutex);
		error = SteamGridDB_ResultMessage(result);
		state = result == SteamGridDBResult::Cancelled ? TaskState::Idle : TaskState::Error;
	}

	void StartSearch(std::string apiKey, std::string title) {
		PrepareTask(TaskState::Searching);
		worker = std::thread([this, apiKey = std::move(apiKey), title = std::move(title)] {
			SetCurrentThreadAffinity(ThreadAffinityRole::IO);
			SetCurrentThreadName("SteamGridDBSearch");
			std::vector<SteamGridDBGame> found;
			SteamGridDBResult result = SteamGridDB_Search(apiKey, title, &found, &cancel);
			if (result != SteamGridDBResult::OK) {
				SetError(result);
				return;
			}
			std::scoped_lock guard(mutex);
			games = std::move(found);
			state = TaskState::GamesReady;
		});
	}

	void StartArtwork(std::string apiKey, int64_t gameId) {
		PrepareTask(TaskState::LoadingArtwork);
		worker = std::thread([this, apiKey = std::move(apiKey), gameId] {
			SetCurrentThreadAffinity(ThreadAffinityRole::IO);
			SetCurrentThreadName("SteamGridDBArtwork");
			std::vector<SteamGridDBArtwork> found;
			SteamGridDBResult result = SteamGridDB_GetIcons(apiKey, gameId, &found, &cancel);
			if (result != SteamGridDBResult::OK) {
				SetError(result);
				return;
			}
			std::scoped_lock guard(mutex);
			artworks = std::move(found);
			state = TaskState::ArtworkReady;
		});
	}

	void StartDownload(std::string url) {
		PrepareTask(TaskState::Downloading);
		worker = std::thread([this, url = std::move(url)] {
			SetCurrentThreadAffinity(ThreadAffinityRole::IO);
			SetCurrentThreadName("SteamGridDBDownload");
			std::vector<uint8_t> downloaded;
			SteamGridDBResult result = SteamGridDB_DownloadImage(url, &downloaded, &cancel);
			if (result != SteamGridDBResult::OK) {
				SetError(result);
				return;
			}
			std::scoped_lock guard(mutex);
			image = std::move(downloaded);
			state = TaskState::ImageReady;
		});
	}

	void StartInstall(std::string path, std::string name, std::string author, std::vector<uint8_t> icon) {
		PrepareTask(TaskState::Installing);
		worker = std::thread([this, path = std::move(path), name = std::move(name), author = std::move(author), icon = std::move(icon)] {
			SetCurrentThreadAffinity(ThreadAffinityRole::IO);
			SetCurrentThreadName("ForwarderInstall");
			std::string failure;
			bool success = SwitchForwarder_Create(path, name, author, icon, &failure);
			std::scoped_lock guard(mutex);
			if (success) {
				state = TaskState::Installed;
			} else {
				error = std::move(failure);
				state = TaskState::Error;
			}
		});
	}

	std::vector<SteamGridDBGame> TakeGames() {
		std::scoped_lock guard(mutex);
		state = TaskState::Idle;
		return std::move(games);
	}

	std::vector<SteamGridDBArtwork> TakeArtwork() {
		std::scoped_lock guard(mutex);
		state = TaskState::Idle;
		return std::move(artworks);
	}

	std::vector<uint8_t> TakeImage() {
		std::scoped_lock guard(mutex);
		state = TaskState::Idle;
		return std::move(image);
	}

	std::string TakeError() {
		std::scoped_lock guard(mutex);
		state = TaskState::Idle;
		return std::move(error);
	}

	void AcknowledgeInstalled() {
		std::scoped_lock guard(mutex);
		state = TaskState::Idle;
	}

	std::mutex mutex;
	std::thread worker;
	std::atomic<bool> cancel{false};
	TaskState state = TaskState::Idle;
	std::vector<SteamGridDBGame> games;
	std::vector<SteamGridDBArtwork> artworks;
	std::vector<uint8_t> image;
	std::string error;
};

SwitchForwarderScreen::SwitchForwarderScreen(const Path &gamePath, std::string title)
	: UI::PopupScreen("Create forwarder"), impl_(new Impl()), gamePath_(gamePath), name_(std::move(title)), searchQuery_(name_) {
	gameInfo_ = g_gameInfoCache->GetInfo(nullptr, gamePath_, GameInfoFlags::ICON);
}

SwitchForwarderScreen::~SwitchForwarderScreen() = default;

bool SwitchForwarderScreen::CanComplete(DialogResult) {
	return impl_->GetState() != TaskState::Installing;
}

void SwitchForwarderScreen::CreatePopupContents(UI::ViewGroup *parent) {
	using namespace UI;
	const TaskState state = impl_->GetState();
	const bool busy = state != TaskState::Idle && state != TaskState::Installed && state != TaskState::Error;

	LinearLayout *details =
		parent->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(8, 4))));
	details->SetSpacing(12.0f);
	details->Add(new ForwarderIconView(&iconData_, new LinearLayoutParams(180.0f, 180.0f)));
	LinearLayout *fields = details->Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(1.0f)));
	fields->Add(new PopupTextInputChoice(GetRequesterToken(), &name_, "Name", "", 255, screenManager()));
	fields->Add(new PopupTextInputChoice(GetRequesterToken(), &author_, "Author", "", 127, screenManager()));
	fields->Add(
		new TextView(iconSource_, ALIGN_LEFT | FLAG_WRAP_TEXT, false, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(12, 6))));

	LinearLayout *iconButtons =
		parent->Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(4))));
	Choice *gameIcon = iconButtons->Add(new Choice("Game icon", ImageID("I_APP"), new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(4))));
	gameIcon->SetEnabled(!busy && !gameIcon_.empty());
	gameIcon->OnClick.Add([this](EventParams &) { UseGameIcon(); });
	Choice *localImage = iconButtons->Add(new Choice("Local image", ImageID("I_FOLDER_OPEN"), new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(4))));
	localImage->SetEnabled(!busy);
	localImage->OnClick.Add([this](EventParams &) { ChooseLocalImage(); });
	Choice *steamGridDB = iconButtons->Add(new Choice("SteamGridDB", ImageID("I_DOWNLOAD"), new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(4))));
	steamGridDB->SetEnabled(!busy);
	steamGridDB->OnClick.Add([this](EventParams &) { StartSteamGridDB(); });

	std::string status;
	switch (state) {
	case TaskState::Searching:
		status = "Searching SteamGridDB...";
		break;
	case TaskState::LoadingArtwork:
		status = "Loading SteamGridDB artwork...";
		break;
	case TaskState::Downloading:
		status = "Downloading artwork...";
		break;
	case TaskState::Installing:
		status = "Creating HOME Menu forwarder...";
		break;
	default:
		break;
	}
	if (!status.empty())
		parent->Add(
			new TextView(status, ALIGN_CENTER | FLAG_WRAP_TEXT, false, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(8))));

	LinearLayout *actions = parent->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, Margins(4))));
	Choice *create = actions->Add(new Choice("Create forwarder", ImageID("I_SHORTCUT"), new LinearLayoutParams(1.0f, Margins(4))));
	create->SetEnabled(!busy && !name_.empty() && !iconData_.empty());
	create->OnClick.Add([this](EventParams &) { CreateForwarder(); });
	Choice *close = actions->Add(new Choice("Close", ImageID("I_EXIT"), new LinearLayoutParams(1.0f, Margins(4))));
	close->SetEnabled(state != TaskState::Installing);
	close->OnClick.Add([this](EventParams &) { TriggerFinish(DR_CANCEL); });
}

void SwitchForwarderScreen::StartSteamGridDB() {
	searchQuery_ = name_;
	const std::string apiKey(StripSpaces(g_Config.sSteamGridDBAPIKey));
	if (apiKey != g_Config.sSteamGridDBAPIKey) {
		g_Config.sSteamGridDBAPIKey = apiKey;
		g_Config.Save("SteamGridDBAPIKey");
	}
	if (g_Config.sSteamGridDBAPIKey.empty()) {
		System_InputBoxGetString(GetRequesterToken(), "Steam Grid DB API Key", "", true, [this](std::string_view value, int) {
			value = StripSpaces(value);
			if (value.empty())
				return;
			g_Config.sSteamGridDBAPIKey = value;
			g_Config.Save("SteamGridDBAPIKey");
			impl_->StartSearch(g_Config.sSteamGridDBAPIKey, searchQuery_);
			RecreateViews();
		});
		return;
	}
	impl_->StartSearch(g_Config.sSteamGridDBAPIKey, searchQuery_);
	RecreateViews();
}

void SwitchForwarderScreen::ChooseLocalImage() {
	Path start = gamePath_.CanNavigateUp() ? gamePath_.NavigateUp() : Path();
	screenManager()->push(CreateSwitchImagePickerScreen(start, [this](const Path &path) {
		if (File::GetFileSize(path) > MAX_ICON_SIZE) {
			ShowError("The selected image is too large.");
			return;
		}
		std::string contents;
		if (!File::ReadBinaryFileToString(path, &contents)) {
			ShowError("The selected image could not be read.");
			return;
		}
		std::vector<uint8_t> data(contents.begin(), contents.end());
		ApplyIcon(std::move(data), path.GetFilename());
	}));
}

void SwitchForwarderScreen::UseGameIcon() {
	if (!gameIcon_.empty())
		ApplyIcon(gameIcon_, "Game icon");
}

bool SwitchForwarderScreen::ApplyIcon(std::vector<uint8_t> data, std::string source) {
	if (data.empty() || data.size() > MAX_ICON_SIZE) {
		ShowError("The selected image is invalid.");
		return false;
	}
	TempImage image;
	if (!image.LoadTextureLevelsFromFileData(data.data(), data.size(), ImageFileType::DETECT, 4096, 4096)) {
		ShowError("The selected image is not a valid PNG or JPEG file.");
		return false;
	}
	image.Free();
	iconData_ = std::move(data);
	iconSource_ = std::move(source);
	RecreateViews();
	return true;
}

void SwitchForwarderScreen::CreateForwarder() {
	if (name_.empty() || iconData_.empty())
		return;
	impl_->StartInstall(gamePath_.ToString(), name_, author_, iconData_);
	RecreateViews();
}

void SwitchForwarderScreen::ShowError(std::string message) {
	screenManager()->push(new UI::MessagePopupScreen("Create forwarder", message, "OK", ""));
}

void SwitchForwarderScreen::update() {
	UI::PopupScreen::update();
	if (!checkedGameIcon_ && gameInfo_->Ready(GameInfoFlags::ICON)) {
		checkedGameIcon_ = true;
		{
			std::scoped_lock guard(gameInfo_->lock);
			gameIcon_.assign(gameInfo_->icon.data.begin(), gameInfo_->icon.data.end());
		}
		if (!gameIcon_.empty()) {
			ApplyIcon(gameIcon_, "Game icon");
		} else {
			iconSource_ = "Choose a local image or download one from SteamGridDB.";
			RecreateViews();
		}
	}

	switch (impl_->GetState()) {
	case TaskState::GamesReady: {
		std::vector<SteamGridDBGame> games = impl_->TakeGames();
		std::vector<std::string> labels{"Custom search..."};
		labels.reserve(games.size() + 1);
		for (const auto &game : games)
			labels.push_back(game.name);
		screenManager()->push(
			new UI::ListPopupScreen("Choose SteamGridDB game", labels, -1, [this, games = std::move(games)](int selected) {
				if (selected == 0) {
					System_InputBoxGetString(GetRequesterToken(), "Custom SteamGridDB search", searchQuery_, false,
											 [this](std::string_view value, int) {
												 if (value.empty())
													 return;
												 searchQuery_ = value;
												 impl_->StartSearch(g_Config.sSteamGridDBAPIKey, searchQuery_);
												 RecreateViews();
											 });
				} else if (selected > 0 && selected <= (int)games.size()) {
					impl_->StartArtwork(g_Config.sSteamGridDBAPIKey, games[selected - 1].id);
					RecreateViews();
				}
			}));
		RecreateViews();
		break;
	}
	case TaskState::ArtworkReady: {
		std::vector<SteamGridDBArtwork> artworks = impl_->TakeArtwork();
		if (artworks.empty()) {
			ShowError(SteamGridDB_ResultMessage(SteamGridDBResult::NotFound));
			break;
		}
		screenManager()->push(new SteamGridArtworkScreen(std::move(artworks), [this](SteamGridDBArtwork artwork) {
			impl_->StartDownload(std::move(artwork.url));
			RecreateViews();
		}));
		RecreateViews();
		break;
	}
	case TaskState::ImageReady:
		ApplyIcon(impl_->TakeImage(), "SteamGridDB artwork");
		break;
	case TaskState::Installed:
		impl_->AcknowledgeInstalled();
		g_OSD.Show(OSDType::MESSAGE_SUCCESS, "Forwarder created on the HOME Menu");
		RecreateViews();
		break;
	case TaskState::Error:
		ShowError(impl_->TakeError());
		RecreateViews();
		break;
	default:
		break;
	}
}

#endif

#if defined(PPSSPP_SWITCH_SDL_LAUNCHER)
// Shared with the stock in-game shortcut dialog, without linking the original
// launcher's library, network-share editor or file-management screens.
namespace {
class ShortcutImagePicker : public UISimpleBaseDialogScreen {
public:
	ShortcutImagePicker(const Path &start, std::function<void(const Path &)> selected)
		: UISimpleBaseDialogScreen(Path(), SimpleDialogFlags::ContentsCanScroll), path_(start), selected_(std::move(selected)) {}
	std::string_view GetTitle() const override { return "Choose image"; }
	const char *tag() const override { return "ShortcutImagePicker"; }
	void CreateDialogViews(UI::ViewGroup *parent) override {
		using namespace UI;
		if (path_.CanNavigateUp()) {
			parent->Add(new Choice(".."))->OnClick.Add([this](EventParams &) {
				path_ = path_.NavigateUp();
				RecreateViews();
			});
		}
		std::vector<File::FileInfo> files;
		File::GetFilesInDir(path_, &files, "png:jpg:jpeg:");
		for (const auto &file : files) {
			parent->Add(new Choice(file.name))->OnClick.Add([this, file](EventParams &) {
				if (file.isDirectory) {
					path_ = file.fullName;
					RecreateViews();
				} else {
					selected_(file.fullName);
					TriggerFinish(DR_OK);
				}
			});
		}
	}

private:
	Path path_;
	std::function<void(const Path &)> selected_;
};
} // namespace

UIScreen *CreateSwitchImagePickerScreen(const Path &start, std::function<void(const Path &)> selected) {
	return new ShortcutImagePicker(start, std::move(selected));
}
#endif
