#pragma once
// Overlay content: main window with tabs (Conexão, Jogadores & Ping,
// Diagnóstico, Chat) and the toast notifications shown even when closed.
// Called only from the render thread inside an ImGui frame.

namespace cts4p::panels {

void on_overlay_ready();
void draw_main_window(bool* open);
void draw_toasts();
bool toasts_active();

}  // namespace cts4p::panels
