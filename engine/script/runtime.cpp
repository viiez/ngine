module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Adapter für hostfxr, den offiziellen Weg, .NET in eine Anwendung einzubetten. Die Funktionstypen
// entsprechen hostfxr.h und coreclr_delegates.h aus dem .NET-Runtime-Repository; wir laden hostfxr
// selbst, statt nethost zu linken (ADR 0023).
module ngine.script;

import ngine.core;
import ngine.profile;
import ngine.reflect;
import ngine.world;
import :native;
import :script_api;
import :world_api;

namespace ngine::script {

namespace {

// Zeichentyp von hostfxr: char unter Linux, wchar_t unter Windows. std::filesystem::path nutzt
// denselben Typ und übernimmt die Umwandlung aus UTF-8.
using char_t = std::filesystem::path::value_type;
using native_string = std::filesystem::path::string_type;

native_string to_native(std::string_view utf8) {
    return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}}.native();
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}

// --- hostfxr -------------------------------------------------------------------------------

using HostHandle = void*;

struct InitializeParameters {
    std::size_t size;
    const char_t* host_path;
    const char_t* dotnet_root;
};

using InitializeForRuntimeConfigFn = std::int32_t (*)(const char_t*, const InitializeParameters*,
                                                      HostHandle*);
using GetRuntimeDelegateFn = std::int32_t (*)(HostHandle, std::int32_t, void**);
using CloseFn = std::int32_t (*)(HostHandle);
using ErrorWriterFn = void (*)(const char_t*);
using SetErrorWriterFn = ErrorWriterFn (*)(ErrorWriterFn);

// Werte aus hostfxr_delegate_type.
constexpr std::int32_t delegate_get_function_pointer = 6;
constexpr std::int32_t delegate_load_assembly = 7;

using LoadAssemblyFn = std::int32_t (*)(const char_t* path, void* load_context, void* reserved);
using GetFunctionPointerFn = std::int32_t (*)(const char_t* type_name, const char_t* method_name,
                                              const char_t* delegate_type_name, void* load_context,
                                              void* reserved, void** delegate);

// Statt eines Delegate-Typs: Die Methode trägt [UnmanagedCallersOnly].
const char_t* const unmanaged_callers_only =
    reinterpret_cast<const char_t*>(static_cast<std::intptr_t>(-1));

void write_hostfxr_error(const char_t* message) {
    log::error(".NET: {}", to_utf8(std::filesystem::path{message}));
}

// --- Funktionen, die C# aufruft ----------------------------------------------------------

// Gleicher Aufbau wie NGine.NativeApi in Native.cs.
struct NativeApi {
    void (*log)(std::int32_t level, const u8* text, std::int32_t length);
    std::int32_t (*add)(std::int32_t a, std::int32_t b);
    WorldApi world;
    ScriptApi script;
};

void native_log(std::int32_t level, const u8* text, std::int32_t length) {
    const auto clamped = std::clamp(level, 0, static_cast<std::int32_t>(log::Level::Fatal));
    log::write(static_cast<log::Level>(clamped),
               std::string_view{reinterpret_cast<const char*>(text), static_cast<usize>(length)});
}

std::int32_t native_add(std::int32_t a, std::int32_t b) {
    return a + b;
}

// --- Funktionen in C# (NGine.Host) --------------------------------------------------------

struct ManagedApi {
    std::int32_t (*initialize)(NativeApi*, std::int32_t) = nullptr;
    std::int32_t (*load_scripts)(const u8*, std::int32_t) = nullptr;
    std::int32_t (*start)() = nullptr;
    std::int32_t (*update)(f32, Position*, std::int32_t) = nullptr;
    std::int32_t (*unload_scripts)() = nullptr;
    std::int32_t (*add)(std::int32_t, std::int32_t) = nullptr;
    std::int64_t (*benchmark_native_calls)(std::int32_t, std::int32_t*) = nullptr;
    void (*world_changed)() = nullptr;
    std::int32_t (*describe_layout)(void (*)(const u8*, std::int32_t, const u8*, std::int32_t,
                                             std::int64_t)) = nullptr;
    std::int32_t (*invoke)(const u8*, std::int32_t) = nullptr;
    std::int32_t (*register_scripts)() = nullptr;
    std::int32_t (*run_scripts)(std::int32_t, f32) = nullptr;
    std::int32_t (*store_fields)() = nullptr;
    std::int32_t (*load_fields)() = nullptr;
    std::int32_t (*reset_instances)() = nullptr;
};

// Anordnung der Structs, wie C# sie meldet: Typ → Größe und Offsets der Felder.
struct ReportedType {
    std::int64_t size = -1;
    std::map<std::string, std::int64_t, std::less<>> fields;
};
std::map<std::string, ReportedType, std::less<>>* layout_report = nullptr;

void report_layout(const u8* type, std::int32_t type_length, const u8* field,
                   std::int32_t field_length, std::int64_t value) {
    if (layout_report == nullptr) {
        return;
    }
    const std::string type_name{reinterpret_cast<const char*>(type),
                                static_cast<usize>(type_length)};
    ReportedType& entry = (*layout_report)[type_name];
    if (field_length == 0) {
        entry.size = value;
    } else {
        entry.fields[std::string{reinterpret_cast<const char*>(field),
                                 static_cast<usize>(field_length)}] = value;
    }
}

// Neueste Version unter <root>/host/fxr/<version>/.
std::optional<std::filesystem::path> find_hostfxr(const std::filesystem::path& root) {
    std::error_code error;
    const auto fxr = root / "host" / "fxr";
    if (!std::filesystem::is_directory(fxr, error)) {
        return std::nullopt;
    }
    auto version_of = [](const std::filesystem::path& dir) {
        std::vector<int> parts;
        int value = 0;
        bool digit = false;
        for (const char c : to_utf8(dir.filename())) {
            if (c >= '0' && c <= '9') {
                value = value * 10 + (c - '0');
                digit = true;
            } else if (c == '.' || c == '-') {
                parts.push_back(digit ? value : 0);
                value = 0;
                digit = false;
                if (c == '-') {
                    break; // Vorabversionen zählen wie die Version davor
                }
            }
        }
        if (digit) {
            parts.push_back(value);
        }
        return parts;
    };
    std::optional<std::filesystem::path> best;
    std::vector<int> best_version;
    for (const auto& entry : std::filesystem::directory_iterator{fxr, error}) {
        const auto library = entry.path() / hostfxr_file_name();
        if (!std::filesystem::exists(library, error)) {
            continue;
        }
        const auto version = version_of(entry.path());
        if (!best || version > best_version) {
            best = library;
            best_version = version;
        }
    }
    return best;
}

std::atomic<bool> runtime_alive{false};

} // namespace

// Schließt den Kontext von hostfxr. Die Laufzeit selbst bleibt geladen; .NET lässt sich nicht
// entladen. Die Funktionszeiger in C# bleiben danach gültig.
struct HostContext {
    HostHandle handle = nullptr;
    CloseFn close = nullptr;

    HostContext() = default;
    HostContext(const HostContext&) = delete;
    HostContext& operator=(const HostContext&) = delete;
    ~HostContext() {
        if (handle != nullptr && close != nullptr) {
            close(handle);
        }
    }
};

struct ScriptRuntime::Impl {
    // hostfxr bleibt geladen, solange der Prozess läuft; .NET hängt daran.
    void* library = nullptr;
    ManagedApi managed;
    NativeApi native{native_log, native_add, make_world_api(), make_script_api()};
    bool scripts_loaded = false;
};

Result<std::unique_ptr<ScriptRuntime>> ScriptRuntime::create(const RuntimeDesc& desc) {
    NGINE_PROFILE_ZONE("ScriptRuntime::create");
    if (runtime_alive.exchange(true)) {
        return std::unexpected("Es gibt schon eine .NET-Laufzeit in diesem Prozess");
    }
    // Gibt die Sperre frei, falls der Start scheitert.
    struct Guard {
        bool armed = true;
        ~Guard() {
            if (armed) {
                runtime_alive = false;
            }
        }
    } guard;

    // 1. hostfxr finden und laden.
    std::vector<std::string> roots;
    if (!desc.dotnet_root.empty()) {
        roots.push_back(desc.dotnet_root);
    }
    for (auto& root : dotnet_roots()) {
        roots.push_back(std::move(root));
    }
    std::optional<std::filesystem::path> hostfxr_path;
    std::filesystem::path dotnet_root;
    for (const auto& root : roots) {
        const std::filesystem::path path{std::u8string{root.begin(), root.end()}};
        if (auto found = find_hostfxr(path)) {
            hostfxr_path = std::move(found);
            dotnet_root = path;
            break;
        }
    }
    if (!hostfxr_path) {
        return std::unexpected(
            "hostfxr nicht gefunden. Ist .NET 10 installiert? Sonst DOTNET_ROOT setzen.");
    }

    auto impl = std::make_unique<Impl>();
    std::string error;
    impl->library = load_library(to_utf8(*hostfxr_path), error);
    if (impl->library == nullptr) {
        return std::unexpected(std::format("hostfxr lässt sich nicht laden: {}", error));
    }
    auto symbol = [&]<typename T>(T& out, const char* name) {
        out = reinterpret_cast<T>(find_symbol(impl->library, name));
        return out != nullptr;
    };
    InitializeForRuntimeConfigFn initialize = nullptr;
    GetRuntimeDelegateFn get_delegate = nullptr;
    SetErrorWriterFn set_error_writer = nullptr;
    HostContext host;
    if (!symbol(initialize, "hostfxr_initialize_for_runtime_config") ||
        !symbol(get_delegate, "hostfxr_get_runtime_delegate") ||
        !symbol(host.close, "hostfxr_close") ||
        !symbol(set_error_writer, "hostfxr_set_error_writer")) {
        return std::unexpected("hostfxr bietet nicht alle nötigen Funktionen an");
    }
    // Fehler von hostfxr ins Log, nur während des Starts: Beim Beenden des Prozesses gibt es das
    // Log womöglich nicht mehr.
    set_error_writer(write_hostfxr_error);
    struct ErrorWriterReset {
        SetErrorWriterFn set;
        ~ErrorWriterReset() {
            set(nullptr);
        }
    } reset_error_writer{set_error_writer};

    // 2. Laufzeit über die runtimeconfig.json von NGine.Scripting starten.
    const std::filesystem::path dir{
        std::u8string{desc.assembly_dir.begin(), desc.assembly_dir.end()}};
    const auto config = dir / "NGine.Scripting.runtimeconfig.json";
    const auto assembly = dir / "NGine.Scripting.dll";
    std::error_code exists_error;
    if (!std::filesystem::exists(assembly, exists_error)) {
        return std::unexpected(std::format("{} fehlt", to_utf8(assembly)));
    }
    const native_string root_native = dotnet_root.native();
    const InitializeParameters parameters{sizeof(InitializeParameters), nullptr,
                                          root_native.c_str()};
    // 0: gestartet, 1: lief schon, 2: lief schon mit anderen Einstellungen.
    if (const std::int32_t result = initialize(config.c_str(), &parameters, &host.handle);
        result < 0 || result > 2) {
        return std::unexpected(
            std::format("hostfxr_initialize_for_runtime_config fehlgeschlagen: 0x{:08x}",
                        static_cast<std::uint32_t>(result)));
    }

    // 3. NGine.Scripting in den Standardkontext laden und die Einstiege holen.
    LoadAssemblyFn load_assembly = nullptr;
    GetFunctionPointerFn get_function = nullptr;
    if (get_delegate(host.handle, delegate_load_assembly,
                     reinterpret_cast<void**>(&load_assembly)) != 0 ||
        get_delegate(host.handle, delegate_get_function_pointer,
                     reinterpret_cast<void**>(&get_function)) != 0) {
        return std::unexpected("hostfxr liefert keine Delegates (braucht .NET 8 oder neuer)");
    }
    if (const std::int32_t result = load_assembly(assembly.c_str(), nullptr, nullptr);
        result != 0) {
        return std::unexpected(std::format("NGine.Scripting lässt sich nicht laden: 0x{:08x}",
                                           static_cast<std::uint32_t>(result)));
    }

    const native_string type = to_native("NGine.Host, NGine.Scripting");
    auto function = [&]<typename T>(T& out, std::string_view method) {
        const native_string name = to_native(method);
        void* pointer = nullptr;
        const std::int32_t result = get_function(type.c_str(), name.c_str(), unmanaged_callers_only,
                                                 nullptr, nullptr, &pointer);
        out = reinterpret_cast<T>(pointer);
        if (result != 0 || pointer == nullptr) {
            log::error("C#-Methode NGine.Host.{} nicht gefunden: 0x{:08x}", method,
                       static_cast<std::uint32_t>(result));
            return false;
        }
        return true;
    };
    ManagedApi& m = impl->managed;
    if (!function(m.initialize, "Initialize") || !function(m.load_scripts, "LoadScripts") ||
        !function(m.start, "Start") || !function(m.update, "Update") ||
        !function(m.unload_scripts, "UnloadScripts") || !function(m.add, "Add") ||
        !function(m.benchmark_native_calls, "BenchmarkNativeCalls") ||
        !function(m.world_changed, "WorldChanged") ||
        !function(m.describe_layout, "DescribeLayout") || !function(m.invoke, "Invoke") ||
        !function(m.register_scripts, "RegisterScripts") ||
        !function(m.run_scripts, "RunScripts") || !function(m.store_fields, "StoreFields") ||
        !function(m.load_fields, "LoadFields") || !function(m.reset_instances, "ResetInstances")) {
        return std::unexpected("NGine.Scripting passt nicht zur Engine");
    }
    if (m.initialize(&impl->native, static_cast<std::int32_t>(sizeof(NativeApi))) != 0) {
        return std::unexpected("NGine.Host.Initialize ist fehlgeschlagen");
    }

    guard.armed = false;
    log::info("hostfxr: {}", to_utf8(*hostfxr_path));
    return std::make_unique<ScriptRuntime>(std::move(impl));
}

ScriptRuntime::ScriptRuntime(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {
    verify(impl_ != nullptr, "ScriptRuntime nur über create() erzeugen");
}

ScriptRuntime::~ScriptRuntime() {
    if (impl_->scripts_loaded) {
        (void)unload_scripts();
    }
    bind_script_world(nullptr);
    impl_.reset();
    runtime_alive = false;
}

Result<u32> ScriptRuntime::load_scripts(std::string_view assembly_path) {
    NGINE_PROFILE_ZONE("ScriptRuntime::load_scripts");
    const std::int32_t count =
        impl_->managed.load_scripts(reinterpret_cast<const u8*>(assembly_path.data()),
                                    static_cast<std::int32_t>(assembly_path.size()));
    if (count < 0) {
        return std::unexpected(
            std::format("Skripte aus {} lassen sich nicht laden", assembly_path));
    }
    impl_->scripts_loaded = true;
    // Skript-Komponenten als Typen anmelden; Komponenten geänderter Typen ziehen um.
    clear_registered_script_types();
    if (script_registry() != nullptr) {
        if (impl_->managed.register_scripts() < 0) {
            return std::unexpected("Skript-Komponenten lassen sich nicht anmelden");
        }
        if (const usize moved = migrate_script_types(); moved > 0) {
            log::info("{} Skript-Komponenten auf geänderte Felder umgezogen", moved);
        }
    }
    return static_cast<u32>(count);
}

void ScriptRuntime::use_types(reflect::TypeRegistry* types) {
    set_script_registry(types);
}

std::span<const reflect::TypeInfo* const> ScriptRuntime::script_types() const noexcept {
    return registered_script_types();
}

Result<void> ScriptRuntime::run(ScriptPhase phase, f32 delta_time) {
    NGINE_PROFILE_ZONE("ScriptRuntime::run");
    if (script_world() == nullptr) {
        return {};
    }
    const std::int32_t errors =
        impl_->managed.run_scripts(phase == ScriptPhase::Fixed ? 0 : 1, delta_time);
    apply_script_commands();
    if (errors != 0) {
        return std::unexpected(std::format("{} Fehler in Skripten", errors));
    }
    return {};
}

void ScriptRuntime::store_fields() {
    NGINE_PROFILE_ZONE("ScriptRuntime::store_fields");
    (void)impl_->managed.store_fields();
}

void ScriptRuntime::load_fields() {
    NGINE_PROFILE_ZONE("ScriptRuntime::load_fields");
    (void)impl_->managed.load_fields();
}

void ScriptRuntime::reset_instances() {
    (void)impl_->managed.reset_instances();
    apply_script_commands();
}

Result<bool> ScriptRuntime::unload_scripts() {
    NGINE_PROFILE_ZONE("ScriptRuntime::unload_scripts");
    const std::int32_t result = impl_->managed.unload_scripts();
    if (result < 0) {
        return std::unexpected("Skripte lassen sich nicht entladen");
    }
    impl_->scripts_loaded = false;
    return result == 1;
}

Result<void> ScriptRuntime::start() {
    if (impl_->managed.start() != 0) {
        return std::unexpected("Fehler beim Start der Skripte");
    }
    return {};
}

Result<void> ScriptRuntime::update(f32 delta_time, std::span<Position> positions) {
    NGINE_PROFILE_ZONE("ScriptRuntime::update");
    if (impl_->managed.update(delta_time, positions.data(),
                              static_cast<std::int32_t>(positions.size())) != 0) {
        return std::unexpected("Fehler in einem Skript");
    }
    return {};
}

void ScriptRuntime::bind_world(world::World* world) {
    bind_script_world(world);
    impl_->managed.world_changed();
}

void ScriptRuntime::apply_commands() {
    NGINE_PROFILE_ZONE("ScriptRuntime::apply_commands");
    apply_script_commands();
}

Result<void> ScriptRuntime::verify_bindings(const reflect::TypeRegistry& types, bool require_all) {
    std::map<std::string, ReportedType, std::less<>> reported;
    layout_report = &reported;
    const std::int32_t result = impl_->managed.describe_layout(report_layout);
    layout_report = nullptr;
    if (result != 0) {
        return std::unexpected("C# kann die Anordnung der Bindings nicht melden");
    }
    std::string problems;
    const auto problem = [&](std::string text) {
        if (!problems.empty()) {
            problems += "; ";
        }
        problems += std::move(text);
    };
    usize compared = 0;
    for (const reflect::TypeInfo* type : types.types()) {
        if (type->kind != reflect::Kind::Struct) {
            continue;
        }
        const auto found = reported.find(type->name);
        if (found == reported.end()) {
            if (require_all) {
                problem(std::format("{} fehlt in C#", type->name));
            }
            continue;
        }
        ++compared;
        const ReportedType& seen = found->second;
        if (seen.size != static_cast<std::int64_t>(type->size)) {
            problem(
                std::format("{}: Größe {} in C#, {} in C++", type->name, seen.size, type->size));
        }
        for (const reflect::Field& field : type->fields) {
            if (!csharp_visible(field)) {
                continue;
            }
            const auto offset = seen.fields.find(field.name);
            if (offset == seen.fields.end()) {
                problem(std::format("{}.{} fehlt in C#", type->name, field.name));
            } else if (offset->second != static_cast<std::int64_t>(field.offset)) {
                problem(std::format("{}.{}: Offset {} in C#, {} in C++", type->name, field.name,
                                    offset->second, field.offset));
            }
        }
        if (seen.fields.size() > type->fields.size()) {
            problem(std::format("{}: C# kennt Felder, die C++ nicht hat", type->name));
        }
    }
    if (require_all) {
        for (const auto& [name, seen] : reported) {
            if (types.find(name) == nullptr) {
                problem(std::format("{} gibt es nur in C#", name));
            }
        }
    }
    if (!problems.empty()) {
        return std::unexpected(std::format("Bindings passen nicht zur Engine: {}", problems));
    }
    log::debug("Bindings geprüft: {} Typen passen", compared);
    return {};
}

Result<void> ScriptRuntime::invoke(std::string_view method) {
    NGINE_PROFILE_ZONE("ScriptRuntime::invoke");
    const std::int32_t result = impl_->managed.invoke(reinterpret_cast<const u8*>(method.data()),
                                                      static_cast<std::int32_t>(method.size()));
    if (result == -2) {
        return std::unexpected(std::format("{} gibt es in den Skripten nicht", method));
    }
    if (result != 0) {
        return std::unexpected(std::format("Fehler in {}", method));
    }
    return {};
}

i32 ScriptRuntime::add(i32 a, i32 b) {
    return impl_->managed.add(a, b);
}

f64 ScriptRuntime::measure_calls_to_managed(u32 count) {
    std::int32_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < count; ++i) {
        sum = impl_->managed.add(sum, 1);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    verify(sum == static_cast<std::int32_t>(count), "Falsche Summe beim Messen der Aufrufe");
    return std::chrono::duration<f64, std::nano>(elapsed).count() / std::max<f64>(count, 1);
}

f64 ScriptRuntime::measure_calls_to_native(u32 count) {
    std::int32_t sum = 0;
    const std::int64_t ns =
        impl_->managed.benchmark_native_calls(static_cast<std::int32_t>(count), &sum);
    verify(sum == static_cast<std::int32_t>(count), "Falsche Summe beim Messen der Aufrufe");
    return static_cast<f64>(ns) / std::max<f64>(count, 1);
}

} // namespace ngine::script
