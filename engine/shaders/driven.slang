// GPU-getriebenes Zeichnen (R3.5, ADR 0043): Jedes Netz, das der Renderer zeichnet, ist eine Instanz
// im Instanzpuffer des Frames; seine Dreiecke liegen in Meshlets (ngine.meshlet). Ein Task-Shader
// (Mesh-Shader-Pfad) oder ein Compute-Shader (Rückfallpfad) prüft je Gruppe bis zu 32 Meshlets einer
// Instanz gegen Stumpf, Normalenkegel und Hi-Z (cull.slang). Sichtbare Meshlets zeichnet ein
// Mesh-Shader bzw. ein indirekter Draw mit Indexpuffer. Beide Pfade lesen dieselben Puffer, und die
// Shader der Netze rechnen einen Eckpunkt in beiden Pfaden gleich (vertex_main und mesh_main).

#include "scene.slang"

// Meshlets je Gruppe, Threads je Arbeitsgruppe von Task-, Mesh- und Compute-Shader.
static const uint group_meshlets = 32;
static const uint mesh_threads = 32;
static const uint max_meshlet_vertices = 64;
static const uint max_meshlet_triangles = 124;

// Instanz ohne Kegeltest (doppelseitiges Material).
static const uint instance_double_sided = 1;

// Wie render::GpuMeshlet (48 Bytes).
struct Meshlet
{
    // xyz: Mittelpunkt der Hüllkugel in Koordinaten der Eckpunkte, w: Radius
    float4 sphere;
    // xyz: Achse des Normalenkegels, w: Grenze (1: kein Test)
    float4 cone;
    // Wort, an dem die Eckpunkte des Meshlets beginnen (Indizes ins Netz)
    uint vertex_offset;
    // Wort, an dem seine Dreiecke beginnen (drei lokale Indizes je Wort)
    uint triangle_offset;
    // Erstes Dreieck im umsortierten Indexpuffer des Teilnetzes
    uint first_triangle;
    // Bits 0–7: Eckpunkte, 8–15: Dreiecke
    uint counts;
};

// Wie render::GpuInstance (144 Bytes). V ist die Art der Eckpunkte.
struct InstanceT<V>
{
    // Zeilen der affinen Matrix von Objekt- in kamerarelative Weltkoordinaten
    float4 model[3];
    // xyz: Mittelpunkt der Hüllkugel, kamerarelativ in Metern, w: Radius
    float4 sphere;
    // Nur Terrain: xyz Ecke des Chunks relativ zum Terrain in Metern, w Meter pro Voxel
    float4 offset;
    V* vertices;
    // Meshlets des Netzes und dieselbe Adresse als Wörter (Eckpunkte und Dreiecke der Meshlets)
    Meshlet* meshlets;
    uint* words;
    uint first_meshlet;
    uint meshlet_count;
    // Material im MaterialStore (Meshes)
    uint material;
    // Kennung der Entity für den ID-Puffer
    uint id;
    // Nur Terrain: Materialnummern der acht Palettenplätze, vier je Wort
    uint2 palette;
    // Erster Index des Teilnetzes im Indexpuffer seines Blocks (Rückfallpfad)
    uint first_index;
    uint flags;
    uint2 reserved;
};

// Wie render::GpuView (384 Bytes): eine Sicht, gegen die geprüft wird.
struct View
{
    // Ebenen des Stumpfs (kamerarelativ, Normalen nach innen, normiert)
    float4 planes[6];
    // Matrix dieses Frames und des letzten (für Hi-Z), Versatz vom Ursprung dieses Frames zu dem des
    // letzten (xyz)
    float4 view_projection[4];
    float4 previous_view_projection[4];
    float4 previous_offset;
    // Bindlose Indizes der Stufen des Hi-Z dieses und des letzten Frames
    uint4 hiz_current[4];
    uint4 hiz_previous[4];
    // x: Schalter (view_*), y: Zahl der Stufen, z, w: Größe der Stufe 0
    uint4 params;
};

static const uint view_cone = 1;
static const uint view_previous_hiz = 2;
static const uint view_phase_two = 4;

// Wie rhi::DrawIndexedCommand (20 Bytes).
struct DrawCommand
{
    uint index_count;
    uint instance_count;
    uint first_index;
    int vertex_offset;
    uint first_instance;
};

// Wie render::detail::DrivenPush (104 Bytes), gemeinsam für alle Stufen.
struct DrivenPushT<V>
{
    Frame* frame;
    InstanceT<V>* instances;
    // Je Gruppe: Instanz, erstes Meshlet in der Instanz, Anfang des Bereichs der Befehle, Zähler
    uint4* groups;
    View* view;
    Material* materials;
    TerrainMaterial* terrain_materials;
    DrawCommand* commands;
    uint* counters;
    // Zähler der Sicht: geprüft, Stumpf, Kegel, verdeckt, gezeichnet, Dreiecke
    uint* stats;
    uint first_group;
    uint command_base;
    uint counter_base;
    // Nur Schatten: die Kaskade, in die gezeichnet wird
    uint cascade;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint reserved3;
};

// Was der Task-Shader an die Mesh-Shader seiner Gruppe gibt.
struct Payload
{
    uint instance;
    uint meshlets[group_meshlets];
};

// Lokale Indizes eines Dreiecks aus seinem Wort.
uint3 unpack_triangle(uint packed)
{
    return uint3(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF);
}

uint meshlet_vertex_count(Meshlet m)
{
    return m.counts & 0xFF;
}

uint meshlet_triangle_count(Meshlet m)
{
    return (m.counts >> 8) & 0xFF;
}
