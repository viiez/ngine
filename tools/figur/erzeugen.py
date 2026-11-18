#!/usr/bin/env python3
"""Erzeugt die Gliederpuppe der Demo (R4.6) unter assets/figuren.

Die Puppe ist ein eigenes Werk unter der Lizenz des Projekts (Abstimmung zu R4.6): Glieder aus
Quadern, ein Skelett mit 18 Gelenken, weich gewichtet an den Gelenken, und Clips aus Sinuskurven und
Schlüsselposen. Eine echte Figur kann sie ersetzen: Datei austauschen, .meta behalten, Clips gleich
benennen, dann bleiben Prefab und Automat gültig.

Aufruf aus der Wurzel des Repositorys:  python3 tools/figur/erzeugen.py
Bestehende .meta-Dateien bleiben, neue bekommen eine zufällige Kennung. Die Puppe schaut entlang
-z (wie die Spielfigur), +y ist oben, der Ursprung liegt zwischen den Füßen auf dem Boden.
"""

import json
import math
import os
import secrets
import struct

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "figuren")
RATE = 30


def meta(path):
    """Kennung aus der .meta-Datei, sonst neue .meta mit zufälliger Kennung."""
    meta_path = path + ".meta"
    if os.path.exists(meta_path):
        with open(meta_path, encoding="utf-8") as f:
            return json.load(f)["id"]
    asset_id = secrets.randbits(63) or 1
    with open(meta_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps({"format": "ngine.meta", "version": 1, "id": asset_id}, indent=2) + "\n")
    return asset_id


# --- Skelett: Name, Elternteil, Lage relativ zum Elternteil in der Bindepose ---

JOINTS = [
    ("Wurzel", None, (0.0, 0.0, 0.0)),
    ("Becken", "Wurzel", (0.0, 0.95, 0.0)),
    ("Bauch", "Becken", (0.0, 0.12, 0.0)),
    ("Brust", "Bauch", (0.0, 0.2, 0.0)),
    ("Hals", "Brust", (0.0, 0.22, 0.0)),
    ("Kopf", "Hals", (0.0, 0.1, 0.0)),
]
for side, x in (("L", 1.0), ("R", -1.0)):
    JOINTS += [
        (f"Oberarm_{side}", "Brust", (0.21 * x, 0.18, 0.0)),
        (f"Unterarm_{side}", f"Oberarm_{side}", (0.0, -0.29, 0.0)),
        (f"Hand_{side}", f"Unterarm_{side}", (0.0, -0.26, 0.0)),
        (f"Oberschenkel_{side}", "Becken", (0.1 * x, -0.04, 0.0)),
        (f"Unterschenkel_{side}", f"Oberschenkel_{side}", (0.0, -0.44, 0.0)),
        (f"Fuss_{side}", f"Unterschenkel_{side}", (0.0, -0.42, 0.0)),
    ]
INDEX = {name: i for i, (name, _, _) in enumerate(JOINTS)}


def bind_position(name):
    x = y = z = 0.0
    while name is not None:
        _, parent, (dx, dy, dz) = JOINTS[INDEX[name]]
        x, y, z = x + dx, y + dy, z + dz
        name = parent
    return (x, y, z)


# --- Netz: je Glied ein Quader zwischen zwei Punkten, entlang der Länge unterteilt ---

# Glied: Gelenk, Anfang und Ende in Modellkoordinaten, Breite (x) und Tiefe (z), Farbe (Material),
# Gelenk, mit dem das obere Ende gemischt wird (oder None).
def limbs():
    out = []
    pelvis = bind_position("Becken")
    chest = bind_position("Brust")
    neck = bind_position("Hals")
    head = bind_position("Kopf")
    out.append(("Becken", (pelvis[0], pelvis[1] - 0.1, 0.0), (pelvis[0], pelvis[1] + 0.12, 0.0),
                0.34, 0.2, 1, None))
    out.append(("Bauch", (0.0, pelvis[1] + 0.12, 0.0), (0.0, chest[1], 0.0), 0.3, 0.18, 0, "Becken"))
    out.append(("Brust", (0.0, chest[1], 0.0), (0.0, neck[1], 0.0), 0.38, 0.22, 0, "Bauch"))
    out.append(("Hals", (0.0, neck[1], 0.0), (0.0, head[1], 0.0), 0.1, 0.1, 2, "Brust"))
    out.append(("Kopf", (0.0, head[1], 0.02), (0.0, head[1] + 0.24, 0.02), 0.2, 0.24, 2, None))
    for side in ("L", "R"):
        shoulder = bind_position(f"Oberarm_{side}")
        elbow = bind_position(f"Unterarm_{side}")
        wrist = bind_position(f"Hand_{side}")
        hip = bind_position(f"Oberschenkel_{side}")
        knee = bind_position(f"Unterschenkel_{side}")
        ankle = bind_position(f"Fuss_{side}")
        out.append((f"Oberarm_{side}", shoulder, elbow, 0.1, 0.1, 0, None))
        out.append((f"Unterarm_{side}", elbow, wrist, 0.085, 0.085, 2, f"Oberarm_{side}"))
        out.append((f"Hand_{side}", wrist, (wrist[0], wrist[1] - 0.16, wrist[2]), 0.07, 0.04, 2,
                    f"Unterarm_{side}"))
        out.append((f"Oberschenkel_{side}", hip, knee, 0.15, 0.15, 1, "Becken"))
        out.append((f"Unterschenkel_{side}", knee, ankle, 0.11, 0.11, 1, f"Oberschenkel_{side}"))
        out.append((f"Fuss_{side}", (ankle[0], 0.0, ankle[2] + 0.04),
                    (ankle[0], 0.08, ankle[2] - 0.08), 0.1, 0.26, 3, f"Unterschenkel_{side}"))
    return out


SEGMENTS = 4
BLEND = 0.35  # Anteil der Länge am oberen Ende, in dem mit dem Elternglied gemischt wird


def build_mesh():
    """Eckpunkte (Position, Normale, Gelenke, Gewichte) und Indizes je Material."""
    positions, normals, joints, weights = [], [], [], []
    indices = {0: [], 1: [], 2: [], 3: []}
    for joint, start, end, width, depth, material, parent in limbs():
        sx, sy, sz = start
        ex, ey, ez = end
        length = math.dist(start, end)
        # Achsen des Quaders: entlang (a), quer (b, x) und Tiefe (c).
        ax, ay, az = (ex - sx) / length, (ey - sy) / length, (ez - sz) / length
        bx, by, bz = 1.0, 0.0, 0.0
        # c = a × b, dann b = c × a (orthonormal).
        cx, cy, cz = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
        cl = math.sqrt(cx * cx + cy * cy + cz * cz)
        cx, cy, cz = cx / cl, cy / cl, cz / cl
        bx, by, bz = cy * az - cz * ay, cz * ax - cx * az, cx * ay - cy * ax
        hw, hd = width / 2.0, depth / 2.0
        a, b, c = (ax, ay, az), (bx, by, bz), (cx, cy, cz)

        def point(t, u, v):
            return tuple(start[k] + a[k] * length * t + b[k] * u + c[k] * v for k in range(3))

        def skin(t):
            # Oben (t = 0 ist der Anfang am Elterngelenk) weich mit dem Elternglied mischen.
            j = INDEX[joint]
            if parent is None or t >= BLEND:
                return (j, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)
            w = 0.5 + 0.5 * (t / BLEND)
            return (j, INDEX[parent], 0, 0), (w, 1.0 - w, 0.0, 0.0)

        def quad(corners, normal, ts):
            base = len(positions)
            for p, t in zip(corners, ts):
                positions.append(p)
                normals.append(normal)
                jt, wt = skin(t)
                joints.append(jt)
                weights.append(wt)
            indices[material].extend([base, base + 1, base + 2, base, base + 2, base + 3])

        neg = lambda v: (-v[0], -v[1], -v[2])
        # Vier Seiten entlang der Länge, je SEGMENTS Abschnitte.
        sides = [(b, c, hw, hd), (c, neg(b), hd, hw), (neg(b), neg(c), hw, hd), (neg(c), b, hd, hw)]
        for normal, tangent, offset, half in sides:
            for s in range(SEGMENTS):
                t0, t1 = s / SEGMENTS, (s + 1) / SEGMENTS

                def side_point(t, sign):
                    return tuple(start[k] + a[k] * length * t + normal[k] * offset +
                                 tangent[k] * half * sign for k in range(3))

                quad([side_point(t0, -1), side_point(t0, 1), side_point(t1, 1), side_point(t1, -1)],
                     normal, [t0, t0, t1, t1])
        # Deckel oben und unten.
        top = [point(0, -hw, -hd), point(0, -hw, hd), point(0, hw, hd), point(0, hw, -hd)]
        quad(top, neg(a), [0, 0, 0, 0])
        bottom = [point(1, -hw, -hd), point(1, hw, -hd), point(1, hw, hd), point(1, -hw, hd)]
        quad(bottom, a, [1, 1, 1, 1])
    return positions, normals, joints, weights, indices


# --- Clips ---

def quat_axis(axis, angle):
    s = math.sin(angle / 2.0)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(angle / 2.0))


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def rx(deg):
    return quat_axis((1.0, 0.0, 0.0), math.radians(deg))


def rz(deg):
    return quat_axis((0.0, 0.0, 1.0), math.radians(deg))


def ry(deg):
    return quat_axis((0.0, 1.0, 0.0), math.radians(deg))


# Positive Drehung um x beugt nach vorn (Richtung -z), weil Glieder nach unten hängen.
def gait(p, hip, knee, arm, elbow, bob, lean, crouch=0.0):
    """Pose eines Gangs zur normierten Zeit p (0 bis 1): Gelenk → Drehung, Becken → Höhe."""
    w = 2.0 * math.pi * p
    pose = {}
    for side, phase in (("L", 0.0), ("R", math.pi)):
        swing = math.sin(w + phase)
        # Knie beugt beim Vorschwingen (Fuß in der Luft) stärker.
        lift = max(0.0, math.sin(w + phase + math.pi / 2.0))
        pose[f"Oberschenkel_{side}"] = rx(hip * swing + crouch * 70.0)
        pose[f"Unterschenkel_{side}"] = rx(-(knee * lift + 8.0 + crouch * 110.0))
        pose[f"Fuss_{side}"] = rx(crouch * 40.0 + 6.0 * swing)
        sign = 1.0 if side == "L" else -1.0
        pose[f"Oberarm_{side}"] = quat_mul(rz(4.0 * sign), rx(-arm * swing))
        pose[f"Unterarm_{side}"] = rx(elbow + 10.0 * max(0.0, -swing))
    pose["Bauch"] = rx(lean * 0.5)
    pose["Brust"] = quat_mul(rx(lean * 0.5), ry(6.0 * math.sin(w)))
    pose["Becken"] = ry(-5.0 * math.sin(w))
    height = 0.95 - crouch * 0.42 + bob * math.cos(2.0 * w)
    return pose, height


def idle(p):
    w = 2.0 * math.pi * p
    pose = {
        "Brust": rx(1.5 * math.sin(w)),
        "Hals": rx(-1.0 * math.sin(w)),
        "Oberarm_L": rz(5.0), "Oberarm_R": rz(-5.0),
        "Unterarm_L": rx(8.0), "Unterarm_R": rx(8.0),
        "Unterschenkel_L": rx(-3.0), "Unterschenkel_R": rx(-3.0),
    }
    return pose, 0.95 - 0.005 + 0.005 * math.cos(w)


def mix(a, b, t):
    """Lineare Mischung zweier Posen (Drehungen über nlerp)."""
    pose = {}
    for name in set(a[0]) | set(b[0]):
        qa = a[0].get(name, (0.0, 0.0, 0.0, 1.0))
        qb = b[0].get(name, (0.0, 0.0, 0.0, 1.0))
        if sum(x * y for x, y in zip(qa, qb)) < 0.0:
            qb = tuple(-x for x in qb)
        q = tuple(x + (y - x) * t for x, y in zip(qa, qb))
        n = math.sqrt(sum(x * x for x in q))
        pose[name] = tuple(x / n for x in q)
    return pose, a[1] + (b[1] - a[1]) * t


def key_pose(name):
    """Schlüsselposen für Springen, Fallen und Landen."""
    if name == "stand":
        return idle(0.0)
    if name == "crouch":
        return ({"Oberschenkel_L": rx(55.0), "Oberschenkel_R": rx(55.0),
                 "Unterschenkel_L": rx(-90.0), "Unterschenkel_R": rx(-90.0),
                 "Fuss_L": rx(35.0), "Fuss_R": rx(35.0), "Bauch": rx(15.0),
                 "Oberarm_L": rx(-30.0), "Oberarm_R": rx(-30.0),
                 "Unterarm_L": rx(20.0), "Unterarm_R": rx(20.0)}, 0.7)
    if name == "stretch":
        return ({"Oberschenkel_L": rx(-5.0), "Oberschenkel_R": rx(-5.0),
                 "Fuss_L": rx(-25.0), "Fuss_R": rx(-25.0),
                 "Oberarm_L": quat_mul(rz(20.0), rx(150.0)),
                 "Oberarm_R": quat_mul(rz(-20.0), rx(150.0)),
                 "Unterarm_L": rx(10.0), "Unterarm_R": rx(10.0)}, 0.97)
    if name == "air":
        return ({"Oberschenkel_L": rx(35.0), "Oberschenkel_R": rx(10.0),
                 "Unterschenkel_L": rx(-50.0), "Unterschenkel_R": rx(-30.0),
                 "Oberarm_L": quat_mul(rz(55.0), rx(20.0)),
                 "Oberarm_R": quat_mul(rz(-55.0), rx(20.0)),
                 "Unterarm_L": rx(25.0), "Unterarm_R": rx(25.0)}, 0.93)
    raise KeyError(name)


def keyed(keys, p):
    """Pose zwischen Schlüsselposen [(Zeit 0..1, Name)] mit weichem Übergang."""
    for (t0, a), (t1, b) in zip(keys, keys[1:]):
        if t0 <= p <= t1:
            t = (p - t0) / (t1 - t0) if t1 > t0 else 0.0
            t = t * t * (3.0 - 2.0 * t)
            return mix(key_pose(a), key_pose(b), t)
    return key_pose(keys[-1][1])


def clip_functions():
    def flail(p):
        pose, height = key_pose("air")
        w = 2.0 * math.pi * p
        pose = dict(pose)
        pose["Oberarm_L"] = quat_mul(rz(55.0 + 10.0 * math.sin(w)), rx(20.0))
        pose["Oberarm_R"] = quat_mul(rz(-55.0 - 10.0 * math.sin(w)), rx(20.0))
        pose["Oberschenkel_L"] = rx(30.0 + 10.0 * math.sin(w))
        pose["Oberschenkel_R"] = rx(15.0 - 10.0 * math.sin(w))
        return pose, height

    return {
        # Name: (Dauer in Sekunden, Funktion der normierten Zeit, Schleife)
        "Stehen": (2.0, idle),
        "Gehen": (0.6, lambda p: gait(p, 32.0, 55.0, 25.0, 15.0, 0.02, 4.0)),
        "Laufen": (0.55, lambda p: gait(p, 48.0, 95.0, 45.0, 80.0, 0.04, 14.0)),
        "Ducken": (1.6, lambda p: mix(gait(0.0, 0.0, 0.0, 5.0, 25.0, 0.0, 25.0, crouch=1.0),
                                      idle(p), 0.0)),
        "Schleichen": (0.9, lambda p: gait(p, 20.0, 30.0, 12.0, 25.0, 0.01, 25.0, crouch=1.0)),
        "Springen": (0.3, lambda p: keyed([(0.0, "crouch"), (0.5, "stretch"), (1.0, "air")], p)),
        "Fallen": (0.8, flail),
        "Landen": (0.35, lambda p: keyed([(0.0, "air"), (0.35, "crouch"), (1.0, "stand")], p)),
    }


def sample_clip(duration, function):
    count = int(round(duration * RATE))
    times, rotations, heights = [], {name: [] for name, _, _ in JOINTS}, []
    for i in range(count + 1):
        p = i / count
        pose, height = function(p)
        times.append(i / RATE if i < count else duration)
        heights.append(height)
        for name, _, _ in JOINTS:
            rotations[name].append(pose.get(name, (0.0, 0.0, 0.0, 1.0)))
    return times, rotations, heights


# --- glTF schreiben ---

class Buffer:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def add(self, fmt, values, component, kind, count, target=None, minmax=None):
        while len(self.data) % 4:
            self.data.append(0)
        offset = len(self.data)
        self.data += struct.pack("<" + fmt * len(values), *values)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(self.data) - offset}
        if target:
            view["target"] = target
        self.views.append(view)
        accessor = {"bufferView": len(self.views) - 1, "componentType": component,
                    "count": count, "type": kind}
        if minmax:
            accessor["min"], accessor["max"] = minmax
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def flat(rows):
    return [x for row in rows for x in row]


def main():
    os.makedirs(ROOT, exist_ok=True)
    gltf_path = os.path.join(ROOT, "puppe.gltf")
    model_id = meta(gltf_path)

    positions, normals, joints, weights, indices = build_mesh()
    buffer = Buffer()
    lo = [min(p[k] for p in positions) for k in range(3)]
    hi = [max(p[k] for p in positions) for k in range(3)]
    count = len(positions)
    position_acc = buffer.add("f", flat(positions), 5126, "VEC3", count, 34962, (lo, hi))
    normal_acc = buffer.add("f", flat(normals), 5126, "VEC3", count, 34962)
    joint_acc = buffer.add("H", flat(joints), 5123, "VEC4", count, 34962)
    weight_acc = buffer.add("f", flat(weights), 5126, "VEC4", count, 34962)
    primitives = []
    for material, list_ in indices.items():
        if not list_:
            continue
        index_acc = buffer.add("I", list_, 5125, "SCALAR", len(list_), 34963)
        primitives.append({"attributes": {"POSITION": position_acc, "NORMAL": normal_acc,
                                          "JOINTS_0": joint_acc, "WEIGHTS_0": weight_acc},
                           "indices": index_acc, "material": material})

    inverse_bind = []
    for name, _, _ in JOINTS:
        x, y, z = bind_position(name)
        # Spaltenweise: Einheitsmatrix mit Verschiebung -Lage.
        inverse_bind += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1]
    ibm_acc = buffer.add("f", inverse_bind, 5126, "MAT4", len(JOINTS))

    nodes = []
    for name, parent, translation in JOINTS:
        node = {"name": name, "translation": list(translation)}
        children = [i for i, (_, p, _) in enumerate(JOINTS) if p == name]
        if children:
            node["children"] = children
        nodes.append(node)
    mesh_node = len(nodes)
    nodes.append({"name": "Puppe", "mesh": 0, "skin": 0})
    figure = len(nodes)
    nodes.append({"name": "Figur", "children": [INDEX["Wurzel"], mesh_node]})

    animations = []
    for clip, (duration, function) in clip_functions().items():
        times, rotations, heights = sample_clip(duration, function)
        time_acc = buffer.add("f", times, 5126, "SCALAR", len(times),
                              minmax=([times[0]], [times[-1]]))
        samplers, channels = [], []
        for name, _, _ in JOINTS:
            if name == "Wurzel":
                continue
            values = rotations[name]
            if all(abs(v[3] - 1.0) < 1e-7 for v in values):
                continue
            acc = buffer.add("f", flat(values), 5126, "VEC4", len(values))
            samplers.append({"input": time_acc, "output": acc, "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1,
                             "target": {"node": INDEX[name], "path": "rotation"}})
        acc = buffer.add("f", flat([(0.0, h, 0.0) for h in heights]), 5126, "VEC3", len(heights))
        samplers.append({"input": time_acc, "output": acc, "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1,
                         "target": {"node": INDEX["Becken"], "path": "translation"}})
        animations.append({"name": clip, "samplers": samplers, "channels": channels})

    gltf = {
        "asset": {"version": "2.0", "generator": "NGine tools/figur/erzeugen.py"},
        "scene": 0,
        "scenes": [{"nodes": [figure]}],
        "nodes": nodes,
        "meshes": [{"name": "Puppe", "primitives": primitives}],
        "materials": [
            {"name": "Stoff", "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.32, 0.18, 1.0],
                                                        "metallicFactor": 0.0,
                                                        "roughnessFactor": 0.85}},
            {"name": "Hose", "pbrMetallicRoughness": {"baseColorFactor": [0.18, 0.22, 0.3, 1.0],
                                                       "metallicFactor": 0.0,
                                                       "roughnessFactor": 0.9}},
            {"name": "Holz", "pbrMetallicRoughness": {"baseColorFactor": [0.78, 0.62, 0.45, 1.0],
                                                       "metallicFactor": 0.0,
                                                       "roughnessFactor": 0.6}},
            {"name": "Schuhe", "pbrMetallicRoughness": {"baseColorFactor": [0.12, 0.09, 0.07, 1.0],
                                                         "metallicFactor": 0.0,
                                                         "roughnessFactor": 0.7}},
        ],
        "skins": [{"name": "Skelett", "joints": list(range(len(JOINTS))),
                   "inverseBindMatrices": ibm_acc, "skeleton": INDEX["Wurzel"]}],
        "animations": animations,
        "buffers": [{"byteLength": len(buffer.data), "uri": "puppe.bin"}],
        "bufferViews": buffer.views,
        "accessors": buffer.accessors,
    }
    with open(os.path.join(ROOT, "puppe.bin"), "wb") as f:
        f.write(buffer.data)
    with open(gltf_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(gltf, indent=1) + "\n")

    graph_path = os.path.join(ROOT, "spielfigur.animgraph")
    meta(graph_path)
    graph = {
        "format": "ngine.animgraph",
        "version": 1,
        "model": model_id,
        "initial": "Boden",
        "parameters": [
            {"name": "Tempo", "type": "float", "value": 0.0},
            {"name": "AmBoden", "type": "bool", "value": True},
            {"name": "Geduckt", "type": "bool", "value": False},
            {"name": "Springen", "type": "trigger"},
        ],
        "states": [
            {"name": "Boden", "parameter": "Tempo",
             "blend": [{"clip": "Stehen", "value": 0.0}, {"clip": "Gehen", "value": 4.0},
                       {"clip": "Laufen", "value": 7.0}],
             "events": [{"time": 0.25, "name": "Fuss_links"},
                        {"time": 0.75, "name": "Fuss_rechts"}]},
            {"name": "Geduckt", "parameter": "Tempo",
             "blend": [{"clip": "Ducken", "value": 0.0}, {"clip": "Schleichen", "value": 2.0}],
             "events": [{"time": 0.25, "name": "Fuss_links"},
                        {"time": 0.75, "name": "Fuss_rechts"}]},
            {"name": "Springen", "clip": "Springen", "loop": False},
            {"name": "Fallen", "clip": "Fallen"},
            {"name": "Landen", "clip": "Landen", "loop": False},
        ],
        "transitions": [
            {"from": "*", "to": "Springen", "duration": 0.08,
             "conditions": [{"parameter": "Springen", "op": "trigger"}]},
            {"from": "Springen", "to": "Fallen", "duration": 0.2, "after_end": True},
            {"from": "Boden", "to": "Fallen", "duration": 0.25,
             "conditions": [{"parameter": "AmBoden", "op": "false"}]},
            {"from": "Geduckt", "to": "Fallen", "duration": 0.25,
             "conditions": [{"parameter": "AmBoden", "op": "false"}]},
            {"from": "Fallen", "to": "Landen", "duration": 0.08,
             "conditions": [{"parameter": "AmBoden", "op": "true"}]},
            {"from": "Landen", "to": "Boden", "duration": 0.2, "after_end": True},
            {"from": "Boden", "to": "Geduckt", "duration": 0.2,
             "conditions": [{"parameter": "Geduckt", "op": "true"}]},
            {"from": "Geduckt", "to": "Boden", "duration": 0.2,
             "conditions": [{"parameter": "Geduckt", "op": "false"}]},
        ],
    }
    with open(graph_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(graph, indent=2, ensure_ascii=False) + "\n")
    print(f"Puppe: {count} Eckpunkte, {len(JOINTS)} Gelenke, {len(animations)} Clips, "
          f"{len(buffer.data) // 1024} KB")


if __name__ == "__main__":
    main()
