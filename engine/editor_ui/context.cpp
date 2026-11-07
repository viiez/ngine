module;

#include <imgui.h>

#include <cfloat>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// Adapter für Dear ImGui (ADR 0009, ADR 0029): Nur diese Datei und widgets.cpp kennen ImGui.
module ngine.editor_ui;

import ngine.core;

namespace ngine::editor_ui {

static_assert(sizeof(ImDrawVert) == sizeof(UiVertex), "ImDrawVert passt nicht zu UiVertex");
static_assert(sizeof(ImTextureID) == sizeof(TextureId), "ImTextureID passt nicht zu TextureId");

// Auch widgets.cpp braucht die Zuordnung der Tasten (Modulbindung, nicht exportiert).
ImGuiKey to_imgui(Key key) {
    switch (key) {
    case Key::Tab: return ImGuiKey_Tab;
    case Key::Left: return ImGuiKey_LeftArrow;
    case Key::Right: return ImGuiKey_RightArrow;
    case Key::Up: return ImGuiKey_UpArrow;
    case Key::Down: return ImGuiKey_DownArrow;
    case Key::PageUp: return ImGuiKey_PageUp;
    case Key::PageDown: return ImGuiKey_PageDown;
    case Key::Home: return ImGuiKey_Home;
    case Key::End: return ImGuiKey_End;
    case Key::Insert: return ImGuiKey_Insert;
    case Key::Delete: return ImGuiKey_Delete;
    case Key::Backspace: return ImGuiKey_Backspace;
    case Key::Space: return ImGuiKey_Space;
    case Key::Enter: return ImGuiKey_Enter;
    case Key::Escape: return ImGuiKey_Escape;
    case Key::Ctrl: return ImGuiKey_LeftCtrl;
    case Key::Shift: return ImGuiKey_LeftShift;
    case Key::Alt: return ImGuiKey_LeftAlt;
    case Key::Super: return ImGuiKey_LeftSuper;
    case Key::A: return ImGuiKey_A;
    case Key::C: return ImGuiKey_C;
    case Key::D: return ImGuiKey_D;
    case Key::N: return ImGuiKey_N;
    case Key::O: return ImGuiKey_O;
    case Key::S: return ImGuiKey_S;
    case Key::V: return ImGuiKey_V;
    case Key::X: return ImGuiKey_X;
    case Key::Y: return ImGuiKey_Y;
    case Key::Z: return ImGuiKey_Z;
    case Key::W: return ImGuiKey_W;
    case Key::E: return ImGuiKey_E;
    case Key::R: return ImGuiKey_R;
    case Key::F2: return ImGuiKey_F2;
    case Key::F5: return ImGuiKey_F5;
    }
    return ImGuiKey_None;
}

namespace {

// Umschalttasten meldet ImGui zusätzlich als Modifikator.
ImGuiKey modifier(Key key) {
    switch (key) {
    case Key::Ctrl: return ImGuiMod_Ctrl;
    case Key::Shift: return ImGuiMod_Shift;
    case Key::Alt: return ImGuiMod_Alt;
    case Key::Super: return ImGuiMod_Super;
    default: return ImGuiKey_None;
    }
}

Cursor to_cursor(ImGuiMouseCursor cursor) {
    switch (cursor) {
    case ImGuiMouseCursor_None: return Cursor::Hidden;
    case ImGuiMouseCursor_TextInput: return Cursor::TextInput;
    case ImGuiMouseCursor_ResizeAll: return Cursor::ResizeAll;
    case ImGuiMouseCursor_ResizeNS: return Cursor::ResizeNS;
    case ImGuiMouseCursor_ResizeEW: return Cursor::ResizeEW;
    case ImGuiMouseCursor_ResizeNESW: return Cursor::ResizeNESW;
    case ImGuiMouseCursor_ResizeNWSE: return Cursor::ResizeNWSE;
    case ImGuiMouseCursor_Hand: return Cursor::Hand;
    case ImGuiMouseCursor_NotAllowed: return Cursor::NotAllowed;
    default: return Cursor::Arrow;
    }
}

} // namespace

struct Context::Impl {
    TextureHost* textures = nullptr;
    ImGuiContext* context = nullptr;
    std::string ini_path;
    std::function<std::string()> get_clipboard;
    std::function<void(const std::string&)> set_clipboard;
    // Hält den Text der Zwischenablage, solange ImGui den Zeiger benutzt.
    std::string clipboard;
    UiDrawData draw_data;
    u64 frames = 0;
    bool in_frame = false;

    // Setzt den eigenen Kontext als aktuellen; ImGui kennt nur einen globalen.
    void use() const {
        ImGui::SetCurrentContext(context);
    }

    // Für Rückrufe von ImGui; sie laufen, während der Kontext der aktuelle ist.
    static Impl& current() {
        return *static_cast<Impl*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
    }

    // Erfüllt die Wünsche von ImGui an seine Texturen (ImGuiBackendFlags_RendererHasTextures).
    // Ändert sich eine Textur, entsteht eine neue; die alte gibt der Host frei, sobald die GPU
    // fertig ist. Das ist einfacher als Teilupdates und kommt nur beim Wachsen des Atlas vor.
    void update_texture(ImTextureData& texture) {
        if (texture.Status == ImTextureStatus_WantCreate ||
            texture.Status == ImTextureStatus_WantUpdates) {
            verify(texture.Format == ImTextureFormat_RGBA32, "ImGui-Textur muss RGBA32 sein");
            const auto size =
                static_cast<usize>(texture.Width) * static_cast<usize>(texture.Height) * 4;
            const auto* pixels = static_cast<const u8*>(texture.GetPixels());
            const TextureId created = textures->create_texture(
                static_cast<u32>(texture.Width), static_cast<u32>(texture.Height), {pixels, size});
            if (created == no_texture) {
                log::error("Editor-UI: Textur {}x{} lässt sich nicht anlegen", texture.Width,
                           texture.Height);
            }
            if (texture.TexID != ImTextureID_Invalid) {
                textures->destroy_texture(texture.TexID);
            }
            texture.SetTexID(created);
            texture.SetStatus(ImTextureStatus_OK);
        } else if (texture.Status == ImTextureStatus_WantDestroy) {
            if (texture.TexID != ImTextureID_Invalid) {
                textures->destroy_texture(texture.TexID);
            }
            texture.SetTexID(ImTextureID_Invalid);
            texture.SetStatus(ImTextureStatus_Destroyed);
        }
    }

    void convert(const ImDrawData& source) {
        draw_data.clear();
        const ImVec2 scale = source.FramebufferScale;
        draw_data.width = source.DisplaySize.x * scale.x;
        draw_data.height = source.DisplaySize.y * scale.y;
        for (const ImDrawList* list : source.CmdLists) {
            const auto base = static_cast<u32>(draw_data.vertices.size());
            const auto index_base = static_cast<u32>(draw_data.indices.size());
            const usize old_size = draw_data.vertices.size();
            draw_data.vertices.resize(old_size + static_cast<usize>(list->VtxBuffer.Size));
            std::memcpy(draw_data.vertices.data() + old_size, list->VtxBuffer.Data,
                        static_cast<usize>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
            draw_data.indices.reserve(draw_data.indices.size() +
                                      static_cast<usize>(list->IdxBuffer.Size));
            for (const ImDrawIdx index : list->IdxBuffer) {
                draw_data.indices.push_back(base + static_cast<u32>(index));
            }
            for (const ImDrawCmd& command : list->CmdBuffer) {
                // Eigene Rückrufe benutzt die Engine nicht.
                if (command.UserCallback != nullptr || command.ElemCount == 0) {
                    continue;
                }
                const UiRect clip{(command.ClipRect.x - source.DisplayPos.x) * scale.x,
                                  (command.ClipRect.y - source.DisplayPos.y) * scale.y,
                                  (command.ClipRect.z - source.DisplayPos.x) * scale.x,
                                  (command.ClipRect.w - source.DisplayPos.y) * scale.y};
                if (clip.max_x <= clip.min_x || clip.max_y <= clip.min_y) {
                    continue;
                }
                // VtxOffset ist ohne ImGuiBackendFlags_RendererHasVtxOffset immer 0; die Indizes
                // sind oben schon auf den gemeinsamen Puffer verschoben.
                draw_data.commands.push_back({.clip = clip,
                                              .texture = command.GetTexID(),
                                              .first_index = index_base + command.IdxOffset,
                                              .index_count = command.ElemCount});
            }
        }
    }
};

Context::Context(TextureHost& textures, const ContextDesc& desc) : impl_{std::make_unique<Impl>()} {
    IMGUI_CHECKVERSION();
    impl_->textures = &textures;
    impl_->ini_path = desc.ini_path;
    impl_->get_clipboard = desc.get_clipboard;
    impl_->set_clipboard = desc.set_clipboard;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    impl_->context = ImGui::CreateContext();
    impl_->use();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_HasMouseCursors;
    io.BackendPlatformName = "ngine_platform";
    io.BackendRendererName = "ngine_editor_ui";
    io.IniFilename = impl_->ini_path.empty() ? nullptr : impl_->ini_path.c_str();
    // Fenster nur an der Titelleiste verschieben: Ziehen im Viewport gehört der Kamera.
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = impl_.get();
    if (impl_->get_clipboard && impl_->set_clipboard) {
        platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
            Impl& impl = Impl::current();
            impl.clipboard = impl.get_clipboard();
            return impl.clipboard.c_str();
        };
        platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
            Impl::current().set_clipboard(text != nullptr ? std::string{text} : std::string{});
        };
    }

    ImGui::StyleColorsDark();
    const f32 scale = desc.scale > 0.0f ? desc.scale : 1.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    // Die eingebaute Vektorschrift bleibt bei jeder Skalierung scharf.
    io.Fonts->AddFontDefaultVector();

    ImGui::SetCurrentContext(previous);
}

Context::~Context() {
    impl_->use();
    // Texturen, die nur dieser Kontext benutzt, an den Host zurückgeben (wie die Backends von
    // ImGui beim Herunterfahren).
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        if (texture->RefCount == 1 && texture->TexID != ImTextureID_Invalid) {
            impl_->textures->destroy_texture(texture->TexID);
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    ImGui::DestroyContext(impl_->context);
}

void Context::add_mouse_position(f32 x, f32 y) {
    impl_->use();
    ImGui::GetIO().AddMousePosEvent(x, y);
}

void Context::add_mouse_left_window() {
    impl_->use();
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

void Context::add_mouse_button(MouseButton button, bool down) {
    impl_->use();
    const int index = button == MouseButton::Left    ? ImGuiMouseButton_Left
                      : button == MouseButton::Right ? ImGuiMouseButton_Right
                                                     : ImGuiMouseButton_Middle;
    ImGui::GetIO().AddMouseButtonEvent(index, down);
}

void Context::add_mouse_wheel(f32 delta_y, f32 delta_x) {
    impl_->use();
    // ImGui erwartet positives x nach links.
    ImGui::GetIO().AddMouseWheelEvent(-delta_x, delta_y);
}

void Context::add_key(Key key, bool down) {
    impl_->use();
    ImGuiIO& io = ImGui::GetIO();
    if (const ImGuiKey mod = modifier(key); mod != ImGuiKey_None) {
        io.AddKeyEvent(mod, down);
    }
    io.AddKeyEvent(to_imgui(key), down);
}

void Context::add_text(std::string_view utf8) {
    impl_->use();
    const std::string text{utf8};
    ImGui::GetIO().AddInputCharactersUTF8(text.c_str());
}

void Context::add_focus(bool focused) {
    impl_->use();
    ImGui::GetIO().AddFocusEvent(focused);
}

void Context::begin_frame(f32 width, f32 height, f32 dt) {
    verify(!impl_->in_frame, "Context::begin_frame zweimal ohne end_frame");
    impl_->use();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2{width > 0.0f ? width : 1.0f, height > 0.0f ? height : 1.0f};
    io.DisplayFramebufferScale = ImVec2{1.0f, 1.0f};
    // ImGui verlangt eine positive Zeit.
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 1000.0f;
    ImGui::NewFrame();
    impl_->in_frame = true;
}

const UiDrawData& Context::end_frame() {
    verify(impl_->in_frame, "Context::end_frame ohne begin_frame");
    impl_->use();
    ImGui::Render();
    impl_->in_frame = false;
    ++impl_->frames;
    const ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data->Textures != nullptr) {
        for (ImTextureData* texture : *draw_data->Textures) {
            impl_->update_texture(*texture);
        }
    }
    impl_->convert(*draw_data);
    return impl_->draw_data;
}

bool Context::wants_mouse() const {
    impl_->use();
    return ImGui::GetIO().WantCaptureMouse;
}

bool Context::wants_keyboard() const {
    impl_->use();
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool Context::wants_text_input() const {
    impl_->use();
    return ImGui::GetIO().WantTextInput;
}

Cursor Context::cursor() const {
    impl_->use();
    return to_cursor(ImGui::GetMouseCursor());
}

void Context::save_layout() {
    impl_->use();
    if (!impl_->ini_path.empty()) {
        ImGui::SaveIniSettingsToDisk(impl_->ini_path.c_str());
    }
}

u64 Context::frame_count() const noexcept {
    return impl_->frames;
}

} // namespace ngine::editor_ui
