// Reflection der Engine (CORE-08, ADR 0018): beschreibt Typen mit ihren Feldern, damit Speichern,
// Netzwerk, Editor und C# mit Daten umgehen können, ohne die Typen zu kennen.
export module ngine.reflect;

export import :type;
export import :registry;
