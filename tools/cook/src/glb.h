#pragma once
#include <string>
#include <vector>

#include <ve/cook/cook.h>

// Hand-rolled minimal GLB reader (doctrine: no new deps; glTF JSON parsed
// at COOK time only). Cook-side: turns a hull .glb into one MESH unit —
// flattened meshes (node TRS baked), materials (emissive strength folded,
// doubleSided dropped, texture refs -> TEX0 content IDs (3.8)), and named
// socket records (mesh-less hp_* nodes with extras).
// See cooked.h kUnitMesh payload layout.
namespace ve::cook {

// tex_deps receives the material texture content IDs (3.8); cook.cpp
// validates them against the scanned source set and records manifest
// dependency edges (mesh -> texture, decision #13).
bool cook_glb_unit(const Bytes& glb, const std::string& id, Unit& out,
                   std::vector<std::string>& tex_deps, std::string* err);

} // namespace ve::cook
