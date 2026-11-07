#pragma once
#include <ve/core/cooked.h>

#include <string>

// Compile one GLSL source file to SPIR-V via the glslangValidator CLI
// (installed through the Phase 2 prereq gate, decision #35 — it is a dev
// tool, never a linked library; the engine itself carries no GLSL front end,
// decision #13). Output is deterministic for a fixed tool version, matching
// the byte-identical cook contract (0.9).
// VE_GLSLANG overrides the tool name/path (Windows users point it at the
// Vulkan SDK's glslangValidator.exe).
bool compile_glsl(const std::string& src_path, ve::core::cooked::Bytes& spirv_out,
                  std::string* err);
