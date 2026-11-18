// Umsetzung von miniaudio und stb_vorbis (Ogg Vorbis), einmal für die ganze Engine. Eigene
// Übersetzungseinheit ohne Modul und ohne Warnungen, weil es Code einer Fremdbibliothek ist
// (siehe CMakeLists.txt). Benutzt wird miniaudio nur im Adapter miniaudio.cpp.
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NULL
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
