#pragma once

#include "UI/SimpleDialogScreen.h"

class SwitchLibraryScreen : public UISimpleBaseDialogScreen {
public:
	SwitchLibraryScreen() : UISimpleBaseDialogScreen(Path(), SimpleDialogFlags::ContentsCanScroll) {}

	const char *tag() const override { return "SwitchLibrary"; }
	std::string_view GetTitle() const override { return "Library"; }
	void CreateDialogViews(UI::ViewGroup *parent) override;
	void dialogFinished(const Screen *dialog, DialogResult result) override;
};
