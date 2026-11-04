#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Render actual Switch menu methods with host SDL3 and fake platform services.

Requires SDL3 (a build directory), clang++, FreeType, HarfBuzz and FriBidi.
Checks dialog text ownership, input, footer bounds, menu layout, rounded-panel
command counts and text-cache eviction. Saves software-rendered BMP previews.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def body(source, marker):
    start = source.index("{", source.index(marker))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdl-build", type=Path, required=True)
    parser.add_argument("--baseline", help="Use this revision to reproduce a menu regression")
    parser.add_argument("--baseline-method", choices=("Round", "Ask"), default="Round")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = (root / "SDL/SwitchLauncher.cpp").read_text()
    baseline = subprocess.check_output(["git", "show", f"{args.baseline}:SDL/SwitchLauncher.cpp"], cwd=root, text=True) if args.baseline else source
    fixture = r'''
#include <SDL3/SDL.h>
#include "SDL/SwitchText.h"
#include "SDL/SwitchLauncherUtil.h"
#include "Common/SwitchUIColors.h"
#include "Common/Data/Text/WrapText.h"
#include "Common/Render/DrawEnum.h"
#include "Common/StringUtils.h"
#include "UI/SwitchSteamGridDB.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
using namespace SwitchLauncher;
constexpr int WIDTH = 1280, HEIGHT = 720;
constexpr size_t MAX_TEXT_BYTES = 12 * 1024 * 1024;
constexpr const char *ART_DIRECTORY = "/covers";
int fills = 0, copies = 0, geometries = 0, promptCalls = 0, installCalls = 0;
uint64_t fakeTicks = 0;
bool CountFill(SDL_Renderer *r, const SDL_FRect *rect) { ++fills; return SDL_RenderFillRect(r, rect); }
bool CountCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_FRect *src, const SDL_FRect *dst) { ++copies; return SDL_RenderTexture(r, t, src, dst); }
bool CountGeometry(SDL_Renderer *r, SDL_Texture *t, const SDL_Vertex *v, int n, const int *i, int count) {
	++geometries; return SDL_RenderGeometry(r, t, v, n, i, count);
}
#define SDL_RenderFillRect CountFill
#define SDL_RenderTexture CountCopy
#define SDL_RenderGeometry CountGeometry
#define SDL_GetTicks() fakeTicks
std::string_view Tr(std::string_view value) { return value; }
class UiText @UITEXT@;
struct Path {
	std::string value;
	Path() = default;
	explicit Path(std::string v) : value(std::move(v)) {}
	const std::string &ToString() const { return value; }
	std::string GetFilename() const { return "game.iso"; }
};
enum class GameInfoFlags { ICON };
struct GameInfo {
	std::mutex lock;
	struct { std::string data = "image"; } icon;
	void WaitUntilReady(GameInfoFlags) {}
};
struct Cache {
	std::shared_ptr<GameInfo> info = std::make_shared<GameInfo>();
	std::shared_ptr<GameInfo> GetInfo(void *, const Path &, GameInfoFlags) { return info; }
} cache;
Cache *g_gameInfoCache = &cache;
namespace File {
bool Exists(const Path &) { return true; }
bool Delete(const Path &) { return true; }
int64_t GetFileSize(const Path &) { return 5; }
bool ReadBinaryFileToString(const Path &, std::string *data) { *data = "image"; return true; }
}
bool startsWith(const std::string &value, const char *prefix) { return value.rfind(prefix, 0) == 0; }
bool SwitchForwarder_Create(const std::string &, const std::string &, const std::string &, const std::vector<uint8_t> &, std::string *) {
	++installCalls; return true;
}
SteamGridDBResult SteamGridDB_DownloadImage(const std::string &, std::vector<uint8_t> *, const std::atomic<bool> *) { return SteamGridDBResult::OK; }
const char *SteamGridDB_ResultMessage(SteamGridDBResult) { return "Error"; }
struct Section {
	bool Get(const char *, std::string *) { return false; }
	void Set(const char *, const std::string &) {}
} appearance;
struct Entry {
	Path path;
	std::string identity, key = "game", title = "Example PSP Game", id = "ULUS10000";
	std::shared_ptr<GameInfo> info = cache.info;
};
struct Texture @TEXTURE@;
struct Row @ROW@;
struct RowPage @ROWPAGE@;
class Launcher {
public:
	SDL_Surface *surface_;
	SDL_Renderer *renderer_;
	SDL_Texture *roundTexture_ = nullptr;
	std::unique_ptr<TextEngine> textEngine_ = std::make_unique<TextEngine>();
	std::vector<std::vector<uint8_t>> fontData_;
	float fontScale_ = 1;
	struct {
		SDL_Color text{235,242,250,255}, dim{163,179,198,255}, value{106,228,247,255}, accent{93,188,232,255};
	} palette_;
	std::unordered_map<std::string, Texture> text_;
	std::unordered_map<std::string, float> textWidths_;
	std::unordered_map<std::string, SDL_Texture *> atlasArt_;
	size_t textBytes_ = 0;
	uint64_t use_ = 0;
	bool animations_ = false;
	std::map<std::string, int> rowSelections_;
	StickNavigation stickNavigation_;
	int repeatDirection_ = 0;
	uint64_t startedAt_ = 0;
	bool updateAvailable_ = false;
	void *updateTask_ = nullptr;
	Section *appearance_ = &appearance;
	std::deque<SDL_Event> events_;
	std::function<void(int)> onFrame_;
	int frames_ = 0, frameLimit_ = 1;
	std::string screenshot_;
	struct Label { std::string text; float x, y; };
	std::vector<Label> labels_;
	Launcher(const char *folder, float scale = 1) : fontScale_(scale) {
		surface_ = SDL_CreateSurface((int)(WIDTH * scale), (int)(HEIGHT * scale), SDL_PIXELFORMAT_RGBA32);
		assert(surface_);
		renderer_ = SDL_CreateSoftwareRenderer(surface_);
		assert(renderer_);
		SDL_SetRenderScale(renderer_, scale, scale);
		SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
		for (const char *name : {"NotoSans-Regular.ttf", "NotoSansArabic-Regular.ttf"}) {
			const std::string path = std::string(folder) + "/" + name;
			size_t size = 0;
			void *data = SDL_LoadFile(path.c_str(), &size);
			assert(data && size);
			fontData_.emplace_back((uint8_t *)data, (uint8_t *)data + size);
			SDL_free(data);
			assert(textEngine_->AddFont(fontData_.back().data(), fontData_.back().size()));
		}
		InitRoundTexture();
	}
	~Launcher() {
		for (auto &[key, image] : text_) { SDL_DestroyTexture(image.texture); }
		SDL_DestroyTexture(roundTexture_);
		textEngine_.reset();
		SDL_DestroyRenderer(renderer_);
		SDL_DestroySurface(surface_);
	}
	bool Frame() {
		if (++frames_ > frameLimit_) { return false; }
		fakeTicks = frames_ * 600;
		if (onFrame_) { onFrame_(frames_); }
		return true;
	}
	bool Poll(SDL_Event *event) {
		if (events_.empty()) { return false; }
		*event = events_.front(); events_.pop_front(); return true;
	}
	bool Button(const SDL_Event &event, SDL_GamepadButton button, SDL_Keycode key) const {
		return (event.type == SDL_EVENT_KEY_DOWN && event.key.key == key) ||
			(event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == button);
	}
	bool Confirm(const SDL_Event &event) const { return Button(event, SDL_GAMEPAD_BUTTON_EAST, SDLK_RETURN); }
	bool Back(const SDL_Event &event) const { return Button(event, SDL_GAMEPAD_BUTTON_SOUTH, SDLK_ESCAPE); }
	int Navigate(const SDL_Event &event) {
		if (event.type != SDL_EVENT_KEY_DOWN) { return 0; }
		return event.key.key == SDLK_DOWN ? 2 : event.key.key == SDLK_UP ? -2 : event.key.key == SDLK_LEFT ? -1 : event.key.key == SDLK_RIGHT ? 1 : 0;
	}
	void Present() {
		assert(SDL_RenderPresent(renderer_));
		if (!screenshot_.empty()) { assert(SDL_SaveBMP(surface_, screenshot_.c_str())); }
	}
	void Background() { Fill(0, 0, WIDTH, HEIGHT, {36,77,117,255}); Gradient({0,0,WIDTH,HEIGHT}, {20,45,75,0}, {3,12,28,230}); }
	void Sound(int) {}
	void Updates() {}
	void Cover(Entry &, SDL_FRect rect, bool) { Fill(rect.x,rect.y,rect.w,rect.h,{10,24,44,255}); Center(rect.x+rect.w/2,rect.y+rect.h/2-22,"PSP",38); }
	SDL_Texture *Artwork(Entry &) { return nullptr; }
	SDL_Texture *RemoteImage(const std::string &) { return nullptr; }
	void ImageFit(SDL_Texture *, SDL_FRect, bool) {}
	void DownloadArtwork(Entry &, bool) {}
	void ImportCover(Entry &) {}
	void InvalidateArtwork(Entry &) {}
	bool PickShortcutIcon(Entry &, std::string *) { return false; }
	bool Prompt(UiText, std::string *, bool = false) { ++promptCalls; return false; }
	bool Busy(UiText, const std::function<void(const std::atomic<bool> &)> &work, bool = true) { std::atomic<bool> cancel{false}; work(cancel); return true; }
	void Fill(float x, float y, float w, float h, SDL_Color color) @FILL@
	void InitRoundTexture() @INIT_ROUND@
	void Round(float x, float y, float w, float h, float radius, SDL_Color color) @ROUND@
	void Panel(float x, float y, float w, float h, bool selected = false) @PANEL@
	void Gradient(SDL_FRect rect, SDL_Color top, SDL_Color bottom) @GRADIENT@
	float TextWidth(UiText rawvalue, int size) @WIDTH@
	void Text(float x, float y, UiText rawvalue, int size = 26, SDL_Color color = {0,0,0,0}) @TEXT@
	void Center(float x, float y, UiText rawvalue, int size, SDL_Color color = {0,0,0,0}) @CENTER@
	void PageHeader(UiText rawtitle, UiText raweyebrow, UiText rawcontext = {}, UiText rawdetail = {}) @HEADER@
	void SectionHeading(UiText rawtitle, float x, float y, float width) @SECTION@
	std::vector<std::string> Wrap(UiText rawtext, float width, int size) @WRAP@
	void Wrapped(float x, float y, float width, std::string_view text, int size, int maxLines, SDL_Color color) @WRAPPED@
	void Footer(UiText rawvalue = {}) @FOOTER@
	bool Ask(UiText rawtitle, UiText rawmessage) @ASK@
	void Message(UiText rawtitle, UiText rawtext) @MESSAGE@
	int Rows(UiText rawtitleView, const std::function<std::vector<Row>()> &build, int initial = -1, RowPage page = {}) @ROWS@
	void CoverSettings(Entry &entry) @COVERS@
	void CreateShortcut(Entry &entry) @SHORTCUT@
};
SDL_Event Key(SDL_Keycode key) { SDL_Event event{}; event.type = SDL_EVENT_KEY_DOWN; event.key.key = key; return event; }
SDL_Event Touch(float x, float y) { SDL_Event event{}; event.type = SDL_EVENT_FINGER_UP; event.tfinger.x = x; event.tfinger.y = y; return event; }
int main(int argc, char **argv) {
	assert(argc == 3);
	assert(SDL_Init(0));
	const std::string output = argv[2];
	{
		Launcher l(argv[1]);
		fills = copies = geometries = 0;
		l.Round(10, 10, 280, 410, 14, {16,23,39,235});
		fprintf(stderr, "Rounded panel: %d drawing commands, independent of height.\n", fills + copies + geometries);
		assert(fills + copies + geometries <= 1);
	}
	{
		Launcher l(argv[1]);
		Entry e; e.title = "Daxter — Clear game settings اختبار";
		const auto original = e.title;
		l.frameLimit_ = 2;
		l.screenshot_ = output + "/menu-confirmation.bmp";
		l.onFrame_ = [&](int frame) { if (frame == 2) { e.title.assign(10000, 'X'); } };
		assert(!l.Ask("Clear game settings?", e.title));
		assert(std::count_if(l.labels_.begin(), l.labels_.end(), [&](const auto &label) { return label.text == original; }) == 2);
	}
	for (const auto &input : {Key(SDLK_RETURN), Key(SDLK_ESCAPE), Touch(500, 420), Touch(780, 420)}) {
		Launcher l(argv[1]);
		l.events_.push_back(input);
		const bool yes = input.type == SDL_EVENT_KEY_DOWN ? input.key.key == SDLK_RETURN : input.tfinger.x < 640;
		assert(l.Ask("Delete game?", std::string("Game title")) == yes);
	}
	{
		Launcher l(argv[1], 1.5f);
		Entry e;
		std::vector<Row> rows;
		for (const char *label : {"Launch", "Game settings", "Rename game", "Favorite", "Cover settings", "Create HOME shortcut",
			"Manage installed content and savedata", "Clear shader caches", "Clear game settings", "Delete game"}) { rows.emplace_back(label, ""); }
		rows.back().labelColor = {238,135,135,255};
		RowPage page{&e, 6, "General", "Manage game", "Game menu"};
		page.hints = "A  Select        B  Back";
		l.screenshot_ = output + "/menu-game.bmp";
		l.Rows(e.title, [&] { return rows; }, 9, page);
		for (const auto &label : l.labels_) {
			if (label.text == "Select" || label.text == "Back") { assert(label.y >= 681); }
			if (label.text == "Delete game") { assert(label.y < 608); }
		}
	}
	{
		Launcher l(argv[1]); Entry e;
		l.screenshot_ = output + "/menu-covers.bmp";
		l.CoverSettings(e);
		assert(promptCalls == 0 && installCalls == 0);
	}
	{
		Launcher l(argv[1]); Entry e;
		l.screenshot_ = output + "/menu-shortcut.bmp";
		l.CreateShortcut(e);
		assert(promptCalls == 0 && installCalls == 0);
	}
	{
		Launcher l(argv[1]); Entry e;
		l.events_ = {Key(SDLK_RIGHT), Key(SDLK_DOWN), Key(SDLK_DOWN), Key(SDLK_RETURN)};
		l.CreateShortcut(e);
		assert(promptCalls == 0 && installCalls == 1);
	}
	{
		Launcher l(argv[1]);
		for (int i = 0; i < 600; ++i) {
			l.Text(0, 0, "Keep this label", 20);
			l.Text(0, 0, std::string("Other label ") + std::to_string(i), 20);
		}
		assert(l.text_.size() <= 512 && l.textBytes_ <= MAX_TEXT_BYTES);
		assert(l.text_.count("20:Keep this label"));
		l.Footer("Left / Right  Change        L / R  +/- 5%        X  Enter value        Y  Reset        A  Apply        B  Cancel");
		assert(l.labels_.back().y >= 681);
	}
	puts("Switch menu checks passed: stable confirmation text, A/B/touch confirmation, bottom footer, "
		"complete game menu, cover cards, shortcut editor, bounded LRU text cache.");
	SDL_Quit();
}
'''
    markers = {
        "UITEXT": "class UiText", "TEXTURE": "struct Texture", "ROW": "struct Row {", "ROWPAGE": "struct RowPage",
        "FILL": "void Launcher::Fill(", "INIT_ROUND": "void Launcher::InitRoundTexture(", "ROUND": "void Launcher::Round(",
        "PANEL": "void Launcher::Panel(", "GRADIENT": "void Launcher::Gradient(", "WIDTH": "float Launcher::TextWidth(",
        "TEXT": "void Launcher::Text(", "CENTER": "void Launcher::Center(", "HEADER": "void Launcher::PageHeader(",
        "SECTION": "void Launcher::SectionHeading(", "WRAP": "std::vector<std::string> Launcher::Wrap(",
        "WRAPPED": "void Launcher::Wrapped(", "FOOTER": "void Launcher::Footer(", "ASK": "bool Launcher::Ask(",
        "MESSAGE": "void Launcher::Message(", "ROWS": "int Launcher::Rows(", "COVERS": "void Launcher::CoverSettings(",
        "SHORTCUT": "void Launcher::CreateShortcut(",
    }
    for name, marker in markers.items():
        implementation = body(baseline if name == args.baseline_method.upper() else source, marker)
        if name == "TEXT":
            implementation = implementation.replace("const std::string_view value = rawvalue;", "const std::string_view value = rawvalue; labels_.push_back({std::string(value), x, y});")
        fixture = fixture.replace(f"@{name}@", implementation)
    output = root / "build-switch-deps"
    sdl_build = args.sdl_build.resolve()
    pkg_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "freetype2", "harfbuzz", "fribidi", "glib-2.0"], text=True))
    with tempfile.TemporaryDirectory(prefix="ppsspp-menu-") as directory:
        test = Path(directory) / "test.cpp"
        test.write_text(fixture)
        executable = Path(directory) / "test"
        compiler = shlex.split(os.environ.get("CXX", "clang++"))
        subprocess.run(compiler + ["-std=c++17", "-O1", "-g", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                                  "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(root),
                                  "-I", str(root / "build-switch-deps/sdl-src/include"), str(test),
                                  str(root / "SDL/SwitchText.cpp"), str(root / "Common/Data/Encoding/Utf8.cpp"),
                                  str(root / "Common/Data/Text/WrapText.cpp"), str(root / "Common/StringUtils.cpp"),
                                  "-L", str(sdl_build), "-Wl,-rpath," + str(sdl_build), "-lSDL3", "-o", str(executable)] + pkg_flags, check=True)
        subprocess.run([str(executable), str(root / "assets/launcher/fonts"), str(output)], check=True, timeout=60)


if __name__ == "__main__":
    main()
