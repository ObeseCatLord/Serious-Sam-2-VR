"""Generate the virtual steering wheel mesh with Blender (headless).

    blender -b --factory-startup -P tools/blender/steering_wheel.py -- \
        --header src/game/steering_wheel_mesh.hpp [--preview out.png --atlas atlas.png]

The mesh is original mod geometry. Its UVs address regions of the stock
HoverFighter texture (Models/Vehicles/HoverFighter/Textures/Fighter.tex),
which the game loads at runtime; no texture bytes are stored in source.
Local frame: X right, Y up, Z toward the driver; the rim lies in the XY plane.
Header UVs use the engine/D3D top-left origin.
"""
import argparse
import math
import sys

import bmesh
import bpy

ATLAS = 1024.0
STRIP = (8, 340, 68, 680)      # Brushed riveted strip: rim and column.
PANEL = (395, 425, 500, 512)   # Plain panel: spokes and hub sides.
DIAL = (946.0, 220.0, 60.0)    # Round three-groove dial with blue centre: hub cap.

RIM_RADIUS, RIM_TUBE = .175, .016
RIM_SEGMENTS, TUBE_SEGMENTS, STRIP_REPEATS = 48, 10, 4
HUB_RADIUS, HUB_FRONT, HUB_BACK, HUB_SEGMENTS = .05, .016, -.022, 32
SPOKE_ANGLES = (0.0, 180.0, 270.0)  # Right, left, bottom (degrees, CCW from +X).
SPOKE_WIDTH, SPOKE_THICK, SPOKE_DISH = .034, .012, .012
COLUMN_RADIUS, COLUMN_LENGTH, COLUMN_SEGMENTS = .021, .13, 16


def uv_rect(rect, s, t):
    x0, y0, x1, y1 = rect
    return ((x0 + (x1 - x0) * s) / ATLAS, (y0 + (y1 - y0) * t) / ATLAS)


class Builder:
    def __init__(self):
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new('UVMap')

    def face(self, points, uvs, smooth):
        verts = [self.bm.verts.new(p) for p in points]
        f = self.bm.faces.new(verts)
        f.smooth = smooth
        for loop, uv in zip(f.loops, uvs):
            loop[self.uv].uv = (uv[0], 1.0 - uv[1])  # Blender UV origin is bottom-left.
        return f

    def finish(self):
        bmesh.ops.remove_doubles(self.bm, verts=self.bm.verts, dist=1e-6)
        bmesh.ops.recalc_face_normals(self.bm, faces=self.bm.faces)
        mesh = bpy.data.meshes.new('SteeringWheel')
        self.bm.to_mesh(mesh)
        self.bm.free()
        return mesh


def rim(b):
    def point(a, t):
        c, s = math.cos(a), math.sin(a)
        r = RIM_RADIUS + RIM_TUBE * math.cos(t)
        return (r * c, r * s, RIM_TUBE * math.sin(t))
    per = RIM_SEGMENTS // STRIP_REPEATS
    for i in range(RIM_SEGMENTS):
        a0, a1 = 2 * math.pi * i / RIM_SEGMENTS, 2 * math.pi * (i + 1) / RIM_SEGMENTS
        t0, t1 = (i % per) / per, (i % per + 1) / per
        if (i // per) % 2:  # Mirror alternate repeats so strip ends meet without seams.
            t0, t1 = 1 - t0, 1 - t1
        for j in range(TUBE_SEGMENTS):
            b0, b1 = 2 * math.pi * j / TUBE_SEGMENTS, 2 * math.pi * (j + 1) / TUBE_SEGMENTS
            s0, s1 = j / TUBE_SEGMENTS, (j + 1) / TUBE_SEGMENTS
            b.face([point(a0, b0), point(a1, b0), point(a1, b1), point(a0, b1)],
                   [uv_rect(STRIP, s0, t0), uv_rect(STRIP, s0, t1),
                    uv_rect(STRIP, s1, t1), uv_rect(STRIP, s1, t0)], True)


def hub(b):
    cx, cy, r = DIAL
    front, back = [], []
    for i in range(HUB_SEGMENTS):
        a0, a1 = 2 * math.pi * i / HUB_SEGMENTS, 2 * math.pi * (i + 1) / HUB_SEGMENTS
        p = lambda a, z: (HUB_RADIUS * math.cos(a), HUB_RADIUS * math.sin(a), z)
        # Side band.
        b.face([p(a0, HUB_BACK), p(a1, HUB_BACK), p(a1, HUB_FRONT), p(a0, HUB_FRONT)],
               [uv_rect(PANEL, i / HUB_SEGMENTS, 1), uv_rect(PANEL, (i + 1) / HUB_SEGMENTS, 1),
                uv_rect(PANEL, (i + 1) / HUB_SEGMENTS, 0), uv_rect(PANEL, i / HUB_SEGMENTS, 0)], True)
        front.append(p(a0, HUB_FRONT))
        back.append(p(a0, HUB_BACK))
    # Front cap faces the driver and shows the dial; image Y grows downward.
    b.face(front, [((cx + r * math.cos(2 * math.pi * i / HUB_SEGMENTS)) / ATLAS,
                    (cy - r * math.sin(2 * math.pi * i / HUB_SEGMENTS)) / ATLAS)
                   for i in range(HUB_SEGMENTS)], False)
    b.face(list(reversed(back)), [uv_rect(PANEL, .5 + .5 * math.cos(-2 * math.pi * i / HUB_SEGMENTS),
                                          .5 + .5 * math.sin(-2 * math.pi * i / HUB_SEGMENTS))
                                  for i in range(HUB_SEGMENTS)], False)


def box(b, corners, rect):
    # corners: 8 points, bottom 0..3 then top 4..7 (same order).
    quads = [(0, 1, 2, 3), (7, 6, 5, 4), (0, 4, 5, 1), (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0)]
    for q in quads:
        b.face([corners[k] for k in q], [uv_rect(rect, 0, 0), uv_rect(rect, 1, 0),
                                         uv_rect(rect, 1, 1), uv_rect(rect, 0, 1)], False)


def spokes(b):
    inner, outer = HUB_RADIUS * .85, RIM_RADIUS - RIM_TUBE * .6
    for degrees in SPOKE_ANGLES:
        a = math.radians(degrees)
        d = (math.cos(a), math.sin(a))
        n = (-d[1], d[0])
        def at(radial, side, z):
            return (d[0] * radial + n[0] * side, d[1] * radial + n[1] * side, z)
        w, h = SPOKE_WIDTH / 2, SPOKE_THICK / 2
        zi, zo = -SPOKE_DISH, 0.0  # Dished toward the column like a car wheel.
        corners = [at(inner, -w, zi - h), at(outer, -w * .8, zo - h), at(outer, w * .8, zo - h), at(inner, w, zi - h),
                   at(inner, -w, zi + h), at(outer, -w * .8, zo + h), at(outer, w * .8, zo + h), at(inner, w, zi + h)]
        box(b, corners, PANEL)


def column(b):
    z0, z1 = HUB_BACK, HUB_BACK - COLUMN_LENGTH
    for i in range(COLUMN_SEGMENTS):
        a0, a1 = 2 * math.pi * i / COLUMN_SEGMENTS, 2 * math.pi * (i + 1) / COLUMN_SEGMENTS
        p = lambda a, z: (COLUMN_RADIUS * math.cos(a), COLUMN_RADIUS * math.sin(a), z)
        b.face([p(a0, z0), p(a0, z1), p(a1, z1), p(a1, z0)],
               [uv_rect(STRIP, i / COLUMN_SEGMENTS, 0), uv_rect(STRIP, i / COLUMN_SEGMENTS, .4),
                uv_rect(STRIP, (i + 1) / COLUMN_SEGMENTS, .4), uv_rect(STRIP, (i + 1) / COLUMN_SEGMENTS, 0)], True)


def build():
    b = Builder()
    rim(b)
    hub(b)
    spokes(b)
    column(b)
    mesh = b.finish()
    obj = bpy.data.objects.new('SteeringWheel', mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def triangles(obj):
    mesh = obj.data
    mesh.calc_loop_triangles()
    normals = mesh.corner_normals
    uv = mesh.uv_layers.active.data
    out = []
    for tri in mesh.loop_triangles:
        for loop in tri.loops:
            v = mesh.vertices[mesh.loops[loop].vertex_index].co
            n = normals[loop].vector
            u = uv[loop].uv
            out.append((v.x, v.y, v.z, n.x, n.y, n.z, u.x, 1.0 - u.y))
    return out


def literal(x):
    text = f'{x:.6g}'
    if '.' not in text and 'e' not in text:
        text += '.0'
    return text + 'f'


def write_header(path, corners):
    lines = ['// Generated by tools/blender/steering_wheel.py; do not edit.',
             '// Original mod geometry. UVs address the stock HoverFighter atlas loaded at runtime.',
             '#pragma once', '#include <cstddef>', '', 'namespace ss2vr::game {',
             'struct SteeringWheelVertex { float x, y, z, nx, ny, nz, u, v; };',
             f'inline constexpr std::size_t SteeringWheelVertexCount = {len(corners)};',
             f'inline constexpr float SteeringWheelRimRadius = {literal(RIM_RADIUS)}, SteeringWheelRimTube = {literal(RIM_TUBE)};',
             'inline constexpr SteeringWheelVertex SteeringWheelMesh[SteeringWheelVertexCount] {']
    for c in corners:
        lines.append('    {' + ','.join(literal(x) for x in c) + '},')
    lines += ['};', '} // namespace ss2vr::game', '']
    with open(path, 'w') as f:
        f.write('\n'.join(lines))


def preview(obj, out, atlas):
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE' if 'BLENDER_EEVEE' in {e.identifier for e in
        bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items} else 'CYCLES'
    scene.render.resolution_x, scene.render.resolution_y = 900, 700
    mat = bpy.data.materials.new('Fighter')
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    tex = nodes.new('ShaderNodeTexImage')
    tex.image = bpy.data.images.load(atlas)
    tex.interpolation = 'Closest' if False else 'Linear'
    bsdf = nodes['Principled BSDF']
    mat.node_tree.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
    bsdf.inputs['Metallic'].default_value = .4
    bsdf.inputs['Roughness'].default_value = .5
    obj.data.materials.append(mat)
    # Blender is Z-up; rotate the Y-up wheel so it faces the camera.
    obj.rotation_euler = (math.radians(90 - 25), 0, 0)
    cam_data = bpy.data.cameras.new('Camera')
    cam = bpy.data.objects.new('Camera', cam_data)
    scene.collection.objects.link(cam)
    cam.location = (0, -.70, .36)
    cam.rotation_euler = (math.radians(66), 0, 0)
    scene.camera = cam
    light_data = bpy.data.lights.new('Key', 'SUN')
    light_data.energy = 4
    light = bpy.data.objects.new('Key', light_data)
    light.rotation_euler = (math.radians(40), math.radians(20), math.radians(-30))
    scene.collection.objects.link(light)
    world = bpy.data.worlds.new('World')
    world.use_nodes = True
    world.node_tree.nodes['Background'].inputs['Color'].default_value = (.08, .09, .1, 1)
    scene.world = world
    scene.render.filepath = out
    bpy.ops.render.render(write_still=True)


def main():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument('--header')
    parser.add_argument('--preview')
    parser.add_argument('--atlas')
    args = parser.parse_args(argv)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    obj = build()
    corners = triangles(obj)
    print('steering wheel triangles', len(corners) // 3)
    if args.header:
        write_header(args.header, corners)
    if args.preview and args.atlas:
        preview(obj, args.preview, args.atlas)


main()
