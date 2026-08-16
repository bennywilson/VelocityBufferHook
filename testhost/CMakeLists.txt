add_executable(mv_testhost src/main.cpp src/wpo_encode_test.cpp)

target_link_libraries(mv_testhost PRIVATE d3d12 dxgi d3dcompiler)

target_compile_definitions(mv_testhost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mv_testhost PRIVATE /W4)
