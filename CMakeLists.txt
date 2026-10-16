cmake_minimum_required(VERSION 3.10)
project(lunet C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(include)

# === LuaJIT ===
find_path(LUAJIT_INCLUDE_DIR luajit.h
  HINTS ${LUAJIT_INCLUDE_DIR} ENV LUAJIT_INCLUDE_DIR /opt/homebrew/include /usr/local/include
  PATH_SUFFIXES luajit-2.1
)
find_library(LUAJIT_LIBRARY NAMES luajit-5.1
  HINTS ${LUAJIT_LIBRARY} ENV LUAJIT_LIBRARY /opt/homebrew/lib /usr/local/lib
)

if (NOT LUAJIT_INCLUDE_DIR OR NOT LUAJIT_LIBRARY)
  message(FATAL_ERROR "LuaJIT not found. Set -DLUAJIT_INCLUDE_DIR and -DLUAJIT_LIBRARY manually.")
endif()
include_directories(${LUAJIT_INCLUDE_DIR})
message(STATUS "Found LuaJIT: ${LUAJIT_LIBRARY}")

# === libuv ===
find_path(LIBUV_INCLUDE_DIR uv.h
  HINTS ${LIBUV_INCLUDE_DIR} ENV LIBUV_INCLUDE_DIR /opt/homebrew/include /usr/local/include
)
find_library(LIBUV_LIBRARY NAMES uv
  HINTS ${LIBUV_LIBRARY} ENV LIBUV_LIBRARY /opt/homebrew/lib /usr/local/lib
)

if (NOT LIBUV_INCLUDE_DIR OR NOT LIBUV_LIBRARY)
  message(FATAL_ERROR "libuv not found. Set -DLIBUV_INCLUDE_DIR and -DLIBUV_LIBRARY manually.")
endif()
include_directories(${LIBUV_INCLUDE_DIR})
message(STATUS "Found libuv: ${LIBUV_LIBRARY}")

# === Source files ===
file(GLOB_RECURSE SOURCES src/*.c)
file(GLOB_RECURSE HEADERS include/*.h)

add_executable(lunet ${SOURCES} ${HEADERS})

target_link_libraries(lunet
  ${LUAJIT_LIBRARY}
  ${LIBUV_LIBRARY}
)
