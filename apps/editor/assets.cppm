module;

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Assets im Editor (R2.4): Asset-Browser (EDIT-09) mit Ziehen in den Viewport, Einfügen eines
// Modells als ein Undo-Schritt und Hot Reload (PLAT-06). Die Datenbank, der Ressourcen-Manager und
// der Dateibeobachter liegen in der Engine; hier steht nur, was der Editor daraus macht.
export module editor.assets;

import ngine.core;
import ngine.asset;
import ngine.edit;
import ngine.editor_ui;
import ngine.filesystem;
import ngine.math;
import ngine.reflect;
import ngine.render;
import ngine.rhi;
import ngine.shader_compiler;
import ngine.world;

namespace editor {

using namespace ngine;
namespace ui = editor_ui;

// Art der Nutzlast beim Ziehen eines Assets (Kennung als u64).
export constexpr std::string_view asset_payload = "ngine.asset";

// Sucht relative (z. B. „assets“) ab dem Arbeitsordner und ab dem Ordner der Anwendung aufwärts,
// bis zur Wurzel. marker ist eine Datei, die im gefundenen Ordner liegen muss (leer: keine).
export std::optional<std::string> find_upwards(std::string_view relative,
                                               std::string_view marker = {}) {
    std::vector<std::string> starts{filesystem::normalize(".")};
    if (auto exe = filesystem::executable_dir()) {
        starts.push_back(filesystem::normalize(*exe));
    }
    for (std::string directory : starts) {
        while (true) {
            const std::string candidate = filesystem::join(directory, relative);
            if (filesystem::is_directory(candidate) &&
                (marker.empty() || filesystem::exists(filesystem::join(candidate, marker)))) {
                return filesystem::normalize(candidate);
            }
            const std::string parent = filesystem::parent_path(directory);
            if (parent.empty() || parent == directory) {
                break;
            }
            directory = parent;
        }
    }
    return std::nullopt;
}

// Hält einen Wert eines reflektierten Typs als Komponente eines Snapshots fest.
template <typename T>
void add_component(const reflect::TypeRegistry& types, edit::EntitySnapshot::Node& node,
                   const T& value) {
    const reflect::TypeInfo& type = types.get<T>();
    node.components.push_back({.type = type.id, .bytes = edit::capture(&value, type)});
}

// Entities eines Modells als Snapshot zum Einfügen: eine Wurzel namens name an position, darunter
// die Knoten der Szene des Modells mit Name, Transform und MeshRenderer (Verweise auf model_id).
export edit::EntitySnapshot model_snapshot(edit::SceneDocument& document,
                                           const asset::Model& layout, asset::AssetId model_id,
                                           std::string name, math::DVec3 position) {
    const reflect::TypeRegistry& types = document.types();
    edit::EntitySnapshot snapshot;
    edit::EntitySnapshot::Node& root = snapshot.nodes.emplace_back();
    root.id = document.reserve_id();
    add_component(types, root, world::Name{std::move(name)});
    add_component(types, root, world::Transform{.position = position});

    // Tiefensuche, damit Eltern vor ihren Kindern stehen.
    const std::function<void(u32, edit::EntityId)> add = [&](u32 index, edit::EntityId parent) {
        if (index >= layout.nodes.size()) {
            return;
        }
        const asset::Node& source = layout.nodes[index];
        const edit::EntityId id = document.reserve_id();
        // Referenz erst nach dem Anhängen nehmen: push_back verschiebt die Knoten.
        snapshot.nodes.push_back({.id = id, .parent = parent, .components = {}});
        edit::EntitySnapshot::Node& node = snapshot.nodes.back();
        add_component(types, node, world::Name{source.name});
        add_component(types, node,
                      world::Transform{.position = math::to_f64(source.translation),
                                       .rotation = source.rotation,
                                       .scale = source.scale});
        if (source.mesh && *source.mesh < layout.meshes.size()) {
            add_component(types, node,
                          render::MeshRenderer{.mesh = {.asset = model_id, .index = *source.mesh}});
        }
        for (const u32 child : source.children) {
            add(child, id);
        }
    };
    for (const u32 node : layout.roots) {
        add(node, root.id);
    }
    return snapshot;
}

// Ein Modell aus dem Asset-Browser wird eingefügt, sobald es geladen ist: bis dahin läuft das
// Laden im Hintergrund, der Editor bleibt bedienbar.
export class ModelDrop {
public:
    void start(asset::ResourceManager& resources, const asset::AssetInfo& info,
               math::DVec3 position) {
        handle_ = resources.load<render::GpuModel>(info.id);
        name_ = filesystem::file_name(info.path);
        if (const auto dot = name_.rfind('.'); dot != std::string::npos && dot > 0) {
            name_.resize(dot);
        }
        position_ = position;
        log::info("Füge {} ein …", info.path);
    }

    [[nodiscard]] bool pending() const noexcept {
        return static_cast<bool>(handle_);
    }

    // Fügt ein, sobald das Modell da ist, und wählt die neue Wurzel aus.
    void update(edit::SceneDocument& document, edit::Selection& selection) {
        if (!handle_) {
            return;
        }
        if (handle_.state() == asset::ResourceState::Failed) {
            log::error("Einfügen gescheitert: {}", handle_.error());
            handle_ = {};
            return;
        }
        const render::GpuModel* model = handle_.get();
        if (model == nullptr || document.read_only()) {
            return;
        }
        edit::EntitySnapshot snapshot =
            model_snapshot(document, model->layout(), handle_.id(), name_, position_);
        const edit::EntityId root = snapshot.nodes.front().id;
        if (auto result = document.execute(
                edit::insert_entities(std::move(snapshot), std::format("'{}' einfügen", name_)));
            result) {
            document.history().seal();
            selection.select(root);
        } else {
            log::error("Einfügen gescheitert: {}", result.error());
        }
        handle_ = {};
    }

private:
    asset::Handle<render::GpuModel> handle_;
    std::string name_;
    math::DVec3 position_;
};

// Ort für ein Modell, das an einen Pixel des Viewports gezogen wurde: Schnitt mit dem Boden
// (y = 0), sonst einige Meter vor der Kamera.
export math::DVec3 drop_position(const edit::ViewCamera& camera, f32 x, f32 y) {
    const math::Ray ray = camera.ray(x, y);
    if (const auto t = math::intersect_plane(ray, {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
        t && *t > 0.0 && *t < 200.0) {
        return ray.at(*t);
    }
    return ray.at(5.0);
}

std::string lower(std::string_view text) {
    std::string result{text};
    std::ranges::transform(result, result.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return result;
}

// Asset-Browser (EDIT-09): alle Assets der Datenbank mit Art und Pfad, Filter nach Namen. Modelle
// lassen sich in den Viewport ziehen.
export class AssetPanel {
public:
    void draw(asset::AssetDatabase* database, asset::ResourceManager& resources) {
        if (database == nullptr) {
            ui::text_disabled("Kein Asset-Ordner (Option --assets <ordner>)");
            return;
        }
        if (ui::button("Neu einlesen")) {
            if (auto changes = database->refresh(); !changes) {
                log::error("Assets: {}", changes.error());
            }
        }
        ui::same_line();
        ui::set_next_item_width(220.0f);
        ui::input_text("Filter", filter_);
        ui::same_line();
        const asset::ResourceStats stats = resources.stats();
        ui::text_disabled(std::format("{} Assets in {} | geladen {}, lädt {}, neu geladen {}",
                                      database->assets().size(), database->root(), stats.resident,
                                      stats.loading, stats.reloads));
        ui::separator();
        if (ui::begin_child("##Liste")) {
            const std::string needle = lower(filter_);
            for (const asset::AssetInfo& info : database->assets()) {
                if (!needle.empty() && lower(info.path).find(needle) == std::string::npos) {
                    continue;
                }
                ui::push_id(info.id);
                const std::string label =
                    std::format("{:<8} {}", asset::to_string(info.type), info.path);
                if (ui::selectable(label, selected_ == info.id)) {
                    selected_ = info.id;
                }
                if (ui::is_item_hovered()) {
                    ui::set_tooltip(std::format("Kennung {}{}", info.id,
                                                info.type == asset::AssetType::Model
                                                    ? "\nIn den Viewport ziehen zum Einfügen"
                                                    : ""));
                }
                if (info.type == asset::AssetType::Model) {
                    ui::drag_source(asset_payload, info.id, info.path);
                }
                ui::pop_id();
            }
        }
        ui::end_child();
    }

private:
    std::string filter_;
    asset::AssetId selected_ = 0;
};

// Hot Reload (PLAT-06): beobachtet den Asset-Ordner und die Quellen der Shader. Geänderte
// Dateien laden ihre Ressourcen neu (Modelle samt Texturen), neue und gelöschte lesen die
// Datenbank neu ein, geänderte Shader übersetzt Slang zur Laufzeit.
export class HotReload {
public:
    HotReload(const asset::AssetDatabase* database, std::optional<std::string> shader_dir) {
        if (database != nullptr) {
            assets_.emplace(database->root());
            static_cast<void>(assets_->scan());
        }
        if (shader_dir) {
            shader_dir_ = *shader_dir;
            shaders_.emplace(*shader_dir);
            static_cast<void>(shaders_->scan());
            log::info("Hot Reload der Shader aus {}", *shader_dir);
        }
    }

    void poll(asset::AssetDatabase* database, asset::ResourceManager& resources,
              render::Renderer& renderer) {
        if (assets_ && database != nullptr) {
            const std::vector<filesystem::FileEvent> events = assets_->poll();
            bool structure = false;
            std::vector<std::string> changed;
            for (const filesystem::FileEvent& event : events) {
                log::debug("Asset-Datei geändert: {}", event.path);
                if (event.change != filesystem::FileChange::Modified ||
                    event.path.ends_with(asset::meta_extension)) {
                    structure = true;
                }
                changed.push_back(event.path);
            }
            if (structure) {
                if (auto result = database->refresh(); !result) {
                    log::error("Assets: {}", result.error());
                }
            }
            if (!changed.empty()) {
                const usize count = resources.reload_files(changed);
                if (count > 0) {
                    log::info("Hot Reload: {} Ressource(n) werden neu geladen", count);
                }
            }
        }
        if (shaders_) {
            const std::vector<filesystem::FileEvent> events = shaders_->poll();
            for (const filesystem::FileEvent& event : events) {
                log::debug("Shader-Datei geändert: {}", event.path);
            }
            const bool any = std::ranges::any_of(events, [](const filesystem::FileEvent& event) {
                return event.path.ends_with(".slang");
            });
            if (any) {
                reload_shaders(renderer);
            }
        }
    }

    // Übersetzt alle Shader des Renderers neu (auch per Menü).
    void reload_shaders(render::Renderer& renderer) {
        if (shader_dir_.empty()) {
            log::warn("Hot Reload der Shader: Quellordner unbekannt (Option --shaders <ordner>)");
            return;
        }
        if (!compiler_) {
            auto created = shader_compiler::Compiler::create();
            if (!created) {
                log::error("Shader-Compiler: {}", created.error());
                return;
            }
            compiler_ = *std::move(created);
        }
        // Die GPU darf die alten Pipelines noch benutzen; das Device gibt sie später frei.
        auto result = renderer.reload_shaders(
            [&](std::string_view file, std::string_view entry, rhi::ShaderStage stage) {
                return compiler_->compile(filesystem::join(shader_dir_, file), entry, stage);
            });
        if (result) {
            log::info("Shader neu übersetzt");
        } else {
            log::error("Shader: {}", result.error());
        }
    }

    [[nodiscard]] bool watches_shaders() const noexcept {
        return shaders_.has_value();
    }

private:
    std::optional<filesystem::FileWatcher> assets_;
    std::optional<filesystem::FileWatcher> shaders_;
    std::string shader_dir_;
    std::unique_ptr<shader_compiler::Compiler> compiler_;
};

} // namespace editor
