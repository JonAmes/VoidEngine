#pragma once
#include <string>

#include <ve/cook/cook.h>

// Hand-rolled minimal GLB reader (doctrine: no new deps; glTF JSON parsed
// at COOK time only). Cook-side: turns a hull .glb into one MESH unit —
// flattened meshes (node TRS baked), materials (emissive strength folded,
// doubleSided dropped), and named socket records (mesh-less hp_* nodes with
// extras). See cooked.h kUnitMesh payload layout.
namespace ve::cook {

bool cook_glb_unit(const Bytes& glb, const std::string& id, Unit& out,
                   std::string* err);

} // namespace ve::cook
