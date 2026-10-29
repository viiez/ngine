module;

#include <source_location>
#include <string_view>

export module ngine.core:check;

export namespace ngine {

// Prüft eine Bedingung, die nie falsch sein darf. Ist sie falsch, schreibt verify()
// eine Fatal-Meldung mit Datei und Zeile ins Log und bricht das Programm ab.
// Bleibt auch in Release-Builds aktiv.
void verify(bool condition, std::string_view message,
            std::source_location location = std::source_location::current());

} // namespace ngine
