#pragma once
// Visual identity of the overlay: palette, fonts (Segoe UI + Segoe MDL2 icons,
// both shipped with Windows 10/11) and small reusable widgets.
//
// Principles (game HUD guidelines): one accent colour (the plumbob green the
// game itself uses in Default.ini: 0.43, 0.78, 0.12), high contrast, show only
// what the player needs and keep technical details behind "Avançado".

#include <imgui.h>

namespace cts4p::theme {

// Palette
inline constexpr ImVec4 kAccent{0.43f, 0.78f, 0.12f, 1.0f};       // plumbob green #6EC71F
inline constexpr ImVec4 kAccentDim{0.28f, 0.52f, 0.08f, 1.0f};
inline constexpr ImVec4 kText{0.93f, 0.95f, 0.97f, 1.0f};
inline constexpr ImVec4 kMuted{0.60f, 0.65f, 0.72f, 1.0f};
inline constexpr ImVec4 kWarn{0.98f, 0.74f, 0.30f, 1.0f};
inline constexpr ImVec4 kError{1.00f, 0.45f, 0.42f, 1.0f};
inline constexpr ImVec4 kInfo{0.30f, 0.67f, 0.96f, 1.0f};          // the game's UI blue
inline constexpr ImVec4 kWindow{0.07f, 0.09f, 0.12f, 0.96f};
inline constexpr ImVec4 kCard{0.12f, 0.15f, 0.20f, 1.0f};
inline constexpr ImVec4 kCardHover{0.15f, 0.19f, 0.25f, 1.0f};

// Icons (Segoe MDL2 Assets code points)
inline constexpr const char* kIconLink = "\xEE\x9C\x9B";       // U+E71B
inline constexpr const char* kIconPeople = "\xEE\x9C\x96";     // U+E716
inline constexpr const char* kIconBuild = "\xEE\xA4\x8F";      // U+E90F (repair)
inline constexpr const char* kIconChat = "\xEE\xA2\xBD";       // U+E8BD
inline constexpr const char* kIconSettings = "\xEE\x9C\x93";   // U+E713
inline constexpr const char* kIconCopy = "\xEE\xA3\x88";       // U+E8C8
inline constexpr const char* kIconCheck = "\xEE\x9C\xBE";      // U+E73E
inline constexpr const char* kIconWarning = "\xEE\x9E\xBA";    // U+E7BA
inline constexpr const char* kIconClose = "\xEE\x9C\x91";      // U+E711
inline constexpr const char* kIconAdd = "\xEE\x9C\x90";        // U+E710
inline constexpr const char* kIconEnter = "\xEE\xA2\xAD";      // U+E8AD (sign in)
inline constexpr const char* kIconLeave = "\xEE\xA2\xAC";      // U+E8AC (sign out)
inline constexpr const char* kIconSave = "\xEE\x9D\x8E";       // U+E74E
inline constexpr const char* kIconSend = "\xEE\x9C\xA5";       // U+E725
inline constexpr const char* kIconPlay = "\xEE\x9D\xA8";       // U+E768
inline constexpr const char* kIconHome = "\xEE\xA0\x8F";       // U+E80F
inline constexpr const char* kIconInfo = "\xEE\xA5\x86";       // U+E946
inline constexpr const char* kIconSync = "\xEE\xA2\x95";       // U+E895

// Loads fonts and applies the style. Call once after ImGui::CreateContext().
void apply();

ImFont* body();
ImFont* strong();  // semibold, same size as body
float title_size();
float big_size();

// Widgets
void pill(const char* text, ImVec4 color);                 // small rounded status badge
bool begin_card(const char* id, ImVec2 size);              // rounded panel; pair with end_card()
void end_card();
bool primary_button(const char* label, ImVec2 size = {0, 0});  // filled green
bool ghost_button(const char* label, ImVec2 size = {0, 0});    // subtle, for secondary actions
void heading(const char* icon, const char* text);
void muted(const char* fmt, ...);
void help(const char* text);                               // (?) with tooltip
ImVec4 ping_color(double ms);
void plumbob(float size);  // small green diamond drawn inline (the Sims "plumbob")

}  // namespace cts4p::theme
