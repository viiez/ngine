module;

#include <optional>
#include <string>
#include <string_view>
#include <vector>

module ngine.replication;

import ngine.core;
import ngine.reflect;
import ngine.world;

namespace ngine::replication {

namespace {

class Fnv {
public:
    void add(std::string_view text) noexcept {
        for (const char c : text) {
            add_byte(static_cast<u8>(c));
        }
        // Trenner, damit "ab"+"c" und "a"+"bc" verschieden sind.
        add_byte(0);
    }
    void add(u64 value) noexcept {
        for (u32 i = 0; i < 8; ++i) {
            add_byte(static_cast<u8>(value >> (i * 8)));
        }
    }
    [[nodiscard]] u64 value() const noexcept {
        return hash_;
    }

private:
    void add_byte(u8 byte) noexcept {
        hash_ ^= byte;
        hash_ *= 1099511628211ull;
    }
    u64 hash_ = 14695981039346656037ull;
};

} // namespace

Schema::Schema(const reflect::TypeRegistry& types) {
    const reflect::TypeInfo* transform = types.find<world::Transform>();
    Fnv hash;
    hash.add(u64{protocol_version});
    for (const reflect::TypeInfo* type : types.types()) {
        if (!type->replicated || type->retired || type->kind != reflect::Kind::Struct) {
            continue;
        }
        verify(components_.size() < 0xFFFF, "Höchstens 65535 replizierte Komponenten");
        SchemaComponent component{.type = type, .codec = Codec::Fields, .fields = {}, .parts = 0};
        hash.add(type->name);
        if (type == transform) {
            component.codec = Codec::Transform;
            component.parts = 3;
            transform_ = static_cast<u16>(components_.size());
            hash.add("codec:transform");
        } else {
            for (const reflect::Field& field : type->fields) {
                if (field.local) {
                    continue;
                }
                component.fields.push_back(&field);
                hash.add(field.name);
                hash.add(field.type->name);
            }
            component.parts = static_cast<u32>(component.fields.size());
        }
        if (component.parts == 0 || component.parts > 64) {
            log::fatal("Replikation: {} hat {} replizierte Felder (1 bis 64 erlaubt)", type->name,
                       component.parts);
            verify(false, "Replizierte Komponente mit falscher Zahl an Feldern");
        }
        components_.push_back(std::move(component));
    }
    hash_ = hash.value();
}

std::optional<u16> Schema::index_of(const reflect::TypeInfo& type) const noexcept {
    for (usize i = 0; i < components_.size(); ++i) {
        if (components_[i].type == &type) {
            return static_cast<u16>(i);
        }
    }
    return std::nullopt;
}

std::string Schema::describe() const {
    std::string text;
    for (const SchemaComponent& component : components_) {
        if (!text.empty()) {
            text += ", ";
        }
        text += component.type->name;
    }
    return text;
}

} // namespace ngine::replication
