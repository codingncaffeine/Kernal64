cmake_minimum_required(VERSION 3.20)
project(Kernal64 VERSION 0.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(K64_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  option(K64_WERROR "Treat warnings as errors" OFF)
  if(K64_WERROR)
    list(APPEND K64_WARNINGS -Werror)
  endif()
endif()

# The emulator itself, shared by the core and the test tools.
add_library(k64core STATIC
  src/core/c64.cpp
  src/core/vic2.cpp
  src/core/cia6526.cpp
  src/core/sid.cpp
  src/core/roms.cpp
)
target_include_directories(k64core PUBLIC src/core)
target_compile_options(k64core PRIVATE ${K64_WARNINGS})

# The libretro core.
add_library(kernal64_libretro SHARED src/libretro/libretro.cpp)
target_include_directories(kernal64_libretro PRIVATE include)
target_link_libraries(kernal64_libretro PRIVATE k64core)
target_compile_options(kernal64_libretro PRIVATE ${K64_WARNINGS})
target_compile_definitions(kernal64_libretro PRIVATE KERNAL64_VERSION="${PROJECT_VERSION}")
set_target_properties(kernal64_libretro PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(kernal64_libretro PRIVATE -Wl,--no-undefined -Wl,--version-script=${CMAKE_SOURCE_DIR}/src/libretro/link.T)
endif()

# A minimal libretro host that loads the built core and checks it through the API.
add_executable(k64host tools/k64host/main.cpp)
target_include_directories(k64host PRIVATE include)
target_compile_options(k64host PRIVATE ${K64_WARNINGS})
target_link_libraries(k64host PRIVATE ${CMAKE_DL_LIBS})

# The per-opcode CPU suite runner.
add_executable(cputest tools/cputest/main.cpp)
target_include_directories(cputest PRIVATE src/core)
target_compile_options(cputest PRIVATE ${K64_WARNINGS})

# Differential test against the transistor-level 6502, built only when the
# fetched simulation is present (tools/fetch.sh perfect6502). Never shipped.
set(K64_P6502 ${CMAKE_SOURCE_DIR}/gitignore/external/perfect6502)
if(EXISTS ${K64_P6502}/perfect6502.c)
  enable_language(C)
  add_library(p6502 STATIC ${K64_P6502}/netlist_sim.c ${K64_P6502}/perfect6502.c tools/cpudiff/lines.c)
  target_include_directories(p6502 PUBLIC ${K64_P6502})
  target_compile_options(p6502 PRIVATE -w)
  add_executable(cpudiff tools/cpudiff/main.cpp)
  target_include_directories(cpudiff PRIVATE src/core)
  target_compile_options(cpudiff PRIVATE ${K64_WARNINGS})
  target_link_libraries(cpudiff PRIVATE p6502)
endif()

# Differential test of the SID against the reference engine, built only when its fetched
# source is present (tools/fetch.sh resid). Never shipped.
set(K64_RESID ${CMAKE_SOURCE_DIR}/gitignore/external/resid/src)
if(EXISTS ${K64_RESID}/sid.cc)
  set(RESID_INLINING 1)
  set(RESID_INLINE inline)
  set(RESID_BRANCH_HINTS 1)
  set(RESID_FPGA_CODE 0)
  set(RESID_CONSTEVAL consteval)
  set(RESID_CONSTEXPR "static constexpr")
  set(RESID_CONSTINIT constinit)
  set(HAVE_BUILTIN_EXPECT 1)
  configure_file(${K64_RESID}/siddefs.h.in ${CMAKE_BINARY_DIR}/resid/siddefs.h @ONLY)
  add_library(resid STATIC ${K64_RESID}/sid.cc ${K64_RESID}/voice.cc ${K64_RESID}/wave.cc ${K64_RESID}/envelope.cc
              ${K64_RESID}/filter.cc ${K64_RESID}/extfilt.cc ${K64_RESID}/pot.cc ${K64_RESID}/version.cc)
  target_include_directories(resid SYSTEM PUBLIC ${CMAKE_BINARY_DIR}/resid ${K64_RESID})
  target_compile_definitions(resid PRIVATE VERSION="1.0")
  target_compile_options(resid PRIVATE -w)
  add_executable(siddiff tools/siddiff/main.cpp)
  target_include_directories(siddiff SYSTEM PRIVATE ${K64_RESID}/..)
  target_link_libraries(siddiff PRIVATE k64core resid)
  target_compile_options(siddiff PRIVATE ${K64_WARNINGS})
endif()

# Runs 6502 test program images (functional test, illegal-opcode tests).
add_executable(imgtest tools/imgtest/main.cpp)
target_include_directories(imgtest PRIVATE src/core)
target_compile_options(imgtest PRIVATE ${K64_WARNINGS})

# Compares the PLA logic with a read-out of a real PLA.
add_executable(placheck tools/placheck/main.cpp)
target_include_directories(placheck PRIVATE src/core)
target_compile_options(placheck PRIVATE ${K64_WARNINGS})

# Boots the machine headless and prints the text screen.
add_executable(k64run tools/k64run/main.cpp)
target_link_libraries(k64run PRIVATE k64core)
target_compile_options(k64run PRIVATE ${K64_WARNINGS})

# The VIC-II checked rule by rule against the article.
add_executable(victest tools/victest/main.cpp)
target_link_libraries(victest PRIVATE k64core)
target_compile_options(victest PRIVATE ${K64_WARNINGS})
