# 0006: Scripting in C# über eingebettetes .NET

- **Status:** angenommen
- **Datum:** 2026-10-23

## Kontext

Gameplay-Logik soll ohne Neukompilieren der Engine entstehen. Gewünschte Sprache ist C#.

## Entscheidung

.NET (CoreCLR, aktuelles LTS) wird über hostfxr eingebettet. Die C#-API wird aus unserer Reflection
erzeugt. Hot Reload über entladbare Assemblies.

## Konsequenzen

- Moderne Sprache mit großem Ökosystem, ähnlich wie in Unity.
- Interop zwischen C++ und C# kostet Aufwand; die Reflection muss dafür früh tragen.
- Der Durchstich D2 klärt die Risiken vor dem Ausbau in R4.

## Erwogene Alternativen

- **Mono:** von Unity genutzt, aber nur noch in Wartung.
- **NativeAOT:** schnell, aber kein Hot Reload.
- **Lua:** leichter einzubetten, aber nicht gewünscht.
