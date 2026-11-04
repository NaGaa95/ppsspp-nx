#pragma once

#include <functional>

#include "UI/SimpleDialogScreen.h"

UIScreen *CreateSwitchImagePickerScreen(const Path &start, std::function<void(const Path &)> selected);

class SwitchLibraryScreen : public UISimpleBaseDialogScreen {
public:
	SwitchLibraryScreen() : UISimpleBaseDialogScreen(Path(), SimpleDialogFlags::ContentsCanScroll) {}

	const char *tag() const override { return "SwitchLibrary"; }
	std::string_view GetTitle() const override { return "Library"; }
	void CreateDialogViews(UI::ViewGroup *parent) override;
	void dialogFinished(const Screen *dialog, DialogResult result) override;
};
