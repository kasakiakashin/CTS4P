#include "ui/theme.h"

#include <windows.h>

#include <cstdarg>
#include <filesystem>
#include <string>

#include "core/paths.h"

namespace cts4p::theme {
namespace {

ImFont* g_body = nullptr;
ImFont* g_strong = nullptr;
constexpr float kBodySize = 17.0f;

std::string font_path(const wchar_t* file) {
    wchar_t windir[MAX_PATH]{};
    GetWindowsDirectoryW(windir, MAX_PATH);
    const auto p = std::filesystem::path(windir) / L"Fonts" / file;
    std::error_code ec;
    return std::filesystem::exists(p, ec) ? paths::to_utf8(p.wstring()) : std::string();
}

// Loads `file` and merges the Segoe MDL2 icon glyphs into it.
ImFont* load_with_icons(const wchar_t* file, float size) {
    ImGuiIO& io = ImGui::GetIO();
    const std::string main = font_path(file);
    ImFont* font = main.empty() ? io.Fonts->AddFontDefault()
                                : io.Fonts->AddFontFromFileTTF(main.c_str(), size, nullptr, io.Fonts->GetGlyphRangesDefault());
    const std::string icons = font_path(L"segmdl2.ttf");
    if (!icons.empty()) {
        static const ImWchar kIconRanges[] = {0xE700, 0xEAFF, 0};
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.GlyphOffset = ImVec2(0, 3);  // MDL2 glyphs sit high next to Segoe UI text
        io.Fonts->AddFontFromFileTTF(icons.c_str(), size * 0.92f, &cfg, kIconRanges);
    }
    return font;
}

}  // namespace

void apply() {
    g_body = load_with_icons(L"segoeui.ttf", kBodySize);
    g_strong = load_with_icons(L"seguisb.ttf", kBodySize);

    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 12.0f;
    s.ChildRounding = 10.0f;
    s.FrameRounding = 7.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 7.0f;
    s.TabRounding = 7.0f;
    s.ScrollbarRounding = 9.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = {16, 14};
    s.FramePadding = {12, 7};
    s.ItemSpacing = {10, 9};
    s.ItemInnerSpacing = {8, 6};
    s.ScrollbarSize = 10.0f;
    s.TabBarBorderSize = 0.0f;
    s.SeparatorTextBorderSize = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = kWindow;
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = {0.10f, 0.12f, 0.16f, 0.98f};
    c[ImGuiCol_Border] = {1, 1, 1, 0.07f};
    c[ImGuiCol_FrameBg] = {0.16f, 0.19f, 0.25f, 1.0f};
    c[ImGuiCol_FrameBgHovered] = {0.20f, 0.24f, 0.31f, 1.0f};
    c[ImGuiCol_FrameBgActive] = {0.22f, 0.27f, 0.35f, 1.0f};
    c[ImGuiCol_TitleBg] = kWindow;
    c[ImGuiCol_TitleBgActive] = kWindow;
    c[ImGuiCol_Button] = {0.18f, 0.22f, 0.29f, 1.0f};
    c[ImGuiCol_ButtonHovered] = {0.23f, 0.28f, 0.37f, 1.0f};
    c[ImGuiCol_ButtonActive] = {0.27f, 0.33f, 0.43f, 1.0f};
    c[ImGuiCol_Header] = {1, 1, 1, 0.035f};
    c[ImGuiCol_HeaderHovered] = {1, 1, 1, 0.07f};
    c[ImGuiCol_HeaderActive] = {1, 1, 1, 0.10f};
    c[ImGuiCol_ResizeGrip] = {1, 1, 1, 0.05f};
    c[ImGuiCol_ResizeGripHovered] = {1, 1, 1, 0.15f};
    c[ImGuiCol_ResizeGripActive] = kAccent;
    c[ImGuiCol_Tab] = {0, 0, 0, 0};
    c[ImGuiCol_TabHovered] = {0.18f, 0.22f, 0.29f, 1.0f};
    c[ImGuiCol_TabSelected] = {0.16f, 0.20f, 0.26f, 1.0f};
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = {0, 0, 0, 0};
    c[ImGuiCol_TabDimmedSelected] = {0.16f, 0.20f, 0.26f, 1.0f};
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_Separator] = {1, 1, 1, 0.08f};
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = {1, 1, 1, 0.15f};
    c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt] = {1, 1, 1, 0.03f};
    c[ImGuiCol_TextSelectedBg] = {kAccent.x, kAccent.y, kAccent.z, 0.35f};
    c[ImGuiCol_NavCursor] = kAccent;
}

ImFont* body() { return g_body; }
ImFont* strong() { return g_strong; }
float title_size() { return 22.0f; }
float big_size() { return 40.0f; }

void pill(const char* text, ImVec4 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pad{10, 3};
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size{ts.x + pad.x * 2 + 12, ts.y + pad.y * 2};
    dl->AddRectFilled(p, {p.x + size.x, p.y + size.y}, ImGui::GetColorU32({color.x, color.y, color.z, 0.18f}), size.y * 0.5f);
    dl->AddCircleFilled({p.x + pad.x + 3, p.y + size.y * 0.5f}, 3.5f, ImGui::GetColorU32(color));
    dl->AddText({p.x + pad.x + 12, p.y + pad.y}, ImGui::GetColorU32(color), text);
    ImGui::Dummy(size);
}

bool begin_card(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14, 12});
    return ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding | (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0),
                             ImGuiWindowFlags_NoScrollbar);
}

void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

bool primary_button(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.36f, 0.66f, 0.10f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
    ImGui::PushFont(g_strong, 0.0f);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
    return pressed;
}

bool ghost_button(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, {1, 1, 1, 0.04f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.09f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.13f});
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

void heading(const char* icon, const char* text) {
    ImGui::PushFont(g_strong, title_size() * 0.82f);
    ImGui::TextColored(kAccent, "%s", icon);
    ImGui::SameLine(0, 8);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void muted(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%s", kIconInfo);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void plumbob(float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const float cx = p.x + size * 0.5f, cy = p.y + h * 0.5f;
    const float rx = size * 0.38f, ry = size * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddQuadFilled({cx, cy - ry}, {cx + rx, cy}, {cx, cy + ry}, {cx - rx, cy}, ImGui::GetColorU32(kAccent));
    dl->AddTriangleFilled({cx, cy - ry}, {cx + rx, cy}, {cx, cy}, ImGui::GetColorU32({1, 1, 1, 0.25f}));  // facet highlight
    ImGui::Dummy({size, h});
}

ImVec4 ping_color(double ms) {
    if (ms < 0) return kMuted;
    if (ms < 80) return kAccent;
    if (ms < 180) return kWarn;
    return kError;
}

}  // namespace cts4p::theme
