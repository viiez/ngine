module;

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

module ngine.editor_ui;

import ngine.core;

namespace ngine::editor_ui {

namespace {

bool layout_reset_requested = false;

// ImGui braucht Bezeichnungen mit Nullbyte am Ende.
std::string zero_terminated(std::string_view text) {
    return std::string{text};
}

ImVec4 to_imgui(UiColor color) {
    return {color.r, color.g, color.b, color.a};
}

void dock_all(const std::vector<std::string>& windows, ImGuiID node) {
    for (const std::string& window : windows) {
        ImGui::DockBuilderDockWindow(window.c_str(), node);
    }
}

void build_layout(ImGuiID dockspace, const ImGuiViewport& viewport, const DockLayout& layout) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, viewport.WorkSize);
    ImGuiID center = dockspace;
    // Erst unten über die ganze Breite, dann links und rechts neben der Mitte.
    if (!layout.bottom.empty()) {
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,
                                                           layout.bottom_ratio, nullptr, &center);
        dock_all(layout.bottom, bottom);
    }
    if (!layout.left.empty()) {
        const ImGuiID left =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, layout.left_ratio, nullptr, &center);
        dock_all(layout.left, left);
    }
    if (!layout.right.empty()) {
        // Der Anteil bezieht sich auf das, was nach links übrig ist.
        const f32 ratio = std::clamp(layout.right_ratio / std::max(1.0f - layout.left_ratio, 0.01f),
                                     0.05f, 0.95f);
        const ImGuiID right =
            ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, ratio, nullptr, &center);
        dock_all(layout.right, right);
    }
    dock_all(layout.center, center);
    ImGui::DockBuilderFinish(dockspace);
}

} // namespace

void dockspace(const DockLayout& layout) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID id = ImGui::GetID("ngine.dockspace");
    if (layout_reset_requested || ImGui::DockBuilderGetNode(id) == nullptr) {
        build_layout(id, *viewport, layout);
        layout_reset_requested = false;
    }
    ImGui::DockSpaceOverViewport(id, viewport);
}

void reset_layout() {
    layout_reset_requested = true;
}

bool begin_window(std::string_view title, bool* open, WindowFlags flags) {
    ImGuiWindowFlags imgui_flags = ImGuiWindowFlags_None;
    if (has(flags, WindowFlags::NoScrollbar)) {
        imgui_flags |= ImGuiWindowFlags_NoScrollbar;
    }
    if (has(flags, WindowFlags::NoScrollWithMouse)) {
        imgui_flags |= ImGuiWindowFlags_NoScrollWithMouse;
    }
    const bool no_padding = has(flags, WindowFlags::NoPadding);
    if (no_padding) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
    }
    const bool visible = ImGui::Begin(zero_terminated(title).c_str(), open, imgui_flags);
    if (no_padding) {
        ImGui::PopStyleVar();
    }
    return visible;
}

void end_window() {
    ImGui::End();
}

bool begin_child(std::string_view id, Size size, bool border) {
    return ImGui::BeginChild(zero_terminated(id).c_str(), ImVec2{size.width, size.height},
                             border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None);
}

void end_child() {
    ImGui::EndChild();
}

bool begin_main_menu_bar() {
    return ImGui::BeginMainMenuBar();
}

void end_main_menu_bar() {
    ImGui::EndMainMenuBar();
}

bool begin_menu(std::string_view label) {
    return ImGui::BeginMenu(zero_terminated(label).c_str());
}

void end_menu() {
    ImGui::EndMenu();
}

bool menu_item(std::string_view label, std::string_view shortcut, bool* selected, bool enabled) {
    const std::string shortcut_text{shortcut};
    return ImGui::MenuItem(zero_terminated(label).c_str(),
                           shortcut.empty() ? nullptr : shortcut_text.c_str(), selected, enabled);
}

void text(std::string_view text) {
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void text_colored(UiColor color, std::string_view text) {
    ImGui::PushStyleColor(ImGuiCol_Text, to_imgui(color));
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

void text_disabled(std::string_view text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

void text_wrapped(std::string_view text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

void separator() {
    ImGui::Separator();
}

void same_line(f32 spacing) {
    ImGui::SameLine(0.0f, spacing);
}

bool button(std::string_view label) {
    return ImGui::Button(zero_terminated(label).c_str());
}

bool checkbox(std::string_view label, bool& value) {
    return ImGui::Checkbox(zero_terminated(label).c_str(), &value);
}

bool input_text(std::string_view label, std::string& value) {
    // ImGui schreibt in einen eigenen Puffer; wächst der Text, vergrößert der Rückruf den String.
    const auto resize = [](ImGuiInputTextCallbackData* data) -> int {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* string = static_cast<std::string*>(data->UserData);
            string->resize(static_cast<usize>(data->BufTextLen));
            data->Buf = string->data();
        }
        return 0;
    };
    return ImGui::InputText(zero_terminated(label).c_str(), value.data(), value.capacity() + 1,
                            ImGuiInputTextFlags_CallbackResize, resize, &value);
}

void set_next_item_width(f32 width) {
    ImGui::SetNextItemWidth(width);
}

void image(TextureId texture, Size size, Point uv0, Point uv1) {
    ImGui::Image(ImTextureRef{texture}, ImVec2{size.width, size.height}, ImVec2{uv0.x, uv0.y},
                 ImVec2{uv1.x, uv1.y});
}

Size content_region_available() {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    return {available.x, available.y};
}

Point item_min() {
    const ImVec2 min = ImGui::GetItemRectMin();
    return {min.x, min.y};
}

bool is_item_hovered() {
    return ImGui::IsItemHovered();
}

bool is_window_hovered() {
    return ImGui::IsWindowHovered();
}

bool is_window_focused() {
    return ImGui::IsWindowFocused();
}

void focus_window() {
    ImGui::SetWindowFocus();
}

f32 scroll_y() {
    return ImGui::GetScrollY();
}

f32 scroll_max_y() {
    return ImGui::GetScrollMaxY();
}

void scroll_here(f32 ratio) {
    ImGui::SetScrollHereY(ratio);
}

void clipped_rows(usize count, const std::function<void(usize first, usize last)>& rows) {
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(count));
    while (clipper.Step()) {
        rows(static_cast<usize>(clipper.DisplayStart), static_cast<usize>(clipper.DisplayEnd));
    }
    clipper.End();
}

} // namespace ngine::editor_ui
