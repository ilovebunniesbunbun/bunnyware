cmake_minimum_required(VERSION 3.10)
project(bunnyware LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(GLM_ENABLE_EXPERIMENTAL)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Release target CPU. "native" optimises for the build machine; set a baseline
# (e.g. -DBUNNYWARE_MARCH=x86-64-v2) for builds that will be shared.
set(BUNNYWARE_MARCH "native" CACHE STRING "Target CPU for Release builds (-march value)")

# Find OpenGL
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(glm REQUIRED)
find_package(spdlog REQUIRED)

# Find GLFW3
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED glfw3)

# Find X11 and Xext for shape extension click-through
find_package(X11 REQUIRED)
find_library(XEXT_LIB Xext REQUIRED)

# Find Wayland and EGL
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-egl egl)
pkg_check_modules(DBUS REQUIRED dbus-1)

# Find Wayland Protocols and Scanner for dynamic protocol generation
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

# Warning levels and vendor silencing flags
if(MSVC)
    set(PROJECT_WARNING_FLAGS /W4)
    set(VENDOR_WARNING_FLAGS /W0)
else()
    set(PROJECT_WARNING_FLAGS -Wall -Wextra)
    set(VENDOR_WARNING_FLAGS -w)
endif()

# ==========================================
# 3rd Party Target Definitions (Isolated)
# ==========================================

# 1. Meshoptimizer target
add_library(meshopt STATIC
    src/external/meshopt/Allocator.cpp
    src/external/meshopt/IndexCodec.cpp
    src/external/meshopt/IndexGenerator.cpp
    src/external/meshopt/OverdrawOptimizer.cpp
    src/external/meshopt/VcacheOptimizer.cpp
    src/external/meshopt/VertexCodec.cpp
    src/external/meshopt/VfetchOptimizer.cpp
)
target_include_directories(meshopt SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/external
    ${CMAKE_CURRENT_SOURCE_DIR}/src/external/meshopt
)
target_compile_options(meshopt PRIVATE ${VENDOR_WARNING_FLAGS})

# 2. Zstd target
add_library(zstd STATIC
    src/external/zstd/zstdimpl.cpp
)
target_include_directories(zstd SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/external/zstd
)
target_compile_definitions(zstd PUBLIC ZSTD_DISABLE_ASM)
target_compile_options(zstd PRIVATE ${VENDOR_WARNING_FLAGS})

# 3. ImGui target
add_library(imgui STATIC
    src/external/imgui/imgui.cpp
    src/external/imgui/imgui_draw.cpp
    src/external/imgui/imgui_tables.cpp
    src/external/imgui/imgui_widgets.cpp
    src/external/imgui/imgui_impl_glfw.cpp
    src/external/imgui/imgui_impl_opengl3.cpp
)
target_include_directories(imgui SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/external/imgui
)
target_include_directories(imgui PRIVATE
    ${GLFW_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIR}
)
target_link_libraries(imgui PRIVATE
    OpenGL::GL
    ${GLFW_LIBRARIES}
)
target_compile_options(imgui PRIVATE ${VENDOR_WARNING_FLAGS})


# ==========================================
# Project Targets
# ==========================================

# Shared parser sources
file(GLOB_RECURSE PARSER_SOURCES CONFIGURE_DEPENDS
    "src/vpk/*.cpp"
)

# Main Application sources
file(GLOB APP_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/app/*.cpp"
    "src/renderer/*.cpp"
    "src/menu/*.cpp"
    "src/overlay/*.cpp"
)
list(FILTER APP_SOURCES EXCLUDE REGEX "src/scratch_dump.cpp")

# Generate Wayland protocols dynamically
set(WAYLAND_PROTOCOL_XMLS
    "${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml"
    "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
    "${WAYLAND_PROTOCOLS_DIR}/staging/tearing-control/tearing-control-v1.xml"
    "${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml"
    "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml"
)

set(WAYLAND_GENERATED_SOURCES)
foreach(xml_file ${WAYLAND_PROTOCOL_XMLS})
    if(NOT EXISTS "${xml_file}")
        message(FATAL_ERROR "Required Wayland protocol XML missing: ${xml_file}. Install/update wayland-protocols (fractional-scale-v1 and viewporter are required at build time).")
    endif()
    get_filename_component(xml_name ${xml_file} NAME_WE)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${xml_name}-client-protocol.h")
    set(code "${CMAKE_CURRENT_BINARY_DIR}/${xml_name}-protocol.c")
    
    add_custom_command(
        OUTPUT "${header}"
        COMMAND ${WAYLAND_SCANNER} client-header "${xml_file}" "${header}"
        DEPENDS "${xml_file}"
        VERBATIM
    )
    
    add_custom_command(
        OUTPUT "${code}"
        COMMAND ${WAYLAND_SCANNER} private-code "${xml_file}" "${code}"
        DEPENDS "${xml_file}"
        VERBATIM
    )
    
    list(APPEND WAYLAND_GENERATED_SOURCES "${header}" "${code}")
endforeach()

# Main Executable Target
add_executable(bunnyware ${APP_SOURCES} ${PARSER_SOURCES} ${WAYLAND_GENERATED_SOURCES})

target_include_directories(bunnyware PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols
    ${CMAKE_CURRENT_BINARY_DIR}
    ${OPENGL_INCLUDE_DIR}
    ${GLFW_INCLUDE_DIRS}
    ${X11_INCLUDE_DIR}
    ${DBUS_INCLUDE_DIRS}
    ${LIBDECOR_INCLUDE_DIRS}
)

target_link_libraries(bunnyware PRIVATE
    imgui
    meshopt
    zstd
    glm::glm
    spdlog::spdlog
    OpenGL::GL
    GLEW::GLEW
    ${GLFW_LIBRARIES}
    ${X11_LIBRARIES}
    ${XEXT_LIB}
    ${WAYLAND_LIBRARIES}
    ${DBUS_LIBRARIES}
    ${LIBDECOR_LIBRARIES}
    rt
    pthread
    m
)

target_compile_options(bunnyware PRIVATE 
    ${PROJECT_WARNING_FLAGS}
    $<$<CONFIG:Release>:-O3 -march=${BUNNYWARE_MARCH} -ffast-math -flto>
)
target_link_options(bunnyware PRIVATE
    $<$<CONFIG:Release>:-O3 -march=${BUNNYWARE_MARCH} -flto>
)

# Sync offsets.json into the build tree whenever the source file changes.
# A POST_BUILD-only copy is not enough: if only offsets.json is edited, make
# skips rebuilding bunnyware and the stale build/offsets.json is left behind.
set(BUNNYWARE_OFFSETS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/offsets.json")
set(BUNNYWARE_OFFSETS_DST "${CMAKE_CURRENT_BINARY_DIR}/offsets.json")
add_custom_command(
    OUTPUT "${BUNNYWARE_OFFSETS_DST}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${BUNNYWARE_OFFSETS_SRC}"
        "${BUNNYWARE_OFFSETS_DST}"
    DEPENDS "${BUNNYWARE_OFFSETS_SRC}"
    COMMENT "Syncing offsets.json -> build/"
    VERBATIM
)
add_custom_target(bunnyware_sync_offsets ALL DEPENDS "${BUNNYWARE_OFFSETS_DST}")
add_dependencies(bunnyware bunnyware_sync_offsets)

# Copy assets (including the immutable config baseline) and offsets after linking.
add_custom_command(TARGET bunnyware POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:bunnyware>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/offsets.json
        $<TARGET_FILE_DIR:bunnyware>/offsets.json
    COMMENT "Copying assets and offsets.json to build directory"
)

option(BUNNYWARE_BUILD_TESTS "Build C++ unit tests" OFF)
if(BUNNYWARE_BUILD_TESTS)
    enable_testing()

    add_executable(unit_tests tests/cpp/test_main.cpp src/config.cpp src/menu/menu_search.cpp src/overlay/focus_monitor.cpp ${PARSER_SOURCES} ${WAYLAND_GENERATED_SOURCES})
    target_include_directories(unit_tests PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/overlay
        ${CMAKE_CURRENT_SOURCE_DIR}/protocols
        ${CMAKE_CURRENT_BINARY_DIR}
        ${DBUS_INCLUDE_DIRS}
    )
    target_link_libraries(unit_tests PRIVATE
        meshopt
        zstd
        glm::glm
        spdlog::spdlog
        ${WAYLAND_LIBRARIES}
        ${DBUS_LIBRARIES}
        ${X11_LIBRARIES}
        rt
        pthread
        m
    )
    # The tests are assert-based; strip NDEBUG so they actually validate in the
    # default Release configuration instead of passing vacuously.
    target_compile_options(unit_tests PRIVATE -UNDEBUG)
    add_test(NAME test_distance COMMAND unit_tests --distance)
    add_test(NAME test_esp_distance COMMAND unit_tests --esp-distance)
    add_test(NAME test_shm_reader_bounds COMMAND unit_tests --shm-bounds)
    add_test(NAME test_flash_alpha COMMAND unit_tests --flash-alpha)
    add_test(NAME test_hitbox_parse COMMAND unit_tests --hitbox-parse)
    add_test(NAME test_bvh_raycast COMMAND unit_tests --bvh-raycast)
    add_test(NAME test_color_conversions COMMAND unit_tests --color-conv)
    add_test(NAME test_config_serialize COMMAND unit_tests --config-serialize)
    add_test(NAME test_config_profiles COMMAND unit_tests --config-profiles)
    add_test(NAME test_triggerbot_math COMMAND unit_tests --triggerbot-math)
    add_test(NAME test_proximity_beep COMMAND unit_tests --proximity-beep)
    add_test(NAME test_smoke_wireframe COMMAND unit_tests --smoke-wireframe)
    add_test(NAME test_hit_features COMMAND unit_tests --hit-features)
    add_test(NAME test_fuzzy_search COMMAND unit_tests --fuzzy-search)
    add_test(NAME test_focus_monitor COMMAND unit_tests --focus-monitor)
    add_test(NAME test_offset_scanner COMMAND unit_tests --offset-scanner)
endif()
