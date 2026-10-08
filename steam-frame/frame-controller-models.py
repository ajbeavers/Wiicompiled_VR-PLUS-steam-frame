#!/usr/bin/env python3
"""Converts SteamVR's Steam Frame controller render models into the game's controller model format.

The game draws a controller model in each hand in its VR menus and tutorials. Upstream ships Meta's
Touch Plus models (runtime/assets/quest_touch_plus, from WebXR Input Profiles) and, on Windows, asks
SteamVR for the real controller's model; under Lepton that SteamVR interface is not available, so
on the Frame you get Quest controllers. This reads the Frame controller models that SteamVR's own
frame_controller driver keeps on every Frame and writes them in the same format, `WCC1`, so the
launch wrapper can put them in place of the Touch Plus files. Valve's models are only read from
your own SteamVR install, never redistributed.

What is converted, per hand:
  - every visible component mesh (body, sticks, buttons, trigger, grips, bumper), triangulated, in
    the OpenXR grip space the game draws controller models in (the model's `openxr_grip` component);
  - the base colour texture with its ambient occlusion baked in, at 1024x1024;
  - the nine motions the game animates, as SteamVR's model describes them: thumbstick click,
    primary and secondary button (A and B on the right; the left Frame controller has none), trigger,
    grip, and the thumbstick tilting left, right, back and forward;
  - five landmarks the tutorials point at: thumbstick, primary, secondary, trigger, grip (the D-pad
    and View button stand in for primary and secondary on the left).

usage: frame-controller-models.py OUTDIR [SteamVR rendermodels dir]
Needs Python 3 and GdkPixbuf (python3-gobject), both on SteamOS. No numpy or PIL.
"""
import json
import math
import os
import struct
import sys

RENDERMODELS = '/opt/steamvr/drivers/frame_controller/resources/rendermodels'
TEXTURE_SIZE = 1024


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

    marks = [stick, primary or 'dpad', secondary or 'button_view', 'trigger', 'button_grip']
    anchors = []
    for key in marks:
        anchors += to_grip(centroid(key)) if key in meshes else [0.0, 0.0, 0.0]

    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, f'{hand}.wccontroller'), 'wb') as f:
        f.write(struct.pack('<4sII', b'WCC1', len(records), 9))
        f.write(struct.pack('<15f', *anchors))
        f.write(b''.join(records))
    with open(os.path.join(outdir, f'{hand}.rgba'), 'wb') as f:
        f.write(texture(directory, name))
    print(f'{hand}: {len(records) // 3} triangles from {len(meshes)} components')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else RENDERMODELS
    if not os.path.isdir(src):
        sys.exit(f'{src}: no Steam Frame controller models here (is SteamVR installed?)')
    for hand in ('left', 'right'):
        convert(hand, src, out)
