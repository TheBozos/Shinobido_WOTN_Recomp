cmake_minimum_required(VERSION 3.21)
project(Shinobido_WOTN_Recomp LANGUAGES CXX)

# Shinobido: Way of the Ninja (PAL, SCES-53931) on BozoEngine: the game's EE code recompiled by bozo-recomp, every
# library and kernel call answered natively by the engine's runtime. No PS2 hardware model.
# The ELF comes from the player's own disc (scripts/extract_iso.sh puts it in game/).

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(SHINO_GAME_DIR "${CMAKE_SOURCE_DIR}/game" CACHE PATH "User-owned extracted disc files")
set(SHINO_ISO "${CMAKE_SOURCE_DIR}/Shinobido - Way of the Ninja (Europe) (En,Fr,De,Es,It).iso" CACHE FILEPATH
    "User-owned PAL disc image")
set(BOZO_ENGINE_DIR "${CMAKE_SOURCE_DIR}/BozoEngine" CACHE PATH "Pinned native engine source")
option(SHINO_WINDOW "Build the SDL3 native game window (shinobido) next to shino-headless" ON)
set(BOZO_APP ${SHINO_WINDOW} CACHE BOOL "" FORCE)
set(BOZO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BOZO_REQUIRE_FFMPEG ON CACHE BOOL "The movies need native decoding" FORCE)
# The movies' decoder: the minimal static FFmpeg of scripts/build_ffmpeg.sh (MPEG-2 video only), handed to BozoEngine as
# BOZO_FFMPEG_ROOT (TLOTR_TTA_Recomp's set-up). Not built: the system's FFmpeg through pkg-config.
set(SHINO_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/build/ffmpeg" CACHE PATH "Minimal static FFmpeg (scripts/build_ffmpeg.sh)")
if(SHINO_FFMPEG_ROOT AND EXISTS "${SHINO_FFMPEG_ROOT}/lib/libavcodec.a")
  set(BOZO_FFMPEG_ROOT "${SHINO_FFMPEG_ROOT}" CACHE PATH "FFmpeg folder (include/, lib/) for libmpeg's decoder" FORCE)
  message(STATUS "Shinobido: the movies' FFmpeg from ${SHINO_FFMPEG_ROOT} (static, MPEG-2 only)")
else()
  set(BOZO_FFMPEG_ROOT "" CACHE PATH "FFmpeg folder (include/, lib/) for libmpeg's decoder" FORCE)
endif()
add_subdirectory("${BOZO_ENGINE_DIR}" BozoEngine)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(shino_elf ${SHINO_GAME_DIR}/SCES_539.31)
set(shino_symbols ${CMAKE_SOURCE_DIR}/recomp/shino_symbols.txt)
# Jump-table targets bozo-recomp cannot find itself (scripts/entries.sh, tools/shino_entries.py).
set(shino_entries ${CMAKE_SOURCE_DIR}/recomp/shino_entries.txt)
# shino_prep: the lists made before translation; shino_gen: bozo-recomp's output only (it removes any other shino_*
# file there).
set(shino_prep ${CMAKE_BINARY_DIR}/shino_prep)
set(shino_gen ${CMAKE_BINARY_DIR}/shino_gen)
file(MAKE_DIRECTORY ${shino_prep} ${shino_gen})
if(NOT EXISTS ${shino_elf})
  message(FATAL_ERROR "${shino_elf} not found: run scripts/extract_iso.sh <iso> first")
endif()
file(GLOB shino_port_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/shino_*.cpp)

# The executable with its one section cut to the EE code (tools/shino_prepare.py): the image bozo-recomp translates and
# the runner loads.
set(shino_image ${shino_prep}/shino.elf)
add_custom_command(OUTPUT ${shino_image}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/shino_prepare.py ${shino_elf} ${shino_image}
  DEPENDS ${shino_elf} ${CMAKE_SOURCE_DIR}/tools/shino_prepare.py
  COMMENT "Preparing SCES_539.31")

# The externals list: library and kernel functions named by the symbol map become stubs the runtime answers; every
# address the port registers (tools/shino_overrides_list.py scans runtime/) becomes an override or a native site.
set(shino_externals ${shino_prep}/shino_externals.txt)
add_custom_command(OUTPUT ${shino_externals}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/shino_overrides_list.py ${CMAKE_SOURCE_DIR}/runtime ${shino_prep}/shino_overrides.txt
  COMMAND bozo-externals ${shino_image} ${shino_externals} --symbols ${shino_symbols} --overrides ${shino_prep}/shino_overrides.txt
  DEPENDS bozo-externals ${shino_image} ${shino_symbols} ${CMAKE_SOURCE_DIR}/tools/shino_overrides_list.py ${shino_port_sources}
  COMMENT "Listing the externals of SCES_539.31")

set(shino_chunks 32)
set(shino_sources ${shino_gen}/shino_functions.h ${shino_gen}/shino_table.cpp)
math(EXPR shino_last "${shino_chunks} - 1")
foreach(i RANGE ${shino_last})
  if(i LESS 10)
    list(APPEND shino_sources ${shino_gen}/shino_00${i}.cpp)
  else()
    list(APPEND shino_sources ${shino_gen}/shino_0${i}.cpp)
  endif()
endforeach()
add_custom_command(OUTPUT ${shino_sources}
  COMMAND bozo-recomp ${shino_image} ${shino_gen} --prefix shino --files ${shino_chunks} --no-comments
          --externals ${shino_externals} --symbols ${shino_symbols} --entries ${shino_entries}
          --report ${shino_prep}/shino_report.txt
  DEPENDS bozo-recomp ${shino_image} ${shino_externals} ${shino_symbols} ${shino_entries}
  COMMENT "Recompiling SCES_539.31")
add_custom_target(shino_generate DEPENDS ${shino_sources})

add_library(shino_game STATIC ${shino_sources})
target_link_libraries(shino_game PUBLIC bozo_runtime_headers)
target_include_directories(shino_game PUBLIC ${shino_gen})
target_compile_options(shino_game PRIVATE -O1 -w -ffp-contract=off)

# The VU1 microprograms, recompiled from the microcode packets of the player's executable (tools/shino_vu1_recomp.py,
# runtime/shino_vu.h): the interpreter in runtime/shino_vu1.cpp runs what they do not cover.
set(shino_vu1_gen ${CMAKE_BINARY_DIR}/shino_vu1_gen)
set(shino_vu1_sources ${shino_vu1_gen}/shino_vu1_table.cpp)
foreach(i RANGE 7)
  list(APPEND shino_vu1_sources ${shino_vu1_gen}/shino_vu1_0${i}.cpp)
endforeach()
add_custom_command(OUTPUT ${shino_vu1_sources}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/shino_vu1_recomp.py ${shino_elf} ${shino_vu1_gen} --files 8
  DEPENDS ${shino_elf} ${CMAKE_SOURCE_DIR}/tools/shino_vu1_recomp.py
  COMMENT "Recompiling the VU1 microprograms")
add_library(shino_vu1_code STATIC ${shino_vu1_sources})
target_include_directories(shino_vu1_code PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_compile_options(shino_vu1_code PRIVATE -O2 -w -ffp-contract=off)

# The IOP sound modules, statically compiled from the player's disc (tools/shino_bozo_audio.py, docs/native-irx.md in
# BozoEngine): Sony's LIBSD and SDRDRV, Acquire's TORAST and TORASE.
set(shino_audio_gen ${CMAKE_BINARY_DIR}/shino_audio_gen)
set(shino_audio_names LIBSD SDRDRV TORAST TORASE)
set(shino_audio_outputs ${shino_audio_gen}/shino_audio_modules.h)
set(shino_audio_inputs)
set(shino_audio_code)
foreach(module ${shino_audio_names})
  string(TOLOWER "${module}" lower)
  list(APPEND shino_audio_outputs ${shino_audio_gen}/${lower}_iop.cpp)
  list(APPEND shino_audio_code ${shino_audio_gen}/${lower}_iop.cpp)
  list(APPEND shino_audio_inputs ${SHINO_GAME_DIR}/IOP/MODULES/${module}.IRX)
endforeach()
add_custom_command(OUTPUT ${shino_audio_outputs}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/shino_bozo_audio.py ${SHINO_GAME_DIR} ${shino_audio_gen} ${BOZO_ENGINE_DIR}
  DEPENDS ${shino_audio_inputs} ${CMAKE_SOURCE_DIR}/tools/shino_bozo_audio.py
          ${BOZO_ENGINE_DIR}/tools/native_irx/relocate.py ${BOZO_ENGINE_DIR}/tools/native_irx/recompile.py
  COMMENT "Statically recompiling the IOP sound modules")
add_custom_target(shino_native_audio DEPENDS ${shino_audio_outputs})
add_library(shino_audio_code OBJECT ${shino_audio_code})
target_link_libraries(shino_audio_code PRIVATE bozo_runtime_headers)
target_compile_options(shino_audio_code PRIVATE -O1 -w)

set(shino_runtime_sources ${shino_port_sources} $<TARGET_OBJECTS:shino_audio_code>)
list(FILTER shino_runtime_sources EXCLUDE REGEX "shino_main\\.cpp$")

add_executable(shino-headless runtime/shino_main.cpp ${shino_runtime_sources})
target_link_libraries(shino-headless PRIVATE shino_game shino_vu1_code bozo_host)
target_include_directories(shino-headless PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${shino_audio_gen})
add_dependencies(shino-headless shino_native_audio)
target_compile_definitions(shino-headless PRIVATE SHINO_DEFAULT_ELF="${shino_image}" SHINO_DEFAULT_ISO="${SHINO_ISO}"
                                                    SHINO_SYMBOL_MAP="${shino_symbols}")
if(MINGW)
  target_link_options(shino-headless PRIVATE -static)
endif()

# The game in a window (SDL3, OpenGL 3.3): the same port, the PC's pads and sound.
if(SHINO_WINDOW)
  add_executable(shinobido runtime/shino_main.cpp runtime/app/shino_cheats_ui.cpp ${shino_runtime_sources})
  target_link_libraries(shinobido PRIVATE shino_game shino_vu1_code bozo_host bozo_app)
  target_include_directories(shinobido PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${shino_audio_gen})
  add_dependencies(shinobido shino_native_audio)
  target_compile_definitions(shinobido PRIVATE SHINO_WITH_WINDOW SHINO_DEFAULT_ELF="${shino_image}"
                                               SHINO_DEFAULT_ISO="${SHINO_ISO}" SHINO_SYMBOL_MAP="${shino_symbols}")
  if(WIN32)
    # The window's icon (docs/branding/make_logo.py: the Bozos clown as a ninja; no game art), the UTF-8 manifest and
    # the version information.
    enable_language(RC)
    set_source_files_properties(runtime/app/shinobido.rc PROPERTIES OBJECT_DEPENDS
      "${CMAKE_SOURCE_DIR}/runtime/app/shinobido.ico;${CMAKE_SOURCE_DIR}/runtime/app/shinobido.manifest")
    target_sources(shinobido PRIVATE runtime/app/shinobido.rc)
  endif()
  if(MINGW)
    target_link_options(shinobido PRIVATE -static)
    get_filename_component(shino_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET shinobido POST_BUILD
      COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:shinobido> -DSEARCH=${shino_toolchain_bin}
              -P ${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake)
  endif()
endif()
