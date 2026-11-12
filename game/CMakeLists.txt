# game/ — THIS space sim on top of the engine: stat/weapon defs, combat FSMs,
# cockpit/hangar UI, content glue. All game concept names live here (AGENTS.md).
add_library(ve_game STATIC src/version.cpp src/scrubber_panel.cpp src/units.cpp src/def_fields.cpp
                  src/def_forms_doc.cpp src/def_forms_panel.cpp
                  src/inspector_panel.cpp)
target_include_directories(ve_game PUBLIC include)
target_link_libraries(ve_game PUBLIC ve_core ve_gfx ve_platform)
