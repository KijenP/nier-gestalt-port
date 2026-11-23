// nier - the title menu on the phone as a row of big cards (2026-11-16, the user's request).
//
// The title's lists (New Game / Continue / Options / Downloadable Content, and the difficulty after New Game) stay
// the game's own: the carousel only shows them, moves their row the way the game does (video_options_menu.cpp
// TitleSetRow: its sound and highlight) and presses A for a chosen card, so the saves, notices and Options all run
// as before. The game's rows are left undrawn (the renderer skips the draws of their textures, by the title pack's
// pages); the black screen, the NIER logo and the copyright stay where they are. Cards scroll sideways: a swipe
// moves a card at a time, a touch on a side card brings it to the middle, a touch on the middle one chooses it.
// The cards' words are the game's own images from 2D\TITLE.2DP as loaded (the translation's, if one is installed).
//
// On Android always; on the PC with NIER_TITLE_CAROUSEL=1 (to look at it: the mouse drives it there).
#pragma once

#include <cstdint>
#include <memory>

namespace rex::ui {
class ImGuiDrawer;
class ImGuiDialog;
}  // namespace rex::ui

namespace nier::title_carousel {

bool Enabled();

// The game's thread, each frame the title object runs (any state): the pages of its textures may be hidden.
void OnTitleTick();
// The game's thread, while a title list takes input: which list (0 menu, 1 difficulty), its rows and row, and
// its rows' highlight names (the cards' words follow them). Returns the row the carousel asks for (-1: none).
int OnTitleList(int kind, int rows, int row, const char* const names[4]);

// The cards are on screen (their touches are theirs, never the game's).
bool Showing();
// The input thread (touch_controls.h on Android; the dialog with the mouse on the PC): the fingers on the cards now
// (fractions of the window), each poll; one that is gone was lifted.
struct Touch {
  uint64_t id;
  float x, y;
};
void Fingers(const Touch* touches, int count, float aspect);
// A tap that went down and up between two polls.
void Tap(float x, float y, float aspect);
// The A the carousel gives the game (a chosen card), checked each pad read.
bool WantA();

// The game's 1280x720 point on the window (fractions of it), as the picture is fitted (nier_screen_fit).
void GameToScreen(float gx, float gy, float aspect, float& fx, float& fy);

// Draws the cards (ImGui, foreground); made by NierApp::OnCreateDialogs when Enabled().
std::unique_ptr<rex::ui::ImGuiDialog> MakeDialog(rex::ui::ImGuiDrawer* drawer);

}  // namespace nier::title_carousel
