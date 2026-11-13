// Prüfen der Meshlets auf der GPU (R3.5, ADR 0043): je Arbeitsgruppe bis zu 32 Meshlets einer
// Instanz gegen den Stumpf der Sicht, ihren Normalenkegel und das Hi-Z. Zwei Einstiege mit derselben
// Prüfung: cull_task (Task-Shader, startet je sichtbarem Meshlet einen Mesh-Shader) und cull_compute
// (Rückfallpfad, schreibt je sichtbarem Meshlet einen indirekten Draw).
//
// Verdeckung in zwei Phasen: Phase 1 prüft gegen das Hi-Z des letzten Frames (mit dessen Matrix)
// und zeichnet, was dort sichtbar war. Aus der Tiefe danach entsteht das Hi-Z dieses Frames. Phase 2
// prüft nur, was Phase 1 als verdeckt verworfen hat, gegen das neue Hi-Z und zeichnet, was jetzt
// sichtbar ist. So fehlt nichts, auch wenn sich die Kamera schnell dreht, und was hinter Bergen
// liegt, wird nie gezeichnet.

#include "driven.slang"

[[vk::push_constant]]
ConstantBuffer<DrivenPushT<uint>> push;

typealias Instance = InstanceT<uint>;

static const uint stat_tested = 0;
static const uint stat_frustum = 1;
static const uint stat_cone = 2;
static const uint stat_occluded = 3;
static const uint stat_drawn = 4;
static const uint stat_triangles = 5;

float4 clip_of(float4 rows[4], float3 p)
{
    let w = float4(p, 1.0);
    return float4(dot(rows[0], w), dot(rows[1], w), dot(rows[2], w), dot(rows[3], w));
}

float hiz_texel(uint4 levels[4], uint level, int2 texel)
{
    let index = levels[level / 4][level % 4];
    return bindless_textures[NonUniformResourceIndex(index)].Load(int3(texel, 0)).x;
}

// Ist die Kugel sicher hinter dem, was im Hi-Z steht? Umgekehrte Tiefe: Das Hi-Z hält je Texel die
// fernste Tiefe (kleinster Wert), die Kugel ihre nächste (größter Wert). Teile außerhalb des Bildes
// oder vor der Kamera gelten als sichtbar.
bool occluded(float3 center, float radius, float4 rows[4], uint4 levels[4], uint4 params)
{
    let level_count = params.y;
    if (level_count == 0)
    {
        return false;
    }
    var low = float2(1e30, 1e30);
    var high = float2(-1e30, -1e30);
    var nearest = 0.0;
    for (uint i = 0; i < 8; ++i)
    {
        let corner = center + float3((i & 1) != 0 ? radius : -radius, (i & 2) != 0 ? radius : -radius,
                                     (i & 4) != 0 ? radius : -radius);
        let clip = clip_of(rows, corner);
        if (clip.w <= 1e-4)
        {
            return false;
        }
        let ndc = clip.xyz / clip.w;
        low = min(low, ndc.xy);
        high = max(high, ndc.xy);
        nearest = max(nearest, ndc.z);
    }
    // Der Viewport zeigt mit y nach oben (begin_rendering), Texel zählen von oben.
    let uv_low = float2(low.x * 0.5 + 0.5, 0.5 - high.y * 0.5);
    let uv_high = float2(high.x * 0.5 + 0.5, 0.5 - low.y * 0.5);
    if (any(uv_low < 0.0) || any(uv_high > 1.0))
    {
        return false;
    }
    let size0 = params.zw;
    let extent = (uv_high - uv_low) * float2(size0);
    let level = uint(ceil(log2(max(max(extent.x, extent.y), 1.0))));
    if (level >= level_count)
    {
        return false;
    }
    let size = max(size0 >> level, uint2(1, 1));
    let last = int2(size) - 1;
    let a = min(int2(uv_low * float2(size)), last);
    let b = min(int2(uv_high * float2(size)), last);
    let farthest = min(min(hiz_texel(levels, level, a), hiz_texel(levels, level, int2(b.x, a.y))),
                       min(hiz_texel(levels, level, int2(a.x, b.y)), hiz_texel(levels, level, b)));
    return nearest < farthest;
}

// Ergebnis der Prüfung eines Meshlets.
static const uint cull_visible = 0;
static const uint cull_frustum = 1;
static const uint cull_cone = 2;
static const uint cull_occluded = 3;
// Phase 2: schon in Phase 1 gezeichnet oder weiter verdeckt.
static const uint cull_done = 4;

uint test_meshlet(Instance instance, Meshlet m, View view)
{
    let c0 = float3(instance.model[0].x, instance.model[1].x, instance.model[2].x);
    let c1 = float3(instance.model[0].y, instance.model[1].y, instance.model[2].y);
    let c2 = float3(instance.model[0].z, instance.model[1].z, instance.model[2].z);
    let scale = max(length(c0), max(length(c1), length(c2)));
    let center = transform_point(instance.model, m.sphere.xyz);
    let radius = m.sphere.w * scale;

    for (uint i = 0; i < 6; ++i)
    {
        if (dot(view.planes[i].xyz, center) + view.planes[i].w < -radius)
        {
            return cull_frustum;
        }
    }

    // Normalenkegel; der Betrachter steht im Ursprung (kamerarelativ).
    let flags = view.params.x;
    if ((flags & view_cone) != 0 && (instance.flags & instance_double_sided) == 0 && m.cone.w < 1.0)
    {
        let axis = normalize(cross(c1, c2) * m.cone.x + cross(c2, c0) * m.cone.y +
                             cross(c0, c1) * m.cone.z);
        if (dot(center, axis) >= m.cone.w * length(center) + radius)
        {
            return cull_cone;
        }
    }

    let before = (flags & view_previous_hiz) != 0 &&
                 occluded(center + view.previous_offset.xyz, radius, view.previous_view_projection,
                          view.hiz_previous, view.params);
    if ((flags & view_phase_two) == 0)
    {
        return before ? cull_occluded : cull_visible;
    }
    if (!before)
    {
        return cull_done;
    }
    return occluded(center, radius, view.view_projection, view.hiz_current, view.params) ? cull_done
                                                                                        : cull_visible;
}

groupshared uint visible_count;
groupshared uint group_stats[6];
groupshared Payload payload;

void count(uint stat, uint amount)
{
    if (amount != 0)
    {
        InterlockedAdd(group_stats[stat], amount);
    }
}

// Prüft das Meshlet dieses Threads und zählt; liefert, ob es gezeichnet wird.
bool cull_thread(uint4 group, uint thread, out Instance instance, out Meshlet m, out uint index)
{
    instance = push.instances[group.x];
    let local = group.y + thread;
    // Die letzte Gruppe einer Instanz hat weniger als 32 Meshlets.
    index = instance.first_meshlet + min(local, instance.meshlet_count - 1);
    m = instance.meshlets[index];
    if (local >= instance.meshlet_count)
    {
        return false;
    }
    let view = *push.view;
    let result = test_meshlet(instance, m, view);
    let phase_two = (view.params.x & view_phase_two) != 0;
    if (!phase_two)
    {
        count(stat_tested, 1);
        count(stat_frustum, result == cull_frustum ? 1 : 0);
        count(stat_cone, result == cull_cone ? 1 : 0);
        count(stat_occluded, result == cull_occluded ? 1 : 0);
    }
    if (result != cull_visible)
    {
        return false;
    }
    count(stat_drawn, 1);
    count(stat_triangles, meshlet_triangle_count(m));
    if (phase_two)
    {
        // In Phase 1 als verdeckt gezählt, jetzt doch sichtbar.
        count(stat_occluded, 0xFFFFFFFF);
    }
    return true;
}

void begin_group(uint thread)
{
    if (thread == 0)
    {
        visible_count = 0;
    }
    if (thread < 6)
    {
        group_stats[thread] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
}

void end_group(uint thread)
{
    GroupMemoryBarrierWithGroupSync();
    if (thread < 6 && group_stats[thread] != 0)
    {
        InterlockedAdd(push.stats[thread], group_stats[thread]);
    }
}

[shader("amplification")]
[numthreads(32, 1, 1)]
void cull_task(uint thread : SV_GroupThreadID, uint group_id : SV_GroupID)
{
    let group = push.groups[push.first_group + group_id];
    begin_group(thread);
    Instance instance;
    Meshlet m;
    uint index;
    if (cull_thread(group, thread, instance, m, index))
    {
        uint slot;
        InterlockedAdd(visible_count, 1, slot);
        payload.meshlets[slot] = index;
    }
    if (thread == 0)
    {
        payload.instance = group.x;
    }
    end_group(thread);
    DispatchMesh(visible_count, 1, 1, payload);
}

[shader("compute")]
[numthreads(32, 1, 1)]
void cull_compute(uint thread : SV_GroupThreadID, uint group_id : SV_GroupID)
{
    let group = push.groups[push.first_group + group_id];
    begin_group(thread);
    Instance instance;
    Meshlet m;
    uint index;
    if (cull_thread(group, thread, instance, m, index))
    {
        uint slot;
        InterlockedAdd(push.counters[push.counter_base + group.w], 1, slot);
        DrawCommand command;
        command.index_count = meshlet_triangle_count(m) * 3;
        command.instance_count = 1;
        command.first_index = instance.first_index + m.first_triangle * 3;
        command.vertex_offset = 0;
        command.first_instance = group.x;
        push.commands[push.command_base + group.z + slot] = command;
    }
    end_group(thread);
}
