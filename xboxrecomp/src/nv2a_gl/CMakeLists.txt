cmake_minimum_required(VERSION 3.20)
project(nv2a_gl C)

# OpenGL 3.3 core renderer for the pushbuffer executor. SDL2 provides the
# window, the context and the entry points (gl_api.c resolves them itself),
# so there is no GL loader dependency and the same code builds for the
# Switch's Mesa.
add_library(xbox_nv2a_gl STATIC
    nv2a_gl.c
    gl_api.c
    gl_vsh.c
    gl_psh.c
)
target_include_directories(xbox_nv2a_gl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(VITA)
    # PS Vita: vitaGL (GL ES 2.0 with vitashark's GLSL/Cg compiler) makes the
    # display and the context itself, through the game target's link line
    # (CMakeLists.txt). No SDL here; gl_api.h includes <vitaGL.h>, and
    # __vita__ comes from the toolchain.
    # Link order: xbox_nv2a_gl calls glViewport/glUseProgram/glBindTexture,
    # so vitaGL and vitashark must come after it on the link line. PUBLIC
    # puts them after this archive for every consumer.
    target_link_libraries(xbox_nv2a_gl PUBLIC vitaGL vitashark)
elseif(NINTENDO_SWITCH)
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_nv2a_gl PUBLIC SDL2::SDL2)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    target_link_libraries(xbox_nv2a_gl PUBLIC PkgConfig::SDL2)
endif()
if(NOT MSVC)
    target_compile_options(xbox_nv2a_gl PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
    if(VITA)
        target_compile_options(xbox_nv2a_gl PRIVATE
            -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -ffast-math)
    endif()
endif()
