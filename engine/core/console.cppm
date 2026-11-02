// Interne Partition: Vorbereitung der Konsole für die Log-Ausgabe. Plattformcode steht in
// console_windows.cpp bzw. console_posix.cpp.
module ngine.core:console;

namespace ngine::log::detail {

// Stellt die Konsole auf UTF-8 um, damit Umlaute richtig erscheinen. Unter Linux ist sie es schon.
void prepare_console() noexcept;

} // namespace ngine::log::detail
