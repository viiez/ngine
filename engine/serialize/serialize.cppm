// Serialisierung (CORE-09): JSON als lesbares Format (Szenen, Einstellungen, ADR 0020) und ein
// binäres Format (Netzwerk, gekochte Assets). Beide arbeiten über die Reflection.
export module ngine.serialize;

export import :json;
export import :binary;
export import :reflected;
