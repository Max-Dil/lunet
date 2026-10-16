cmake_minimum_required(VERSION 3.10)
project(lunet C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(include)

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

find_path(LIBPQ_INCLUDE_DIR libpq-fe.h
  HINTS ${LIBPQ_INCLUDE_DIR} ENV LIBPQ_INCLUDE_DIR /opt/homebrew/opt/libpq/include /opt/homebrew/include /usr/local/include /usr/include
  PATH_SUFFIXES postgresql
)
find_library(LIBPQ_LIBRARY NAMES pq
  HINTS ${LIBPQ_LIBRARY} ENV LIBPQ_LIBRARY /opt/homebrew/opt/libpq/lib /opt/homebrew/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
)

if (NOT LIBPQ_INCLUDE_DIR OR NOT LIBPQ_LIBRARY)
  message(FATAL_ERROR "libpq not found. Set -DLIBPQ_INCLUDE_DIR and -DLIBPQ_LIBRARY manually.")
endif()
include_directories(${LIBPQ_INCLUDE_DIR})
message(STATUS "Found libpq: ${LIBPQ_LIBRARY}")

file(GLOB_RECURSE SOURCES src/*.c)
file(GLOB_RECURSE HEADERS include/*.h)

add_executable(lunet ${SOURCES} ${HEADERS})

target_link_libraries(lunet
  ${LUAJIT_LIBRARY}
  ${LIBUV_LIBRARY}
  ${LIBPQ_LIBRARY}
  m
  dl
  pthread
)