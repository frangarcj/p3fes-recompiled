add_library(p3fes_native_model STATIC
    native_model.cpp)
target_include_directories(p3fes_native_model PUBLIC ..)
target_compile_features(p3fes_native_model PUBLIC cxx_std_20)

if(NOT ANDROID AND NOT CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi")
    add_library(p3fes_native_renderer_desktop STATIC
        renderer_raylib.cpp)
    target_include_directories(p3fes_native_renderer_desktop PUBLIC ..)
    target_link_libraries(p3fes_native_renderer_desktop PUBLIC raylib)
    target_compile_features(p3fes_native_renderer_desktop PUBLIC cxx_std_20)
endif()
