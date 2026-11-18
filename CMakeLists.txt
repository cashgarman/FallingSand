cmake_minimum_required(VERSION 3.28)
project(FallingSand VERSION 1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Falling Sand uses a native Win32 window.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)

add_executable(falling_sand
    src/main.cpp src/window.cpp src/game.cpp src/ui.cpp src/vulkan.cpp)
set_target_properties(falling_sand PROPERTIES OUTPUT_NAME FallingSand)
target_compile_definitions(falling_sand PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE VK_USE_PLATFORM_WIN32_KHR)
if(MSVC)
    target_compile_options(falling_sand PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    set_property(TARGET falling_sand PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
target_link_libraries(falling_sand PRIVATE Vulkan::Vulkan user32 gdi32)

set(shader_names brush.comp reaction.comp grains.comp predict.comp
    equation.comp pressure.comp project.comp flux.comp limit.comp advect.comp separate.comp
    fullscreen.vert world.frag ui.vert ui.frag)
set(shader_outputs)
set(shader_directory "${CMAKE_CURRENT_BINARY_DIR}/compiled-shaders/$<CONFIG>")
foreach(shader IN LISTS shader_names)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
    set(output "${shader_directory}/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_directory}"
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O "${source}" -o "${output}"
        DEPENDS "${source}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/world.glsl"
        VERBATIM)
    list(APPEND shader_outputs "${output}")
endforeach()
add_custom_target(sand_shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:falling_sand>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${shader_outputs} "$<TARGET_FILE_DIR:falling_sand>/shaders"
    DEPENDS ${shader_outputs}
    VERBATIM)
add_dependencies(falling_sand sand_shaders)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME gpu_simulation COMMAND falling_sand --test)
endif()

install(TARGETS falling_sand RUNTIME DESTINATION ".")
install(FILES ${shader_outputs} DESTINATION shaders)
install(FILES README.md DESTINATION ".")

