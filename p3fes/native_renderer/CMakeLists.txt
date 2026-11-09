add_library(p3fes_native_model STATIC
    native_model.cpp)
target_include_directories(p3fes_native_model PUBLIC ..)
target_compile_features(p3fes_native_model PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi")
    add_library(p3fes_native_renderer_vita STATIC
        renderer_vitagl.cpp)
    target_include_directories(p3fes_native_renderer_vita PUBLIC ..)
    target_link_libraries(p3fes_native_renderer_vita PUBLIC ps2x_vita_raylib)
    target_compile_definitions(p3fes_native_renderer_vita PUBLIC
        PLATFORM_VITA
        PS2X_VITA_SDL2_SHIM=1)
    target_compile_features(p3fes_native_renderer_vita PUBLIC cxx_std_20)
    add_library(p3fes_native_renderer_platform ALIAS p3fes_native_renderer_vita)
elseif(NOT ANDROID)
    add_library(p3fes_native_renderer_desktop STATIC
        renderer_raylib.cpp)
    target_include_directories(p3fes_native_renderer_desktop PUBLIC ..)
    target_link_libraries(p3fes_native_renderer_desktop PUBLIC raylib)
    target_compile_features(p3fes_native_renderer_desktop PUBLIC cxx_std_20)
    add_library(p3fes_native_renderer_platform ALIAS p3fes_native_renderer_desktop)
endif()
