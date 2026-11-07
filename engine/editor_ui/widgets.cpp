module;

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

module ngine.editor_ui;

import ngine.core;

namespace ngine::editor_ui {

// Definiert in context.cpp.
ImGuiKey to_imgui(Key key);

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

bool input_text(std::string_view label, std::string& value, bool enter_returns) {
    // ImGui schreibt in einen eigenen Puffer; wächst der Text, vergrößert der Rückruf den String.
    const auto resize = [](ImGuiInputTextCallbackData* data) -> int {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* string = static_cast<std::string*>(data->UserData);
            string->resize(static_cast<usize>(data->BufTextLen));
            data->Buf = string->data();
        }
        return 0;
    };
    const ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_CallbackResize |
        (enter_returns ? ImGuiInputTextFlags_EnterReturnsTrue : ImGuiInputTextFlags_None);
    return ImGui::InputText(zero_terminated(label).c_str(), value.data(), value.capacity() + 1,
                            flags, resize, &value);
}

namespace {

ImGuiDataType to_imgui(DataType type) {
    switch (type) {
    case DataType::I8: return ImGuiDataType_S8;
    case DataType::I16: return ImGuiDataType_S16;
    case DataType::I32: return ImGuiDataType_S32;
    case DataType::I64: return ImGuiDataType_S64;
    case DataType::U8: return ImGuiDataType_U8;
    case DataType::U16: return ImGuiDataType_U16;
    case DataType::U32: return ImGuiDataType_U32;
    case DataType::U64: return ImGuiDataType_U64;
    case DataType::F32: return ImGuiDataType_Float;
    case DataType::F64: return ImGuiDataType_Double;
    }
    return ImGuiDataType_Float;
}

// Grenze als Wert der Art type (ImGui erwartet Zeiger auf denselben Typ wie die Daten).
union Bound {
    i8 i8_value;
    i16 i16_value;
    i32 i32_value;
    i64 i64_value;
    u8 u8_value;
    u16 u16_value;
    u32 u32_value;
    u64 u64_value;
    f32 f32_value;
    f64 f64_value;
};

Bound to_bound(DataType type, f64 value) {
    Bound bound{};
    switch (type) {
    case DataType::I8: bound.i8_value = static_cast<i8>(value); break;
    case DataType::I16: bound.i16_value = static_cast<i16>(value); break;
    case DataType::I32: bound.i32_value = static_cast<i32>(value); break;
    case DataType::I64: bound.i64_value = static_cast<i64>(value); break;
    case DataType::U8: bound.u8_value = static_cast<u8>(value); break;
    case DataType::U16: bound.u16_value = static_cast<u16>(value); break;
    case DataType::U32: bound.u32_value = static_cast<u32>(value); break;
    case DataType::U64: bound.u64_value = static_cast<u64>(value); break;
    case DataType::F32: bound.f32_value = static_cast<f32>(value); break;
    case DataType::F64: bound.f64_value = value; break;
    }
    return bound;
}

ImGuiMouseButton to_imgui(MouseButton button) {
    switch (button) {
    case MouseButton::Left: return ImGuiMouseButton_Left;
    case MouseButton::Right: return ImGuiMouseButton_Right;
    case MouseButton::Middle: return ImGuiMouseButton_Middle;
    }
    return ImGuiMouseButton_Left;
}

} // namespace

bool drag_scalars(std::string_view label, DataType type, void* data, u32 count, f32 speed,
                  const f64* min, const f64* max, std::string_view format) {
    const ImGuiDataType imgui_type = to_imgui(type);
    const Bound low = min != nullptr ? to_bound(type, *min) : Bound{};
    const Bound high = max != nullptr ? to_bound(type, *max) : Bound{};
    const std::string format_text{format};
    // Mit Grenzen klemmt ImGui auch eingetippte Werte.
    return ImGui::DragScalarN(
        zero_terminated(label).c_str(), imgui_type, data, static_cast<int>(count), speed,
        min != nullptr ? &low : nullptr, max != nullptr ? &high : nullptr,
        format.empty() ? nullptr : format_text.c_str(),
        min != nullptr && max != nullptr ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None);
}

bool color_edit(std::string_view label, f32* color, u32 count) {
    // Werte sind linear; ImGui zeigt sie trotzdem als Zahlen 0 bis 1 und als Farbe an.
    const ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
    return count >= 4 ? ImGui::ColorEdit4(zero_terminated(label).c_str(), color, flags)
                      : ImGui::ColorEdit3(zero_terminated(label).c_str(), color, flags);
}

bool combo(std::string_view label, usize& current, std::span<const std::string> items) {
    const std::string preview = current < items.size() ? items[current] : std::string{};
    bool changed = false;
    if (ImGui::BeginCombo(zero_terminated(label).c_str(), preview.c_str())) {
        for (usize i = 0; i < items.size(); ++i) {
            const bool selected = i == current;
            if (ImGui::Selectable(items[i].c_str(), selected)) {
                changed = current != i;
                current = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool collapsing_header(std::string_view label, bool default_open) {
    return ImGui::CollapsingHeader(zero_terminated(label).c_str(),
                                   default_open ? ImGuiTreeNodeFlags_DefaultOpen
                                                : ImGuiTreeNodeFlags_None);
}

bool selectable(std::string_view label, bool selected) {
    return ImGui::Selectable(zero_terminated(label).c_str(), selected);
}

void indent(f32 width) {
    ImGui::Indent(width);
}

void unindent(f32 width) {
    ImGui::Unindent(width);
}

void dummy(Size size) {
    ImGui::Dummy(ImVec2{size.width, size.height});
}

bool tree_node(u64 id, std::string_view label, TreeFlags flags) {
    ImGuiTreeNodeFlags imgui_flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                     ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                     ImGuiTreeNodeFlags_SpanAvailWidth;
    if (has(flags, TreeFlags::Leaf)) {
        imgui_flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (has(flags, TreeFlags::Selected)) {
        imgui_flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (has(flags, TreeFlags::DefaultOpen)) {
        imgui_flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    const std::string text{label};
    return ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)), imgui_flags,
                             "%s", text.c_str());
}

void tree_pop() {
    ImGui::TreePop();
}

void drag_source(std::string_view type, u64 payload, std::string_view preview) {
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(zero_terminated(type).c_str(), &payload, sizeof(payload));
        ImGui::TextUnformatted(preview.data(), preview.data() + preview.size());
        ImGui::EndDragDropSource();
    }
}

std::optional<u64> drop_target(std::string_view type) {
    std::optional<u64> result;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(zero_terminated(type).c_str());
            payload != nullptr && payload->DataSize == sizeof(u64)) {
            u64 value = 0;
            std::memcpy(&value, payload->Data, sizeof(value));
            result = value;
        }
        ImGui::EndDragDropTarget();
    }
    return result;
}

void open_popup(std::string_view id) {
    ImGui::OpenPopup(zero_terminated(id).c_str());
}

bool begin_popup(std::string_view id) {
    return ImGui::BeginPopup(zero_terminated(id).c_str());
}

bool begin_popup_modal(std::string_view title, bool* open) {
    return ImGui::BeginPopupModal(zero_terminated(title).c_str(), open,
                                  ImGuiWindowFlags_AlwaysAutoResize);
}

bool begin_popup_context_item(std::string_view id) {
    return ImGui::BeginPopupContextItem(zero_terminated(id).c_str());
}

bool begin_popup_context_window(std::string_view id) {
    return ImGui::BeginPopupContextWindow(zero_terminated(id).c_str(),
                                          ImGuiPopupFlags_MouseButtonRight |
                                              ImGuiPopupFlags_NoOpenOverItems);
}

void end_popup() {
    ImGui::EndPopup();
}

void close_current_popup() {
    ImGui::CloseCurrentPopup();
}

bool is_item_clicked(MouseButton button) {
    return ImGui::IsItemClicked(to_imgui(button));
}

bool is_item_deactivated_after_edit() {
    return ImGui::IsItemDeactivatedAfterEdit();
}

bool is_item_deactivated() {
    return ImGui::IsItemDeactivated();
}

bool is_any_item_active() {
    return ImGui::IsAnyItemActive();
}

bool is_key_pressed(Key key, bool repeat) {
    return ImGui::IsKeyPressed(to_imgui(key), repeat);
}

bool is_key_down(Key key) {
    return ImGui::IsKeyDown(to_imgui(key));
}

bool is_mouse_clicked(MouseButton button) {
    return ImGui::IsMouseClicked(to_imgui(button));
}

Point mouse_position() {
    const ImVec2 position = ImGui::GetMousePos();
    return {position.x, position.y};
}

void set_keyboard_focus_here() {
    ImGui::SetKeyboardFocusHere();
}

void set_tooltip(std::string_view text) {
    const std::string copy{text};
    ImGui::SetTooltip("%s", copy.c_str());
}

void push_id(u64 id) {
    ImGui::PushID(reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)));
}

void push_id(std::string_view id) {
    ImGui::PushID(id.data(), id.data() + id.size());
}

void pop_id() {
    ImGui::PopID();
}

void begin_disabled(bool disabled) {
    ImGui::BeginDisabled(disabled);
}

void end_disabled() {
    ImGui::EndDisabled();
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
