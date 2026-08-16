add_library(mv_hook SHARED
  src/dllmain.cpp
  src/logging.cpp
  src/d3d12_hook.cpp
  src/resource_tracking.cpp
  src/velocity_identify.cpp
  src/depth_identify.cpp
  src/view_cb.cpp
  src/capture.cpp
  src/overlay.cpp
)

target_include_directories(mv_hook PRIVATE src)
target_link_libraries(mv_hook PRIVATE minhook d3d12 dxgi d3dcompiler)

target_compile_definitions(mv_hook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mv_hook PRIVATE /W4)

# Static CRT (this DLL gets loaded into the game's process and must not
# depend on a specific VC++ redistributable being present there) is set
# globally in the top-level CMakeLists.txt, before MinHook is fetched, so
# every target - including MinHook's - uses the same runtime library.
