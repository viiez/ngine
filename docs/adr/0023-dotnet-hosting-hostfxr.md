# 0023: .NET über hostfxr hosten, Skripte in entladbarem Kontext

- **Status:** vorgeschlagen
- **Datum:** 2026-11-01

## Kontext

[ADR 0006](0006-csharp-scripting-dotnet.md) legt C# über eingebettetes .NET (CoreCLR) fest, mit
Hot Reload über entladbare Assemblies. Offen war, wie genau die Engine .NET startet, wie Aufrufe in
beide Richtungen laufen und ob Entladen in der Praxis funktioniert. Der Durchstich D2 hat das gebaut
und gemessen.

## Entscheidung

- **Start über hostfxr:** Die Engine sucht hostfxr selbst (`DOTNET_ROOT`, dann die üblichen
  Installationsorte, jeweils die neueste Version unter `host/fxr/`), lädt es dynamisch und startet
  .NET mit `hostfxr_initialize_for_runtime_config`. Die wenigen Funktionstypen stehen im Adapter;
  nethost linken wir nicht.
- **Verwalteter Host im Standardkontext:** `NGine.Scripting` kommt über `load_assembly` in den
  Standard-`AssemblyLoadContext`, die Einstiege über `get_function_pointer` als Methoden mit
  `[UnmanagedCallersOnly]`. Nicht über `load_assembly_and_get_function_pointer`, das in einen
  isolierten Kontext lädt.
- **Aufrufe:** C++ → C# über diese Funktionszeiger, C# → C++ über eine Tabelle von
  `delegate* unmanaged`, die C++ beim Start übergibt. Keine Delegates, kein Marshalling: Texte sind
  UTF-8 mit Länge, gemeinsame Daten sind blittable Structs mit gleichem Layout auf beiden Seiten.
- **Skripte in einem entladbaren Kontext:** Spielskripte kommen aus einem Speicherstrom (die Datei
  bleibt frei) in einen eigenen, entladbaren `AssemblyLoadContext`. `NGine.Scripting` wird dort
  nicht noch einmal geladen, damit Schnittstellen wie `IScript` auf beiden Seiten derselbe Typ sind.
- **Version:** .NET 10 (LTS). Unter Linux aus den Paketquellen von Ubuntu 24.04
  (`dotnet-sdk-10.0`, [ADR 0017](0017-platform-baseline-feature-tiers.md)), unter Windows das SDK
  von Microsoft.
- **Bauen:** CMake ruft `dotnet build` mit `--artifacts-path` im Build-Verzeichnis. Ohne SDK baut
  der C++-Teil weiter, nur ohne Assemblies.

## Konsequenzen

- Aufrufe sind billig (rund 10 ns in jede Richtung im Debug-Build); ein Skript kann pro Frame viele
  Engine-Funktionen nutzen. Daten der Engine sieht C# ohne Kopie als `Span<T>`.
- .NET startet in rund 40 ms und lässt sich pro Prozess nur einmal starten und nie entladen. Tests
  teilen sich eine Laufzeit; Editor und Spiel laden Skripte neu, nicht die Laufzeit.
- Entladen funktioniert nur, wenn nichts außerhalb des Kontexts auf Skripttypen verweist (Events,
  statische Felder in `NGine.Scripting`, Caches). Die Engine prüft das Freigeben über einen
  schwachen Verweis und meldet, wenn es scheitert.
- Eine installierte .NET-Laufzeit ist Voraussetzung. Für ausgelieferte Spiele kommt später eine
  mitgelieferte Laufzeit (self-contained) in Frage; der Suchweg über `dotnet_root` lässt das zu.
- Die C#-API von Hand (`Log`, `IScript`, `Frame`) ist Wegwerf-Code des Durchstichs. In R4 erzeugen
  wir sie aus der Reflection ([ADR 0018](0018-own-reflection.md), SCR-02).
- Ausnahmen dürfen nicht nach C++ gelangen; jeder Einstieg in `NGine.Host` fängt sie und liefert
  einen Fehlercode.

## Erwogene Alternativen

- **nethost linken (`get_hostfxr_path`):** offizieller Suchweg, aber eine weitere Bibliothek, die
  vcpkg nicht anbietet und die nur wenige Zeilen Suchlogik ersetzt.
- **`load_assembly_and_get_function_pointer`:** einfacher, lädt aber in einen isolierten Kontext;
  Skripte hätten eine zweite Kopie von `NGine.Scripting` gesehen.
- **Delegates und `Marshal.GetFunctionPointerForDelegate`:** langsamer und mit GC-Fallen (Delegate
  muss am Leben bleiben). Funktionszeiger mit `[UnmanagedCallersOnly]` sind seit .NET 5 der
  empfohlene Weg.
- **Mono oder NativeAOT:** siehe ADR 0006.
