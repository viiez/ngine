module;

// Einzige Übersetzungseinheit, die Slang kennt (ADR 0034).
#include <ngine/profile.hpp>

#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <slang-com-ptr.h>
#include <slang.h>

module ngine.shader_compiler;

import ngine.core;
import ngine.filesystem;
import ngine.profile;
import ngine.rhi;

namespace ngine::shader_compiler {

namespace {

std::string diagnostics(slang::IBlob* blob) {
    if (blob == nullptr || blob->getBufferSize() == 0) {
        return {};
    }
    std::string text{static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize()};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '\0')) {
        text.pop_back();
    }
    return text;
}

SlangStage to_slang(rhi::ShaderStage stage) {
    switch (stage) {
    case rhi::ShaderStage::Vertex: return SLANG_STAGE_VERTEX;
    case rhi::ShaderStage::Fragment: return SLANG_STAGE_FRAGMENT;
    case rhi::ShaderStage::Compute: return SLANG_STAGE_COMPUTE;
    }
    return SLANG_STAGE_NONE;
}

} // namespace

struct Compiler::Impl {
    Slang::ComPtr<slang::IGlobalSession> global;
};

Compiler::Compiler(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Compiler::~Compiler() = default;

Result<std::unique_ptr<Compiler>> Compiler::create() {
    NGINE_PROFILE_ZONE("shader_compiler::create");
    auto impl = std::make_unique<Impl>();
    if (SLANG_FAILED(slang::createGlobalSession(impl->global.writeRef()))) {
        return std::unexpected(std::string{"Slang lässt sich nicht starten"});
    }
    return std::unique_ptr<Compiler>{new Compiler{std::move(impl)}};
}

Result<std::vector<u32>> Compiler::compile(std::string_view path, std::string_view entry,
                                           rhi::ShaderStage stage) {
    NGINE_PROFILE_ZONE("shader_compiler::compile");
    auto source = filesystem::read_text(path);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    const std::string file{path};
    const std::string directory = filesystem::parent_path(path);
    const std::string entry_name{entry};

    // Dieselben Optionen wie slangc beim Bauen (cmake/NgineShaders.cmake).
    slang::TargetDesc target{};
    target.format = SLANG_SPIRV;
    target.profile = impl_->global->findProfile("spirv_1_5");
    const char* search_paths[] = {directory.c_str()};
    slang::CompilerOptionEntry options[] = {
        {.name = slang::CompilerOptionName::VulkanUseEntryPointName,
         .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = 1}},
    };
    slang::SessionDesc session_desc{};
    session_desc.targets = &target;
    session_desc.targetCount = 1;
    session_desc.searchPaths = search_paths;
    session_desc.searchPathCount = 1;
    session_desc.compilerOptionEntries = options;
    session_desc.compilerOptionEntryCount = 1;

    // Eine Sitzung pro Aufruf: Sie merkt sich geladene Module, beim Neuladen sollen aber die
    // geänderten Dateien gelesen werden.
    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(impl_->global->createSession(session_desc, session.writeRef()))) {
        return std::unexpected(std::string{"Slang-Sitzung lässt sich nicht anlegen"});
    }
    Slang::ComPtr<slang::IBlob> messages;
    const std::string module_name = std::filesystem::path{file}.stem().string();
    slang::IModule* module = session->loadModuleFromSourceString(
        module_name.c_str(), file.c_str(), source->c_str(), messages.writeRef());
    if (module == nullptr) {
        return std::unexpected(diagnostics(messages));
    }
    Slang::ComPtr<slang::IEntryPoint> entry_point;
    if (SLANG_FAILED(module->findAndCheckEntryPoint(entry_name.c_str(), to_slang(stage),
                                                    entry_point.writeRef(), messages.writeRef()))) {
        const std::string text = diagnostics(messages);
        return std::unexpected(text.empty() ? std::format("Einstiegspunkt '{}' fehlt", entry_name)
                                            : text);
    }
    slang::IComponentType* parts[] = {module, entry_point};
    Slang::ComPtr<slang::IComponentType> program;
    if (SLANG_FAILED(session->createCompositeComponentType(parts, 2, program.writeRef(),
                                                           messages.writeRef()))) {
        return std::unexpected(diagnostics(messages));
    }
    Slang::ComPtr<slang::IComponentType> linked;
    if (SLANG_FAILED(program->link(linked.writeRef(), messages.writeRef()))) {
        return std::unexpected(diagnostics(messages));
    }
    Slang::ComPtr<slang::IBlob> code;
    if (SLANG_FAILED(linked->getEntryPointCode(0, 0, code.writeRef(), messages.writeRef())) ||
        code == nullptr) {
        return std::unexpected(diagnostics(messages));
    }
    if (const std::string warnings = diagnostics(messages); !warnings.empty()) {
        log::warn("Slang: {}", warnings);
    }
    if (code->getBufferSize() % sizeof(u32) != 0) {
        return std::unexpected(std::string{"Slang lieferte kein gültiges SPIR-V"});
    }
    std::vector<u32> words(code->getBufferSize() / sizeof(u32));
    std::memcpy(words.data(), code->getBufferPointer(), code->getBufferSize());
    return words;
}

} // namespace ngine::shader_compiler
