#include "UI/SwitchLibraryScreen.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string_view>

#include "Common/Data/Text/I18n.h"
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include "Common/System/OSD.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/ScreenManager.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"

#include "UI/EmuScreen.h"
#include "UI/SwitchLibrary.h"

namespace {

enum class BrowserMode {
	Explorer,
	PickFolder,
};

bool ValidEntryName(std::string_view name) {
	if (name.empty() || name == "." || name == "..")
		return false;
	return std::none_of(name.begin(), name.end(), [](unsigned char c) {
		return c < 0x20 || c == '/' || c == '\\' || c == ':';
	});
}

bool IsStorageRoot(const Path &path) {
	std::string value = path.ToString();
	while (value.size() > 1 && value.back() == '/' && value[value.size() - 2] == '/')
		value.pop_back();
	if (value == "/" || value == "sdmc:/")
		return true;
	const size_t colon = value.find(':');
	return colon != std::string::npos && value.size() == colon + 2 && value.back() == '/';
}

bool IsSameOrChildPath(const Path &path, const Path &parent) {
	std::string value = path.ToString();
	std::string root = parent.ToString();
	std::replace(value.begin(), value.end(), '\\', '/');
	std::replace(root.begin(), root.end(), '\\', '/');
	while (root.size() > 1 && root.back() == '/')
		root.pop_back();
	return value == root || (value.size() > root.size() && value.compare(0, root.size(), root) == 0 && value[root.size()] == '/');
}

bool CopyTree(const Path &source, const Path &destination) {
	if (!File::IsDirectory(source))
		return File::Copy(source, destination);
	if (!File::CreateFullPath(destination))
		return false;
	std::vector<File::FileInfo> entries;
	if (!File::GetFilesInDir(source, &entries, nullptr, File::GETFILES_GETHIDDEN))
		return false;
	for (const auto &entry : entries) {
		if (!CopyTree(entry.fullName, destination / entry.name))
			return false;
	}
	return true;
}

bool MoveTree(const Path &source, const Path &destination) {
	if (File::Move(source, destination))
		return true;
	if (!CopyTree(source, destination))
		return false;
	return File::IsDirectory(source) ? File::DeleteDirRecursively(source) : File::Delete(source);
}

class SwitchFileBrowserScreen;

class SwitchFileActionScreen : public UISimpleBaseDialogScreen {
public:
	SwitchFileActionScreen(const Path &path, std::function<void()> changed)
		: UISimpleBaseDialogScreen(Path(), SimpleDialogFlags::ContentsCanScroll), path_(path), changed_(std::move(changed)), name_(path.GetFilename()) {}

	const char *tag() const override { return "SwitchFileAction"; }
	std::string_view GetTitle() const override { return "File options"; }
	void CreateDialogViews(UI::ViewGroup *parent) override;

private:
	void CopyOrMove(bool move);

	Path path_;
	std::function<void()> changed_;
	std::string name_;
	std::string newFolderName_;
};

class SwitchFileBrowserScreen : public UISimpleBaseDialogScreen {
public:
	SwitchFileBrowserScreen(BrowserMode mode, const Path &start, std::function<void(const Path &)> selected = {})
		: UISimpleBaseDialogScreen(Path(), SimpleDialogFlags::ContentsCanScroll), mode_(mode), current_(start), selected_(std::move(selected)) {}

	const char *tag() const override { return "SwitchFileBrowser"; }
	std::string_view GetTitle() const override { return mode_ == BrowserMode::PickFolder ? "Choose game folder" : "File Explorer"; }
	void CreateDialogViews(UI::ViewGroup *parent) override;
	void dialogFinished(const Screen *dialog, DialogResult result) override {
		RecreateViews();
	}

private:
	void Open(const Path &path) {
		current_ = path;
		RecreateViews();
	}

	BrowserMode mode_;
	Path current_;
	std::function<void(const Path &)> selected_;
};

class SwitchSmbEditScreen : public UISimpleBaseDialogScreen {
public:
	explicit SwitchSmbEditScreen(SwitchStorage::SmbShare share)
		: UISimpleBaseDialogScreen(Path(), SimpleDialogFlags::ContentsCanScroll), share_(std::move(share)), originalId_(share_.id) {}

	const char *tag() const override { return "SwitchSmbEdit"; }
	std::string_view GetTitle() const override { return share_.id.empty() ? "Add SMB share" : "Edit SMB share"; }
	void CreateDialogViews(UI::ViewGroup *parent) override;

private:
	std::string MakeId() const;
	SwitchStorage::SmbShare share_;
	std::string originalId_;
};

void ShowResult(ScreenManager *manager, bool success, const std::string &failure) {
	g_OSD.Show(success ? OSDType::MESSAGE_SUCCESS : OSDType::MESSAGE_ERROR,
		success ? "Operation completed" : failure);
	if (!success)
		manager->push(new UI::MessagePopupScreen("File operation", failure, "OK", ""));
}

void SwitchFileActionScreen::CopyOrMove(bool move) {
	screenManager()->push(new SwitchFileBrowserScreen(BrowserMode::PickFolder, path_.NavigateUp(),
		[this, move](const Path &directory) {
			Path destination = directory / path_.GetFilename();
			if (IsSameOrChildPath(destination, path_)) {
				ShowResult(screenManager(), false, "The destination cannot be inside the source");
				return;
			}
			auto perform = [this, move, destination](bool replace) {
				if (replace) {
					bool removed = File::IsDirectory(destination) ? File::DeleteDirRecursively(destination) : File::Delete(destination);
					if (!removed) {
						ShowResult(screenManager(), false, "The existing destination could not be replaced");
						return;
					}
				}
				bool success = move ? MoveTree(path_, destination) : CopyTree(path_, destination);
				ShowResult(screenManager(), success, move ? "Move failed" : "Copy failed");
				if (success && changed_)
					changed_();
			};
			if (File::Exists(destination)) {
				screenManager()->push(new UI::MessagePopupScreen(move ? "Move" : "Copy",
					"An item with the same name already exists in that folder.", "Replace", "Cancel",
					[perform](bool yes) {
						if (yes)
							perform(true);
					}));
			} else {
				perform(false);
			}
		}));
}

void SwitchFileActionScreen::CreateDialogViews(UI::ViewGroup *parent) {
	using namespace UI;
	parent->Add(new TextView(path_.ToString(), ALIGN_LEFT | FLAG_WRAP_TEXT, false));
	parent->Add(new ItemHeader("Actions"));
	if (!File::IsDirectory(path_)) {
		parent->Add(new Choice("Launch", ImageID("I_PLAY")))->OnClick.Add([this](EventParams &) {
			screenManager()->switchScreen(new EmuScreen(path_));
		});
	}
	const bool storageRoot = IsStorageRoot(path_);
	if (!storageRoot)
		parent->Add(new PopupTextInputChoice(GetRequesterToken(), &name_, "Name", "", 255, screenManager()));
	Choice *rename = storageRoot ? nullptr : parent->Add(new Choice("Rename"));
	if (rename)
		rename->OnClick.Add([this](EventParams &) {
		if (name_.empty() || name_ == path_.GetFilename())
			return;
		if (!ValidEntryName(name_)) {
			ShowResult(screenManager(), false, "Names cannot contain /, \\, :, or control characters");
			return;
		}
		Path destination = path_.NavigateUp() / name_;
		if (File::Exists(destination)) {
			ShowResult(screenManager(), false, "An item with that name already exists");
			return;
		}
		bool success = File::Rename(path_, destination);
		ShowResult(screenManager(), success, "Rename failed");
		if (success) {
			path_ = destination;
			if (changed_)
				changed_();
			RecreateViews();
		}
		});
	if (!storageRoot) {
		parent->Add(new Choice("Copy to..."))->OnClick.Add([this](EventParams &) { CopyOrMove(false); });
		parent->Add(new Choice("Move to..."))->OnClick.Add([this](EventParams &) { CopyOrMove(true); });
	}
	if (File::IsDirectory(path_)) {
		parent->Add(new PopupTextInputChoice(GetRequesterToken(), &newFolderName_, "New folder name", "", 255, screenManager()));
		parent->Add(new Choice("Create folder"))->OnClick.Add([this](EventParams &) {
			bool success = ValidEntryName(newFolderName_) && File::CreateDir(path_ / newFolderName_);
			ShowResult(screenManager(), success, "Could not create folder");
			if (success) {
				newFolderName_.clear();
				if (changed_)
					changed_();
			}
		});
	}
	if (!storageRoot)
		parent->Add(new Choice("Delete", ImageID("I_TRASHCAN")))->OnClick.Add([this](EventParams &) {
		screenManager()->push(new MessagePopupScreen("Delete", "Delete this item permanently?", "Delete", "Cancel", [this](bool yes) {
			if (!yes)
				return;
			bool success = File::IsDirectory(path_) ? File::DeleteDirRecursively(path_) : File::Delete(path_);
			ShowResult(screenManager(), success, "Delete failed");
			if (success) {
				if (changed_)
					changed_();
				TriggerFinish(DR_OK);
			}
		}));
		});
}

void SwitchFileBrowserScreen::CreateDialogViews(UI::ViewGroup *parent) {
	using namespace UI;
	auto addLocation = [&](std::string_view label, const Path &path, ImageID icon) {
		parent->Add(new Choice(label, icon))->OnClick.Add([this, path](EventParams &) { Open(path); });
	};
	if (current_.empty()) {
		parent->Add(new ItemHeader("Locations"));
		addLocation("SD Card", Path("/"), ImageID("I_SDCARD"));
		for (const auto &location : SwitchStorage::ListUsbLocations())
			addLocation(location.label, Path(location.path), ImageID("I_FOLDER"));
		for (const auto &share : SwitchLibrary::GetSmbShares()) {
			std::string error;
			if (!SwitchStorage::IsSmbMounted(share.id))
				SwitchStorage::MountSmb(share, &error);
			if (SwitchStorage::IsSmbMounted(share.id))
				addLocation(share.name.empty() ? share.share : share.name, Path(SwitchStorage::SmbBrowsePath(share)), ImageID("I_FOLDER"));
		}
		return;
	}

	parent->Add(new TextView(current_.ToString(), ALIGN_LEFT | FLAG_WRAP_TEXT, false));
	LinearLayout *commands = parent->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
	commands->Add(new Choice("Locations", ImageID("I_HOME"), new LinearLayoutParams(1.0f)))->OnClick.Add([this](EventParams &) {
		current_ = Path();
		RecreateViews();
	});
	if (current_.CanNavigateUp()) {
		commands->Add(new Choice("Up", ImageID("I_UP_DIRECTORY"), new LinearLayoutParams(1.0f)))->OnClick.Add([this](EventParams &) {
			Open(current_.NavigateUp());
		});
	}
	if (mode_ == BrowserMode::PickFolder) {
		commands->Add(new Choice("Use this folder", ImageID("I_CHECKEDBOX"), new LinearLayoutParams(1.0f)))->OnClick.Add([this](EventParams &) {
			if (selected_)
				selected_(current_);
			TriggerFinish(DR_OK);
		});
	} else {
		commands->Add(new Choice("Folder options", ImageID("I_GEAR"), new LinearLayoutParams(1.0f)))->OnClick.Add([this](EventParams &) {
			screenManager()->push(new SwitchFileActionScreen(current_, [this] { RecreateViews(); }));
		});
	}

	std::vector<File::FileInfo> entries;
	File::GetFilesInDir(current_, &entries, nullptr, File::GETFILES_GETHIDDEN);
	std::stable_sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
		if (a.isDirectory != b.isDirectory)
			return a.isDirectory;
		return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
	});
	parent->Add(new ItemHeader("Files"));
	for (const auto &entry : entries) {
		LinearLayout *row = parent->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		Choice *item = row->Add(new Choice(entry.name, entry.isDirectory ? ImageID("I_FOLDER") : ImageID("I_FILE"), new LinearLayoutParams(1.0f)));
		if (entry.isDirectory) {
			item->OnClick.Add([this, path = entry.fullName](EventParams &) { Open(path); });
		} else if (mode_ == BrowserMode::Explorer) {
			item->OnClick.Add([this, path = entry.fullName](EventParams &) {
				screenManager()->push(new SwitchFileActionScreen(path, [this] { RecreateViews(); }));
			});
		} else {
			item->SetEnabled(false);
		}
		if (mode_ == BrowserMode::Explorer) {
			row->Add(new Choice(ImageID("I_GEAR"), new LinearLayoutParams(64.0f, 64.0f)))->OnClick.Add([this, path = entry.fullName](EventParams &) {
				screenManager()->push(new SwitchFileActionScreen(path, [this] { RecreateViews(); }));
			});
		}
	}
}

std::string SwitchSmbEditScreen::MakeId() const {
	std::string id;
	std::string basis = share_.name.empty() ? share_.share : share_.name;
	for (char c : basis) {
		if (std::isalnum((unsigned char)c))
			id.push_back((char)std::tolower((unsigned char)c));
		else if (!id.empty() && id.back() != '_')
			id.push_back('_');
		if (id.size() == 12)
			break;
	}
	if (id.empty())
		id = "share";
	std::string candidate = id;
	int suffix = 2;
	auto shares = SwitchLibrary::GetSmbShares();
	while (std::any_of(shares.begin(), shares.end(), [&](const auto &item) { return item.id == candidate; }))
		candidate = id + std::to_string(suffix++);
	return candidate;
}

void SwitchSmbEditScreen::CreateDialogViews(UI::ViewGroup *parent) {
	using namespace UI;
	parent->Add(new PopupTextInputChoice(GetRequesterToken(), &share_.name, "Name", "Living room NAS", 64, screenManager()));
	parent->Add(new PopupTextInputChoice(GetRequesterToken(), &share_.server, "Server", "192.168.1.10", 255, screenManager()));
	parent->Add(new PopupTextInputChoice(GetRequesterToken(), &share_.share, "Share", "Games", 255, screenManager()));
	parent->Add(new PopupTextInputChoice(GetRequesterToken(), &share_.path, "Path", "PSP", 1024, screenManager()));
	parent->Add(new PopupTextInputChoice(GetRequesterToken(), &share_.user, "Username", "", 255, screenManager()));
	parent->Add(new PopupTextInputChoice(GetRequesterToken(), &share_.password, "Password", "", 255, screenManager()))->SetPasswordDisplay();
	parent->Add(new PopupTextInputChoice(GetRequesterToken(), &share_.domain, "Domain", "", 255, screenManager()));
	parent->Add(new CheckBox(&share_.autoMount, "Connect automatically"));
	parent->Add(new Choice("Save and connect", ImageID("I_CHECKEDBOX")))->OnClick.Add([this](EventParams &) {
		if (share_.id.empty())
			share_.id = MakeId();
		std::string error;
		if (!SwitchLibrary::SaveSmbShare(share_, &error)) {
			screenManager()->push(new MessagePopupScreen("SMB", error, "OK", ""));
			return;
		}
		TriggerFinish(DR_OK);
	});
	if (!originalId_.empty()) {
		parent->Add(new Choice("Remove share", ImageID("I_TRASHCAN")))->OnClick.Add([this](EventParams &) {
			screenManager()->push(new MessagePopupScreen("Remove SMB share", "Remove this share from PPSSPP?", "Remove", "Cancel", [this](bool yes) {
				if (yes) {
					SwitchLibrary::RemoveSmbShare(originalId_);
					TriggerFinish(DR_OK);
				}
			}));
		});
	}
}

}  // namespace

void SwitchLibraryScreen::CreateDialogViews(UI::ViewGroup *parent) {
	using namespace UI;
	parent->Add(new ItemHeader("Game folders"));
	parent->Add(new Choice("File Explorer", ImageID("I_FOLDER")))->OnClick.Add([this](EventParams &) {
		screenManager()->push(new SwitchFileBrowserScreen(BrowserMode::Explorer, Path()));
	});
	parent->Add(new Choice("Add game folder", ImageID("I_FOLDER_OPEN")))->OnClick.Add([this](EventParams &) {
		screenManager()->push(new SwitchFileBrowserScreen(BrowserMode::PickFolder, Path(), [this](const Path &path) {
			SwitchLibrary::AddSource(path);
			RecreateViews();
		}));
	});

	parent->Add(new ItemHeader("Saved folders"));
	auto sources = SwitchLibrary::GetSources();
	for (size_t i = 0; i < sources.size(); ++i) {
		LinearLayout *row = parent->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		std::string description = SwitchLibrary::DescribeSource(sources[i]);
		row->Add(new Choice(description, ImageID("I_FOLDER"), new LinearLayoutParams(1.0f)))->OnClick.Add([this, source = sources[i]](EventParams &) {
			std::string error;
			std::string path = SwitchLibrary::ResolveSource(source, true, &error);
			if (path.empty()) {
				screenManager()->push(new MessagePopupScreen("Library", error, "OK", ""));
				return;
			}
			screenManager()->push(new SwitchFileBrowserScreen(BrowserMode::Explorer, Path(path)));
		});
		row->Add(new Choice(ImageID("I_ROTATE_LEFT"), new LinearLayoutParams(64.0f, 64.0f)))->OnClick.Add([i](EventParams &) {
			SwitchLibrary::RequestScan(i);
		});
		row->Add(new Choice(ImageID("I_TRASHCAN"), new LinearLayoutParams(64.0f, 64.0f)))->OnClick.Add([this, i](EventParams &) {
			SwitchLibrary::RemoveSource(i);
			RecreateViews();
		});
	}

	parent->Add(new ItemHeader("USB drives"));
	auto usb = SwitchStorage::ListUsbLocations();
	if (usb.empty())
		parent->Add(new TextView("No USB drives detected", ALIGN_LEFT, false));
	for (const auto &location : usb) {
		LinearLayout *row = parent->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		row->Add(new Choice(location.label, ImageID("I_FOLDER"), new LinearLayoutParams(1.0f)))->OnClick.Add([this, path = location.path](EventParams &) {
			screenManager()->push(new SwitchFileBrowserScreen(BrowserMode::Explorer, Path(path)));
		});
		row->Add(new Choice("Eject", new LinearLayoutParams(110.0f, 64.0f)))->OnClick.Add([this, id = location.id](EventParams &) {
			std::string error;
			if (!SwitchStorage::SafelyEjectUsb(id, &error))
				screenManager()->push(new MessagePopupScreen("USB", error, "OK", ""));
			RecreateViews();
		});
	}

	parent->Add(new ItemHeader("SMB network shares"));
	for (const auto &share : SwitchLibrary::GetSmbShares()) {
		std::string label = share.name.empty() ? share.share : share.name;
		if (SwitchStorage::IsSmbMounted(share.id))
			label += " (connected)";
		parent->Add(new Choice(label, ImageID("I_FOLDER")))->OnClick.Add([this, share](EventParams &) {
			screenManager()->push(new SwitchSmbEditScreen(share));
		});
	}
	parent->Add(new Choice("Add SMB share", ImageID("I_PLUS")))->OnClick.Add([this](EventParams &) {
		screenManager()->push(new SwitchSmbEditScreen({}));
	});
}

void SwitchLibraryScreen::dialogFinished(const Screen *, DialogResult) {
	RecreateViews();
}
