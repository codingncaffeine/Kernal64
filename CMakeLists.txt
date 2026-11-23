cmake_minimum_required(VERSION 3.20)
project(Kernal64 VERSION 0.0.0 LANGUAGES CXX)

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
