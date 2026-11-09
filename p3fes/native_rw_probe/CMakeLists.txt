include(FetchContent)

set(LIBRW_PLATFORM "NULL" CACHE STRING "librw parser-only platform" FORCE)
set(LIBRW_TOOLS OFF CACHE BOOL "" FORCE)
set(LIBRW_INSTALL OFF CACHE BOOL "" FORCE)
set(LIBRW_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    p3fes_librw
    GIT_REPOSITORY https://github.com/aap/librw.git
    GIT_TAG 18532c2e13efbc1aa43f0b00b4459831f9414de0
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(p3fes_librw)

add_executable(p3fes_rw_probe main.cpp)
target_link_libraries(p3fes_rw_probe PRIVATE librw::librw)
target_compile_features(p3fes_rw_probe PRIVATE cxx_std_20)
