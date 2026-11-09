# Blender PoC: build a small test spaceship from primitives, tag it with
# game-relevant metadata, save .blend, export .glb, render a preview.
# Run: blender --background --python make_test_ship.py
import bpy, math, os, sys

OUT = os.path.dirname(os.path.abspath(__file__))

# ---- clean scene ----
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()
for block_list in (bpy.data.meshes, bpy.data.materials, bpy.data.curves):
    for b in list(block_list):
        if b.users == 0:
            block_list.remove(b)

# ---- materials (PBR: what glTF metallicRoughness encodes) ----
def mat(name, color, metallic, rough, alpha=1.0, emit=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = rough
    if alpha < 1.0:
        bsdf.inputs["Alpha"].default_value = alpha
        m.blend_method = "BLEND"
    if emit:
        bsdf.inputs["Emission Color"].default_value = (*emit, 1.0)
        bsdf.inputs["Emission Strength"].default_value = 3.0
    return m

m_hull   = mat("hull_metal",  (0.55, 0.56, 0.58), 0.85, 0.40)
m_wing   = mat("wing_paint",  (0.10, 0.30, 0.65), 0.10, 0.55)
m_engine = mat("engine_glow", (0.30, 0.30, 0.32), 0.90, 0.30, emit=(1.0, 0.15, 0.05))
m_glass  = mat("cockpit_glass", (0.6, 0.8, 0.9), 0.0, 0.1, alpha=0.45)

parts = []

def add_mesh(name, obj, material, parent):
    # keep the location the primitive operator already set; parenting to an
    # identity-transform root keeps world positions intact
    obj.data.materials.append(material)
    obj.parent = parent
    parts.append(obj)
    return obj

# ---- root node: what the cooklet treats as the hull entity ----
root = bpy.data.objects.new("HullRoot", None)
bpy.context.collection.objects.link(root)
root["hull_class"] = "light_freighter"   # -> glTF extras
root["mass_t"] = 84.0

# fuselage: elongated hexagonal cylinder along +Z (Blender up)
bpy.ops.mesh.primitive_cylinder_add(vertices=6, radius=0.9, depth=5.0, location=(0, 0, 0))
fuse = bpy.context.object; fuse.name = "Fuselage"
add_mesh("Fuselage", fuse, m_hull, root)

# nose cone
bpy.ops.mesh.primitive_cone_add(vertices=6, radius1=0.9, radius2=0.05, depth=1.6, location=(0, 0, 3.3))
nose = bpy.context.object; nose.name = "Nose"
add_mesh("Nose", nose, m_hull, fuse)

# cockpit bubble
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.6, segments=16, ring_count=8, location=(0, 0.55, 1.6))
cockpit = bpy.context.object; cockpit.name = "Cockpit"
cockpit.scale = (1.0, 0.9, 0.75)
cockpit.location = (0, 0.72, 1.6)  # bust out of the hull so it reads
add_mesh("Cockpit", cockpit, m_glass, fuse)

# wings: thin boxes
for side in (-1, 1):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(side * 2.4, 0, -0.3))
    w = bpy.context.object; w.name = f"Wing_{'L' if side > 0 else 'R'}"
    w.scale = (2.2, 0.15, 1.4)
    w.rotation_euler = (0, math.radians(-4 * side), 0)
    add_mesh(w.name, w, m_wing, fuse)

# engine nacelles
for side in (-1, 1):
    bpy.ops.mesh.primitive_cylinder_add(vertices=12, radius=0.45, depth=2.2, location=(side * 1.4, 0, -3.4))
    e = bpy.context.object; e.name = f"Engine_{'L' if side > 0 else 'R'}"
    add_mesh(e.name, e, m_engine, fuse)

# ---- named anchors: empties (nodes with no mesh in glTF) ----
# 5.9a (#44/#46a): hardpoint sockets AND #30a interior empties, all authored
# on the 2 m x 2.2 m lattice (subcell 4: 0.5 m x/z, 0.55 m y) so 5.9b's snap
# rewrite is an exact no-op on the fixture. Frame matches the glTF directly
# (the checked-in .glb is re-authored by JSON-chunk edit; see note below).
socket_defs = {
    "hp_engine_L": (2.0, 0.0, -6.0),
    "hp_engine_R": (-2.0, 0.0, -6.0),
    "hp_wingtip_L": (6.0, 0.0, 0.0),
    "hp_wingtip_R": (-6.0, 0.0, 0.0),
    "hp_dorsal_turret": (0.0, 2.2, 0.0),
    # 5.8: utility hardpoints for the sample hull def (seat/reactor/battery).
    "hp_seat_pilot": (0.0, 0.0, -4.0),
    "hp_reactor": (-2.0, 0.0, 2.0),
    "hp_battery": (2.0, 0.0, 2.0),
}
# #30a interior empties: name == def component id (pairing key, #46a); NO
# socket_type extras (interior anchors are always utility, #46a). Primary
# anchors (door_/berth_) sit on the cell lattice; furnishings (seat/panel)
# may sit on the subcell lattice (#46b).
empty_defs = {
    "door_1": (0.0, 0.0, 0.0),
    "seat_pilot": (0.5, 0.55, -4.0),
    "panel_main": (0.5, 0.0, -3.5),
    "berth_1": (2.0, 0.0, 4.0),
}
for name, loc in {**socket_defs, **empty_defs}.items():
    emp = bpy.data.objects.new(name, None)
    bpy.context.collection.objects.link(emp)
    emp.empty_display_type = "SINGLE_ARROW"
    emp.location = loc
    emp.parent = fuse if name != "hp_dorsal_turret" else root
    if name in empty_defs:
        continue  # interior anchors carry no socket_type (cooklet gate, #46a)
    # RATIFIED categories (decision #21): main/rcs/gun/utility. The cooklet
    # FAILS on any other socket_type, so this map is authoritative.
    # NOTE: the checked-in assets/samples/test_ship.glb was re-authored by
    # direct JSON-chunk edit (no Blender on the CI box); keep in sync.
    SOCKET_TYPES = {"hp_engine_L": "main", "hp_engine_R": "main",
                    "hp_wingtip_L": "utility", "hp_wingtip_R": "rcs",
                    "hp_dorsal_turret": "gun",
                    "hp_seat_pilot": "utility", "hp_reactor": "utility",
                    "hp_battery": "utility"}
    emp["socket_type"] = SOCKET_TYPES[name]   # -> glTF extras per socket

# ---- simple UVs on fuselage so TEX_COORD_0 exists in the export ----
bpy.context.view_layer.objects.active = fuse
fuse.select_set(True)
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.uv.cube_project(cube_size=2.0)
bpy.ops.object.mode_set(mode="OBJECT")

# shade: autosmooth-ish on curved parts
for ob in parts:
    if ob.type == "MESH":
        for p in ob.data.polygons:
            p.use_smooth = ob.name in ("Cockpit", "Engine_L", "Engine_R")

# ---- save .blend ----
blend_path = os.path.join(OUT, "test_ship.blend")
bpy.ops.wm.save_as_mainfile(filepath=blend_path)

# ---- export .glb (defaults: Y-up, triangulated, +extras) ----
glb_path = os.path.join(OUT, "test_ship.glb")
bpy.ops.export_scene.gltf(
    filepath=glb_path,
    export_format="GLB",
    export_extras=True,          # custom properties -> extras
    export_yup=True,             # spec-conformant Y-up
    export_apply=True,           # apply modifiers
    export_image_format="AUTO",
    export_skins=False,
    export_animations=False,
)

# ---- preview render (workbench = headless-safe) ----
scene = bpy.context.scene
try:
    scene.render.engine = "BLENDER_EEVEE_NEXT"   # real PBR materials, headless on GPU
except TypeError:
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "MATERIAL"
scene.render.resolution_x = 1024
scene.render.resolution_y = 1024
cam_data = bpy.data.cameras.new("Cam")
cam = bpy.data.objects.new("Cam", cam_data)
bpy.context.collection.objects.link(cam)
cam.location = (9.5, 8.0, 5.5)  # +Y side: cockpit bubble faces camera
# point cam at origin
d = (cam.location.to_tuple()[0] - 0, cam.location.to_tuple()[1] - 0, cam.location.to_tuple()[2] - 0)
import mathutils
direction = mathutils.Vector((0, 0, 0.5)) - cam.location
cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
scene.camera = cam
light_data = bpy.data.lights.new("Sun", "SUN")
light_data.energy = 4.0
light = bpy.data.objects.new("Sun", light_data)
bpy.context.collection.objects.link(light)
light.rotation_euler = (math.radians(55), 0, math.radians(35))
scene.render.filepath = os.path.join(OUT, "preview.png")
bpy.ops.render.render(write_still=True)

# ---- stats to stdout ----
n_mesh = sum(1 for o in bpy.data.objects if o.type == "MESH")
n_empty = sum(1 for o in bpy.data.objects if o.type == "EMPTY")
n_tris = sum(sum(len(p.vertices) - 2 for p in o.data.polygons) for o in bpy.data.objects if o.type == "MESH")
print(f"STATS meshes={n_mesh} empties={n_empty} tris={n_tris}")
print(f"SAVED {blend_path}")
print(f"SAVED {glb_path}")
