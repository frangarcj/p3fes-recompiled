cmake_minimum_required(VERSION 3.21)

project(ps2xIOP LANGUAGES CXX)

option(PS2X_IOP_BUILD_TESTS "Build ps2xIOP emulator smoke tests" OFF)
option(PS2X_IOP_WORK_DIAGNOSTICS "Enable opt-in IOP work diagnostic counters" OFF)

add_library(ps2_iop STATIC
    src/ps2_path.cpp
    src/iop_module_manager.cpp
    src/iop_subsystem.cpp
    src/emulator/iop_emulator.cpp
    src/emulator/core/iop_cpu.cpp
    src/emulator/core/iop_kernel.cpp
    src/emulator/core/iop_memory.cpp
    src/emulator/core/spu2.cpp
    src/emulator/core/sio2.cpp
    src/emulator/services/iop_module_loader.cpp
    src/emulator/services/iop_rpc.cpp
    src/emulator/imports/iop_cdvd.cpp
    src/emulator/imports/iop_heaplib.cpp
    src/emulator/imports/iop_imports.cpp
    src/emulator/imports/iop_intrman.cpp
    src/emulator/imports/iop_ioman.cpp
    src/emulator/imports/iop_loadcore.cpp
    src/emulator/imports/iop_stdio.cpp
    src/emulator/imports/iop_sysclib.cpp
    src/emulator/imports/iop_sysmem.cpp
    src/emulator/imports/iop_timrman.cpp
    src/emulator/imports/iop_vblank.cpp
    src/modules/cri_dtx.cpp
    src/modules/dbcman.cpp
    src/modules/libsd.cpp
    src/modules/mcserv.cpp
)

target_compile_features(ps2_iop PUBLIC cxx_std_20)

if(PS2X_IOP_WORK_DIAGNOSTICS)
    target_compile_definitions(ps2_iop PRIVATE PS2X_IOP_WORK_DIAGNOSTICS=1)
endif()

target_include_directories(ps2_iop
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(ps2x::iop ALIAS ps2_iop)

if(PS2X_IOP_BUILD_TESTS)
    enable_testing()
    add_executable(ps2_iop_emulator_tests tests/iop_emulator_tests.cpp)
    target_link_libraries(ps2_iop_emulator_tests PRIVATE ps2_iop)
    add_test(NAME ps2_iop_emulator_tests COMMAND ps2_iop_emulator_tests)

    add_executable(ps2_iop_import_tests tests/iop_import_tests.cpp)
    target_link_libraries(ps2_iop_import_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_import_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_import_tests COMMAND ps2_iop_import_tests)

    add_executable(ps2_iop_compatibility_tests tests/iop_compatibility_tests.cpp)
    target_link_libraries(ps2_iop_compatibility_tests PRIVATE ps2_iop)
    add_test(NAME ps2_iop_compatibility_tests COMMAND ps2_iop_compatibility_tests)

    add_executable(ps2_iop_import_version_tests tests/iop_import_version_tests.cpp)
    target_link_libraries(ps2_iop_import_version_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_import_version_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_import_version_tests COMMAND ps2_iop_import_version_tests)

    add_executable(ps2_iop_spu2_clock_tests tests/spu2_clock_tests.cpp)
    target_link_libraries(ps2_iop_spu2_clock_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_spu2_clock_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_spu2_clock_tests COMMAND ps2_iop_spu2_clock_tests)

    # Equivalence of the optimised Spu2 against the frozen pre-optimisation copy in tests/reference.
    # Run with --bench for a reference-vs-optimised samples/second report.
    add_executable(ps2_iop_spu2_equivalence_tests
        tests/spu2_equivalence_tests.cpp
        tests/reference/spu2_reference.cpp
    )
    target_link_libraries(ps2_iop_spu2_equivalence_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_spu2_equivalence_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference
    )
    add_test(NAME ps2_iop_spu2_equivalence_tests COMMAND ps2_iop_spu2_equivalence_tests)

    add_executable(ps2_iop_idle_fast_tests tests/idle_fast_tests.cpp)
    target_link_libraries(ps2_iop_idle_fast_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_idle_fast_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(PS2X_IOP_WORK_DIAGNOSTICS)
        target_compile_definitions(ps2_iop_idle_fast_tests PRIVATE PS2X_IOP_WORK_DIAGNOSTICS=1)
    endif()
    add_test(NAME ps2_iop_idle_fast_tests COMMAND ps2_iop_idle_fast_tests)

    add_executable(ps2_iop_kernel_tests tests/iop_kernel_tests.cpp)
    target_link_libraries(ps2_iop_kernel_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_kernel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_kernel_tests COMMAND ps2_iop_kernel_tests)

    add_executable(ps2_iop_fast_equivalence_tests tests/iop_fast_equivalence_tests.cpp)
    target_link_libraries(ps2_iop_fast_equivalence_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_fast_equivalence_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_fast_equivalence_tests COMMAND ps2_iop_fast_equivalence_tests)

    add_executable(ps2_iop_fast_integration_tests tests/iop_fast_integration_tests.cpp)
    target_link_libraries(ps2_iop_fast_integration_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_fast_integration_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME ps2_iop_fast_integration_tests COMMAND ps2_iop_fast_integration_tests)

    add_executable(ps2_iop_real_driver_bench tests/iop_real_driver_bench.cpp)
    target_link_libraries(ps2_iop_real_driver_bench PRIVATE ps2_iop)
    # not a ctest: needs the game's LIB31 directory (ps2_iop_real_driver_bench <dir> [seconds] [warm-up] [chunk])

endif()

install(TARGETS ps2_iop
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)
