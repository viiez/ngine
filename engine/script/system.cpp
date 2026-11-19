module;

#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

module ngine.script;

import ngine.core;
import ngine.filesystem;
import ngine.input;
import ngine.physics;
import ngine.platform;
import ngine.reflect;
import ngine.world;
import ngine.audio;
import ngine.animation;

namespace ngine::script {

namespace {

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(first, last - first + 1)};
}

std::optional<u32> number(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    u32 value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<u32>(c - '0');
    }
    return value;
}

std::optional<Diagnostic> parse_line(std::string_view line) {
    // Datei(Zeile,Spalte): error CS1002: Text [Projekt]
    const std::string_view markers[] = {": error ", ": warning "};
    for (const std::string_view marker : markers) {
        const auto at = line.find(marker);
        if (at == std::string_view::npos) {
            continue;
        }
        Diagnostic d;
        d.error = marker == ": error ";
        std::string_view location = line.substr(0, at);
        if (location.ends_with(')')) {
            const auto open = location.rfind('(');
            if (open != std::string_view::npos) {
                const std::string_view inside =
                    location.substr(open + 1, location.size() - open - 2);
                const auto comma = inside.find(',');
                const auto line_number = number(inside.substr(0, comma));
                if (line_number) {
                    d.line = *line_number;
                    if (comma != std::string_view::npos) {
                        d.column = number(inside.substr(comma + 1)).value_or(0);
                    }
                    location = location.substr(0, open);
                }
            }
        }
        d.file = trim(location);
        std::string_view rest = line.substr(at + marker.size());
        const auto colon = rest.find(": ");
        if (colon != std::string_view::npos &&
            rest.substr(0, colon).find(' ') == std::string_view::npos) {
            d.code = std::string{rest.substr(0, colon)};
            rest = rest.substr(colon + 2);
        }
        // Projekt in eckigen Klammern am Ende weg.
        if (const std::string trimmed = trim(rest); trimmed.ends_with(']')) {
            const auto open = trimmed.rfind(" [");
            d.message = open != std::string::npos ? trimmed.substr(0, open) : trimmed;
        } else {
            d.message = trimmed;
        }
        return d;
    }
    return std::nullopt;
}

f64 seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
}

std::string assembly_of(const ScriptBuildDesc& desc) {
    std::string name = filesystem::file_name(desc.project);
    const std::string extension = filesystem::extension(name);
    name.resize(name.size() - extension.size());
    return filesystem::join(desc.output_dir, name + ".dll");
}

// Für Entwicklungsumgebungen, die das Projekt ohne die Engine bauen.
void write_props(const ScriptBuildDesc& desc) {
    const std::string text =
        std::format("<!-- Erzeugt von NGine (ScriptBuilder). Wo NGine.Scripting.dll liegt. -->\n"
                    "<Project>\n  <PropertyGroup>\n"
                    "    <NgineScriptingDir Condition=\"'$(NgineScriptingDir)' == "
                    "''\">{}/</NgineScriptingDir>\n"
                    "    <NgineBindings Condition=\"'$(NgineBindings)' == ''\">{}</NgineBindings>\n"
                    "  </PropertyGroup>\n</Project>\n",
                    desc.scripting_dir, desc.bindings);
    const std::string path = filesystem::join(desc.output_dir, "ngine.props");
    if (auto current = filesystem::read_text(path); current && *current == text) {
        return;
    }
    static_cast<void>(filesystem::write_text(path, text));
}

BuildResult run_build(const ScriptBuildDesc& desc) {
    const auto start = std::chrono::steady_clock::now();
    BuildResult result;
    if (auto created = filesystem::create_directories(desc.output_dir); !created) {
        result.output = created.error();
        return result;
    }
    write_props(desc);
    const std::vector<std::string> arguments{
        "build",
        desc.project,
        "--nologo",
        "--verbosity",
        "quiet",
        "--configuration",
        "Debug",
        "--output",
        desc.output_dir,
        "--artifacts-path",
        filesystem::join(desc.output_dir, "artifacts"),
        // Mit Schrägstrich am Ende: dotnet verschluckt Argumente, die auf „dotnet“ enden.
        "-p:NgineScriptingDir=" + desc.scripting_dir + "/",
        "-p:NgineBindings=" + desc.bindings,
        "-clp:NoSummary",
    };
    auto ran = platform::run_process(desc.dotnet, arguments);
    result.seconds = seconds_since(start);
    if (!ran) {
        result.output = std::move(ran.error());
        result.sdk_missing = true;
        return result;
    }
    result.output = std::move(ran->output);
    result.diagnostics = parse_diagnostics(result.output);
    const std::string assembly = assembly_of(desc);
    result.ok = ran->exit_code == 0 && filesystem::exists(assembly);
    if (result.ok) {
        result.assembly = assembly;
    }
    return result;
}

} // namespace

std::vector<Diagnostic> parse_diagnostics(std::string_view output) {
    std::vector<Diagnostic> result;
    std::set<std::tuple<std::string, u32, u32, std::string, std::string>> seen;
    usize begin = 0;
    while (begin < output.size()) {
        usize end = output.find('\n', begin);
        if (end == std::string_view::npos) {
            end = output.size();
        }
        if (auto d = parse_line(output.substr(begin, end - begin))) {
            if (seen.emplace(d->file, d->line, d->column, d->code, d->message).second) {
                result.push_back(std::move(*d));
            }
        }
        begin = end + 1;
    }
    return result;
}

// --- ScriptBuilder ---------------------------------------------------------------------------

struct ScriptBuilder::Impl {
    ScriptBuildDesc desc;
    mutable std::mutex mutex;
    std::thread thread;
    bool running = false;
    bool again = false;
    std::optional<BuildResult> done;

    void loop() {
        for (;;) {
            BuildResult result = run_build(desc);
            const std::scoped_lock lock{mutex};
            if (again) {
                again = false;
                continue;
            }
            done = std::move(result);
            running = false;
            return;
        }
    }
};

ScriptBuilder::ScriptBuilder(ScriptBuildDesc desc) : impl_{std::make_unique<Impl>()} {
    impl_->desc = std::move(desc);
}

ScriptBuilder::~ScriptBuilder() {
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

void ScriptBuilder::start() {
    std::unique_lock lock{impl_->mutex};
    if (impl_->running) {
        impl_->again = true;
        return;
    }
    impl_->running = true;
    lock.unlock();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->thread = std::thread{[impl = impl_.get()] { impl->loop(); }};
}

bool ScriptBuilder::busy() const {
    const std::scoped_lock lock{impl_->mutex};
    return impl_->running;
}

std::optional<BuildResult> ScriptBuilder::poll() {
    const std::scoped_lock lock{impl_->mutex};
    return std::exchange(impl_->done, std::nullopt);
}

BuildResult ScriptBuilder::build() {
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    {
        const std::scoped_lock lock{impl_->mutex};
        impl_->done.reset();
    }
    return run_build(impl_->desc);
}

std::string ScriptBuilder::assembly_path() const {
    return assembly_of(impl_->desc);
}

// --- ScriptSystem ----------------------------------------------------------------------------

struct ScriptSystem::Impl {
    ScriptRuntime* runtime = nullptr;
    reflect::TypeRegistry* types = nullptr;
    ScriptSystemDesc desc;
    std::optional<ScriptBuilder> builder;
    std::optional<filesystem::FileWatcher> watcher;
    world::World* world = nullptr;
    ScriptStatus status;
    bool active = true;
    bool sdk_missing = false;
    // Fehler beim Ausführen nur einmal pro geladener Assembly melden.
    u32 reported_generation = 0;

    void handle(BuildResult result) {
        for (const Diagnostic& d : result.diagnostics) {
            if (d.error) {
                log::error("{}({},{}): {} {}", d.file, d.line, d.column, d.code, d.message);
            } else {
                log::warn("{}({},{}): {} {}", d.file, d.line, d.column, d.code, d.message);
            }
        }
        if (result.ok) {
            log::info("Skripte in {:.1f} s gebaut", result.seconds);
        } else if (result.sdk_missing) {
            sdk_missing = true;
            log::warn("Skripte: .NET-SDK fehlt ({}); es läuft die letzte gebaute Assembly",
                      result.output);
        } else {
            log::error("Skripte: Bau gescheitert, die bisherigen laufen weiter");
            if (result.diagnostics.empty()) {
                log::error("{}", trim(result.output));
            }
        }
        const std::string assembly = result.ok ? result.assembly : std::string{};
        status.last_build = std::move(result);
        if (!assembly.empty()) {
            static_cast<void>(reload(assembly));
        }
    }

    bool reload(std::string_view assembly) {
        if (runtime->scripts_loaded()) {
            runtime->store_fields();
            const auto unloaded = runtime->unload_scripts();
            if (!unloaded) {
                log::error("Skripte entladen: {}", unloaded.error());
            } else if (!*unloaded) {
                log::warn("Der alte Skript-Kontext ist noch nicht frei (statische Felder oder "
                          "Ereignisse, die auf Skripte zeigen?)");
            }
            status.assembly.clear();
            status.classes.clear();
        }
        const auto loaded = runtime->load_scripts(assembly);
        if (!loaded) {
            log::error("Skripte laden: {}", loaded.error());
            return false;
        }
        status.assembly = std::string{assembly};
        ++status.generation;
        for (const reflect::TypeInfo* type : runtime->script_types()) {
            status.classes.push_back(type->name);
        }
        if (!active) {
            runtime->validate();
        }
        return true;
    }

    // Komponenten, die NGine.Scripting nicht mitbringt (Anwendung), als C# für das Projekt.
    void write_bindings() {
        const std::string path = filesystem::join(desc.cache_dir, "Komponenten.g.cs");
        BindingOptions options{.name_space = "NGine",
                               .base = nullptr,
                               .skip = runtime->bound_types(),
                               .layout_report = false};
        const std::string text = generate_bindings(*types, options);
        if (auto current = filesystem::read_text(path); current && *current == text) {
            return;
        }
        static_cast<void>(filesystem::create_directories(desc.cache_dir));
        if (auto written = filesystem::write_text(path, text); !written) {
            log::error("Skripte: {}", written.error());
        }
    }

    void run(ScriptPhase phase, f32 delta) {
        if (!active || world == nullptr || !runtime->scripts_loaded()) {
            return;
        }
        if (auto ran = runtime->run(phase, delta);
            !ran && reported_generation != status.generation) {
            reported_generation = status.generation;
            log::error("Skripte: {}", ran.error());
        }
    }
};

ScriptSystem::ScriptSystem(ScriptRuntime& runtime, reflect::TypeRegistry& types,
                           ScriptSystemDesc desc)
    : impl_{std::make_unique<Impl>()} {
    impl_->runtime = &runtime;
    impl_->types = &types;
    impl_->desc = std::move(desc);
    runtime.use_types(&types);
    if (!impl_->desc.project.empty()) {
        impl_->builder.emplace(
            ScriptBuildDesc{.project = impl_->desc.project,
                            .output_dir = impl_->desc.cache_dir,
                            .scripting_dir = runtime.assembly_dir(),
                            .bindings = filesystem::join(impl_->desc.cache_dir, "Komponenten.g.cs"),
                            .dotnet = impl_->desc.dotnet});
    }
}

ScriptSystem::~ScriptSystem() {
    impl_->builder.reset(); // wartet auf einen laufenden Bau
    bind(nullptr);
    if (impl_->runtime->scripts_loaded()) {
        static_cast<void>(impl_->runtime->unload_scripts());
    }
    impl_->runtime->use_types(nullptr);
}

void ScriptSystem::start(bool wait) {
    if (!impl_->builder) {
        return;
    }
    // Die Anordnung der Structs in NGine.Scripting muss zur Engine passen (gleicher Build).
    if (auto verified = impl_->runtime->verify_bindings(*impl_->types); !verified) {
        log::error("Skripte: {}", verified.error());
        return;
    }
    impl_->write_bindings();
    const std::string cached = impl_->builder->assembly_path();
    if (impl_->desc.build && wait) {
        impl_->handle(impl_->builder->build());
        if (impl_->status.generation == 0 && filesystem::exists(cached)) {
            static_cast<void>(reload(cached));
        }
    } else {
        if (filesystem::exists(cached)) {
            static_cast<void>(reload(cached));
        } else if (!impl_->desc.build) {
            log::warn("Skripte: keine gebaute Assembly im Cache ({}), es laufen keine Skripte",
                      cached);
        }
        if (impl_->desc.build) {
            impl_->builder->start();
            impl_->status.building = true;
        }
    }
    if (impl_->desc.build && impl_->desc.watch) {
        impl_->watcher.emplace(filesystem::parent_path(impl_->desc.project));
        static_cast<void>(impl_->watcher->scan());
    }
}

void ScriptSystem::update() {
    if (!impl_->builder) {
        return;
    }
    if (impl_->watcher && !impl_->sdk_missing) {
        bool changed = false;
        for (const filesystem::FileEvent& event : impl_->watcher->poll()) {
            const std::string extension = filesystem::extension(event.path);
            const bool source =
                extension == ".cs" || extension == ".csproj" || extension == ".props";
            const bool output = event.path.find("/obj/") != std::string::npos ||
                                event.path.find("/bin/") != std::string::npos;
            changed = changed || (source && !output);
        }
        if (changed) {
            log::info("Skripte geändert, baue neu");
            impl_->builder->start();
        }
    }
    if (auto result = impl_->builder->poll()) {
        impl_->handle(*std::move(result));
    }
    impl_->status.building = impl_->builder->busy();
}

bool ScriptSystem::reload(std::string_view assembly) {
    return impl_->reload(assembly);
}

void ScriptSystem::rebuild() {
    if (impl_->builder && impl_->desc.build) {
        impl_->sdk_missing = false;
        impl_->builder->start();
        impl_->status.building = true;
    }
}

void ScriptSystem::bind(world::World* world, const input::ActionState* actions,
                        const physics::PhysicsWorld* physics, events::EventBus* bus) {
    impl_->world = world;
    impl_->runtime->bind_world(world);
    impl_->runtime->bind_input(actions);
    impl_->runtime->bind_physics(physics, bus);
}

void ScriptSystem::bind_audio(audio::AudioSystem* audio, audio::AudioEngine* engine,
                              const world::TerrainMaterials* materials) {
    impl_->runtime->bind_audio(audio, engine, materials);
}

void ScriptSystem::bind_animation(animation::AnimationSystem* animation) {
    impl_->runtime->bind_animation(animation);
}

void ScriptSystem::run(ScriptPhase phase, f32 delta) {
    impl_->run(phase, delta);
}

void ScriptSystem::add_to(world::Simulation& simulation) {
    Impl* impl = impl_.get();
    world::World* world = &simulation.world();
    simulation.add_fixed_stage(
        [impl, world] { impl->run(ScriptPhase::Fixed, world->time().fixed_delta); });
    simulation.add_frame_stage(
        [impl, world] { impl->run(ScriptPhase::Frame, world->time().delta); });
}

void ScriptSystem::set_active(bool active) {
    if (impl_->active && !active && impl_->runtime->scripts_loaded()) {
        impl_->runtime->reset_instances();
    }
    impl_->active = active;
    validate();
}

void ScriptSystem::validate() {
    if (!impl_->active) {
        impl_->runtime->validate();
    }
}

bool ScriptSystem::active() const noexcept {
    return impl_->active;
}

const ScriptStatus& ScriptSystem::status() const noexcept {
    return impl_->status;
}

ScriptRuntime& ScriptSystem::runtime() noexcept {
    return *impl_->runtime;
}

} // namespace ngine::script
