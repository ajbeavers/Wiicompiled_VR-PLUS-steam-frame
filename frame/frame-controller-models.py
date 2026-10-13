#!/usr/bin/env python3
"""Converts SteamVR's Steam Frame controller render models into the game's controller model format.

The native Linux build draws a controller model in each hand in its VR menus and tutorials. It never
asks OpenVR for Valve's full render models (about 93k vertices re-animated on the CPU every menu
frame dropped menus to 45 fps). Instead this script reads the Frame controller models that SteamVR's
own frame_controller driver keeps on every Frame, simplifies them, and writes them in the game's
`WCC1` format into the user data directory; the game loads them from there when present and draws
procedural controllers otherwise. Valve's models are only read from your own SteamVR install, never
redistributed or committed.

What is converted, per hand:
  - every visible component mesh (body, sticks, buttons, trigger, grips, bumper), triangulated, in
    the OpenXR grip space the game draws controller models in (the model's `openxr_grip` component),
    simplified toward 4000 triangles per hand (KF_FRAME_MODEL_TRIANGLES; texture seams leave the
    right hand at about 4800);
  - the base colour texture with its ambient occlusion baked in, at 1024x1024;
  - the nine motions the game animates: thumbstick click, primary and secondary button (A and B on
    the right; the left Frame controller has none), trigger, grip, and the thumbstick tilting left,
    right, back and forward;
  - five landmarks the tutorials point at: thumbstick, primary, secondary, trigger, grip (the D-pad
    and View button stand in for primary and secondary on the left).

usage: frame-controller-models.py [OUTDIR [SteamVR rendermodels dir]]
OUTDIR defaults to $KARTFRAME_HOME/UserData/controller_models/frame (KARTFRAME_HOME defaults to
~/.local/share/KartFrame), where the game looks for them.
Needs Python 3 and GdkPixbuf (python3-gobject), both on SteamOS. No numpy or PIL.
"""
import json
import math
import os
import struct
import sys

RENDERMODELS = '/opt/steamvr/drivers/frame_controller/resources/rendermodels'
TEXTURE_SIZE = 1024
TRIANGLES = int(os.environ.get('KF_FRAME_MODEL_TRIANGLES', '4000'))  # per hand


# --- small vector maths -------------------------------------------------------------------------

def sub(a, b): return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
def add(a, b): return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
def scale(a, s): return [a[0] * s, a[1] * s, a[2] * s]
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def norm(a):
    n = math.sqrt(dot(a, a))
    return [a[0] / n, a[1] / n, a[2] / n] if n else [0.0, 0.0, 0.0]


def rotate(p, pivot, axis, degrees):
    """Rodrigues: p rotated about the line through pivot along axis (right-handed)."""
    k = norm(axis)
    v = sub(p, pivot)
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    r = add(add(scale(v, c), scale(cross(k, v), s)), scale(k, dot(k, v) * (1 - c)))
    return add(r, pivot)


def euler_xyz(degrees):
    """3x3 rotation for SteamVR's rotate_xyz (degrees), applied X then Y then Z; rows of the matrix."""
    x, y, z = (math.radians(d) for d in degrees)
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    mul = lambda a, b: [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return mul(rz, mul(ry, rx))


# --- input files --------------------------------------------------------------------------------

def read_obj(path):
    """Triangles of an OBJ as lists of (position, uv, normal) corners."""
    vs, vts, vns, tris = [], [], [], []
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == 'v':
            vs.append([float(x) for x in parts[1:4]])
        elif parts[0] == 'vt':
            vts.append([float(x) for x in parts[1:3]])
        elif parts[0] == 'vn':
            vns.append([float(x) for x in parts[1:4]])
        elif parts[0] == 'f':
            corners = []
            for ref in parts[1:]:
                i = (ref.split('/') + ['', ''])[:3]
                corners.append((vs[int(i[0]) - 1], vts[int(i[1]) - 1] if i[1] else [0.0, 0.0],
                                vns[int(i[2]) - 1] if i[2] else [0.0, 1.0, 0.0]))
            for k in range(1, len(corners) - 1):  # fan
                tris.append([corners[0], corners[k], corners[k + 1]])
    return tris


def texture(directory, prefix):
    """Base colour times ambient occlusion, RGBA, TEXTURE_SIZE square."""
    import gi
    gi.require_version('GdkPixbuf', '2.0')
    from gi.repository import GdkPixbuf

    def load(name):
        pb = GdkPixbuf.Pixbuf.new_from_file(os.path.join(directory, f'{prefix}_{name}.png'))
        pb = pb.scale_simple(TEXTURE_SIZE, TEXTURE_SIZE, GdkPixbuf.InterpType.HYPER)
        return pb.get_pixels(), pb.get_rowstride(), pb.get_n_channels()

    color, cstride, cch = load('color')
    try:
        ao, astride, ach = load('ao')
    except Exception:
        ao = None
    out = bytearray(TEXTURE_SIZE * TEXTURE_SIZE * 4)
    for y in range(TEXTURE_SIZE):
        crow, o = y * cstride, y * TEXTURE_SIZE * 4
        arow = y * astride if ao is not None else 0
        for x in range(TEXTURE_SIZE):
            c = crow + x * cch
            a = ao[arow + x * ach] if ao is not None else 255
            out[o:o + 4] = bytes((color[c] * a // 255, color[c + 1] * a // 255, color[c + 2] * a // 255, 255))
            o += 4
    return struct.pack('<II', TEXTURE_SIZE, TEXTURE_SIZE) + bytes(out)


# --- motions ------------------------------------------------------------------------------------

def motion_moves(motion, up_hint):
    """Functions (point -> moved point) for the given motion at full deflection, keyed by what
    drives them: 'press', or for a joystick 'left', 'right', 'back', 'forward' and 'click'."""
    kind = motion['type']
    full = motion.get('value_mapping', [0.0, 0.0])[1]
    if kind == 'translate':
        d = scale(norm(motion['axis']), full)
        return {'press': lambda p: add(p, d)}
    if kind == 'rotate':
        return {'press': lambda p: rotate(p, motion['pivot'], motion['axis'], full)}
    if kind in ('joystick', 'dpad'):
        center = motion['center']
        up = norm(up_hint)
        # Tilting the stick top toward direction t is a rotation about up x t. Model space is
        # OpenXR's: +X right, +Y up, -Z forward.
        def toward(direction, degrees):
            t = norm(sub(direction, scale(up, dot(direction, up))))
            return lambda p: rotate(p, center, cross(up, t), degrees)
        ax = abs(motion.get('joystick_rotation_x', [20, -20])[0])
        ay = abs(motion.get('joystick_rotation_y', [20, -20])[0])
        frame = euler_xyz(motion.get('rotate_xyz', [0, 0, 0]))
        press = motion.get('press_translate', [0.0, 0.0, 0.0])
        press_model = [sum(frame[i][j] * press[j] for j in range(3)) for i in range(3)]
        return {'left': toward([-1, 0, 0], ax), 'right': toward([1, 0, 0], ax),
                'back': toward([0, 0, 1], ay), 'forward': toward([0, 0, -1], ay),
                'click': lambda p: add(p, press_model)}
    return {}


# --- simplification -----------------------------------------------------------------------------

def simplify(tris, target):
    """Quadric error half-edge collapse (Garland and Heckbert) down to about `target` triangles.

    Only original vertices survive, so their texture coordinates and normals carry over. Vertices on
    a texture seam (one position, several UVs), on an open edge or on a non-manifold edge never
    move, which keeps the texture charts and the component's outline intact; the price is that a
    heavily seamed mesh may stop above the target."""
    import heapq
    from collections import defaultdict

    # Weld corners into shared positions; keep each face's own corner attributes.
    ids, P, faces, attrs = {}, [], [], []
    for tri in tris:
        f = []
        for pos, uv, n in tri:
            key = (round(pos[0], 7), round(pos[1], 7), round(pos[2], 7))
            if key not in ids:
                ids[key] = len(P)
                P.append(pos)
            f.append(ids[key])
        if len(set(f)) == 3:
            faces.append(f)
            attrs.append([(uv, n) for _, uv, n in tri])
    if len(faces) <= target:
        return tris

    uv_sets = defaultdict(set)
    edge_count = defaultdict(int)
    vfaces = defaultdict(set)
    for fi, f in enumerate(faces):
        for k in range(3):
            uv = attrs[fi][k][0]
            uv_sets[f[k]].add((round(uv[0], 5), round(uv[1], 5)))
            a, b = f[k], f[(k + 1) % 3]
            edge_count[(min(a, b), max(a, b))] += 1
            vfaces[f[k]].add(fi)
    locked = {p for p, s in uv_sets.items() if len(s) > 1}
    for (a, b), c in edge_count.items():
        if c != 2:
            locked.update((a, b))

    def plane(f):
        a, b, c = P[f[0]], P[f[1]], P[f[2]]
        n = cross(sub(b, a), sub(c, a))
        area = math.sqrt(dot(n, n))
        if area == 0:
            return None, 0.0
        n = scale(n, 1 / area)
        return n + [-dot(n, a)], area

    Q = defaultdict(lambda: [0.0] * 10)
    for f in faces:
        p, w = plane(f)
        if p is None:
            continue
        a, b, c, d = p
        q = [a * a, a * b, a * c, a * d, b * b, b * c, b * d, c * c, c * d, d * d]
        for v in f:
            Q[v] = [x + w * y for x, y in zip(Q[v], q)]

    def error(q, p):
        x, y, z = p
        return (q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y
                + 2 * q[5] * y * z + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9])

    alive = [True] * len(faces)
    live = len(faces)
    version = defaultdict(int)
    heap = []

    def neighbours(v):
        return {w for fi in vfaces[v] for w in faces[fi] if w != v}

    def push(u, v):
        if u in locked:
            return
        q = [x + y for x, y in zip(Q[u], Q[v])]
        heapq.heappush(heap, (error(q, P[v]), u, v, version[u], version[v]))

    for (a, b) in edge_count:
        push(a, b)
        push(b, a)

    def face_normal(pts):
        return cross(sub(pts[1], pts[0]), sub(pts[2], pts[0]))

    while live > target and heap:
        _, u, v, vu, vv = heapq.heappop(heap)
        if version[u] != vu or version[v] != vv or u in locked or not vfaces[u] or not vfaces[v]:
            continue
        shared = vfaces[u] & vfaces[v]
        if len(shared) != 2:
            continue
        # Link condition: the edge's two opposite vertices are the only common neighbours.
        if len(neighbours(u) & neighbours(v)) != 2:
            continue
        # No flipped or collapsed triangles around u.
        ok = True
        for fi in vfaces[u] - shared:
            before = [P[w] for w in faces[fi]]
            after = [P[v] if w == u else P[w] for w in faces[fi]]
            nb, na = face_normal(before), face_normal(after)
            la, lb = math.sqrt(dot(na, na)), math.sqrt(dot(nb, nb))
            if la < 1e-12 or lb < 1e-12 or dot(na, nb) / (la * lb) < 0.3:
                ok = False
                break
        if not ok:
            continue
        # v's attributes in the chart the collapsed faces belong to.
        sf = next(iter(shared))
        v_attr = attrs[sf][faces[sf].index(v)]
        for fi in shared:
            alive[fi] = False
            live -= 1
            for w in faces[fi]:
                vfaces[w].discard(fi)
        for fi in list(vfaces[u]):
            k = faces[fi].index(u)
            faces[fi][k] = v
            attrs[fi][k] = v_attr
            vfaces[v].add(fi)
        vfaces[u].clear()
        Q[v] = [x + y for x, y in zip(Q[u], Q[v])]
        version[v] += 1
        ring = neighbours(v)
        for w in ring:
            version[w] += 1
        for w in ring:
            push(v, w)
            push(w, v)

    return [[(P[faces[fi][k]], attrs[fi][k][0], attrs[fi][k][1]) for k in range(3)]
            for fi in range(len(faces)) if alive[fi]]


def convert(hand, rendermodels, outdir):
    name = f'frame_controller_{hand}'
    directory = os.path.join(rendermodels, name)
    components = json.load(open(os.path.join(directory, name + '.json')))['components']

    grip = components.get('openxr_grip') or components.get('handgrip')
    grip_origin = grip['component_local']['origin']
    grip_rot = euler_xyz(grip['component_local'].get('rotate_xyz', [0, 0, 0]))
    to_grip = lambda p: [sum(grip_rot[j][i] * (p[j] - grip_origin[j]) for j in range(3)) for i in range(3)]
    dir_to_grip = lambda d: [sum(grip_rot[j][i] * d[j] for j in range(3)) for i in range(3)]

    meshes = {}
    for key, c in components.items():
        if 'filename' in c and c.get('visibility', {}).get('default', True):
            meshes[key] = read_obj(os.path.join(directory, c['filename']))

    def centroid(key):
        pts = [corner[0] for tri in meshes[key] for corner in tri]
        return [sum(p[i] for p in pts) / len(pts) for i in range(3)]

    # Which component each of the game's nine controls moves, and how.
    primary, secondary = ('button_a', 'button_b') if hand == 'right' else (None, None)
    controls = [None] * 9  # (component, motion key)
    stick = 'thumbstick'
    stick_motion = motion_moves(components[stick]['motion'], sub(centroid(stick), components[stick]['motion']['center']))
    controls[0] = (stick, stick_motion['click'])
    if primary in meshes:
        controls[1] = (primary, motion_moves(components[primary]['motion'], None)['press'])
    if secondary in meshes:
        controls[2] = (secondary, motion_moves(components[secondary]['motion'], None)['press'])
    controls[3] = ('trigger', motion_moves(components['trigger']['motion'], None)['press'])
    controls[4] = ('button_grip', motion_moves(components['button_grip']['motion'], None)['press'])
    for slot, key in zip(range(5, 9), ('left', 'right', 'back', 'forward')):
        controls[slot] = (stick, stick_motion[key])

    # The landmarks from the full meshes, then the meshes cut down to the triangle budget, shared
    # between components by size with a floor so small buttons keep their shape. The game
    # re-animates every vertex on the CPU each frame a menu is up: the full models (~31,000
    # triangles for both hands) took the menus from 90 to 45-60 fps on the Frame; Touch Plus has
    # ~8,900 for both.
    marks = [stick, primary or 'dpad', secondary or 'button_view', 'trigger', 'button_grip']
    anchors = []
    for key in marks:
        anchors += to_grip(centroid(key)) if key in meshes else [0.0, 0.0, 0.0]
    # Largest first: the body's seams can stop it above its share, and the rest absorb that.
    remaining, left_to_do = TRIANGLES, sum(len(t) for t in meshes.values())
    for key in sorted(meshes, key=lambda k: -len(meshes[k])):
        n = len(meshes[key])
        meshes[key] = simplify(meshes[key], max(min(n, 60), round(n * remaining / left_to_do)))
        remaining -= len(meshes[key])
        left_to_do -= n

    records = []
    for key, tris in meshes.items():
        for tri in tris:
            for position, uv, normal in tri:
                base = to_grip(position)
                n = norm(dir_to_grip(normal))
                shade = .8 + .2 * min(1.0, max(0.0, n[1] * .5 + .5))
                deltas = []
                for control in controls:
                    if control and control[0] == key:
                        deltas += sub(to_grip(control[1](position)), base)
                    else:
                        deltas += [0.0, 0.0, 0.0]
                records.append(struct.pack('<35f', *base, shade, shade, shade, uv[0], 1.0 - uv[1], *deltas))
    if len(records) > 100000:
        sys.exit(f'{name}: {len(records)} vertices, more than the game accepts (100000)')

    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, f'{hand}.wccontroller'), 'wb') as f:
        f.write(struct.pack('<4sII', b'WCC1', len(records), 9))
        f.write(struct.pack('<15f', *anchors))
        f.write(b''.join(records))
    with open(os.path.join(outdir, f'{hand}.rgba'), 'wb') as f:
        f.write(texture(directory, name))
    print(f'{hand}: {len(records) // 3} triangles from {len(meshes)} components')


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] in ('-h', '--help'):
        sys.exit(__doc__)
    home = os.environ.get('KARTFRAME_HOME') or os.path.expanduser('~/.local/share/KartFrame')
    data = os.path.join(home, 'UserData')
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(data, 'controller_models', 'frame')
    os.makedirs(out, exist_ok=True)
    src = sys.argv[2] if len(sys.argv) > 2 else RENDERMODELS
    if not os.path.isdir(src):
        sys.exit(f'{src}: no Steam Frame controller models here (is SteamVR installed?)')
    for hand in ('left', 'right'):
        convert(hand, src, out)
