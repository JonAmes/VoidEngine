# Parse the GLB the same way the future cooklet would: header + JSON chunk,
# enumerate nodes/meshes/materials/accessors to verify what survived export.
import json, struct, sys, pathlib

p = pathlib.Path(__file__).parent / "test_ship.glb"
data = p.read_bytes()
magic, ver, length = struct.unpack_from("<III", data, 0)
assert magic == 0x46546C67, "not a GLB"
print(f"GLB v{ver}, {length} bytes")
off = 12
json_chunk = None
bin_len = None
i = 0
while off < length:
    clen, ctype = struct.unpack_from("<II", data, off)
    payload = data[off+8: off+8+clen]
    if ctype == 0x4E4F534A:
        json_chunk = json.loads(payload)
    elif ctype == 0x004E4942:
        bin_len = clen
    off += 8 + clen
    i += 1
print(f"chunks: {i}, BIN chunk {bin_len} bytes")

j = json_chunk
print(f"asset: {j['asset']}")
print(f"\n--- materials ({len(j['materials'])}) ---")
for i, m in enumerate(j["materials"]):
    pbr = m.get("pbrMetallicRoughness", {})
    info = {
        "name": m["name"],
        "baseColor": pbr.get("baseColorFactor"),
        "metallic": pbr.get("metallicFactor"),
        "roughness": pbr.get("roughnessFactor"),
    }
    if "emissiveFactor" in m: info["emissive"] = m["emissiveFactor"]
    if "alphaMode" in m: info["alphaMode"] = m["alphaMode"]
    if "doubleSided" in m: info["doubleSided"] = True
    print(f"  [{i}] {info}")

print(f"\n--- meshes ({len(j['meshes'])}) ---")
for m in j["meshes"]:
    for pr in m["primitives"]:
        attrs = list(pr["attributes"].keys())
        acc = j["accessors"][pr["indices"]]
        mode = pr.get("mode", 4)  # glTF spec: mode is optional, default 4 = TRIANGLES
        print(f"  {m['name']}: mode={mode} tris={acc['count']//3} attrs={attrs}")

print(f"\n--- nodes ({len(j['nodes'])}) ---")
mesh_nodes, empty_nodes = [], []
for i, n in enumerate(j["nodes"]):
    kind = "MESH" if "mesh" in n else "EMPTY"
    extras = n.get("extras", {})
    line = f"  [{i}] {kind} name={n.get('name')}"
    if "translation" in n: line += f" t={['%.2f'%v for v in n['translation'][:3]] if isinstance(n['translation'],list) else n['translation']}"
    if extras: line += f" extras={extras}"
    print(line)
    (mesh_nodes if "mesh" in n else empty_nodes).append(n.get("name"))

print(f"\nmesh nodes: {mesh_nodes}")
print(f"empty nodes (hardpoint sockets + root): {empty_nodes}")

root = j["nodes"][j["scenes"][0]["nodes"][0]]
print(f"\nroot node extras (from HullRoot custom props): {root.get('extras')}")
print(f"root children: {len(root.get('children', []))}")

# check Y-up: fuselage long axis was Blender +Z; Y-up means it should be +Y
fuse_node = next(n for n in j["nodes"] if n.get("name") == "Fuselage")
acc_idx = j["meshes"][fuse_node["mesh"]]["primitives"][0]["attributes"]["POSITION"]
acc = j["accessors"][acc_idx]
print(f"\nFuselage POSITION bounds min={['%.2f'%v for v in acc['min']]} max={['%.2f'%v for v in acc['max']]}")
print("(Blender authored long axis = Z 5.0; expect Y range ~5.0 here if Y-up applied)")

print(f"\nextensionsUsed: {j.get('extensionsUsed')}  extensionsRequired: {j.get('extensionsRequired')}")
