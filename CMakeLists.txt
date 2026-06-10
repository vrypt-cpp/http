cmake_minimum_required(VERSION 3.16)
project(ultrahttp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# ── Backend selection ──────────────────────────────────────────────────────────
option(USE_URING "Build with io_uring backend" OFF)

if(USE_URING)
    set(BACKEND_DEF BACKEND_URING=1 BACKEND_EPOLL=0)
else()
    set(BACKEND_DEF BACKEND_EPOLL=1 BACKEND_URING=0)
endif()

# ── Sources ────────────────────────────────────────────────────────────────────
set(COMMON_SOURCES
    src/mem.c
    src/conn.c
    src/http.c
    src/worker.c
    src/server.c
    src/main.c
)

if(USE_URING)
    list(APPEND COMMON_SOURCES src/uring_backend.c)
else()
    list(APPEND COMMON_SOURCES src/epoll_backend.c)
endif()

add_executable(ultrahttp ${COMMON_SOURCES})

target_include_directories(ultrahttp PRIVATE include)
target_compile_definitions(ultrahttp PRIVATE ${BACKEND_DEF} _GNU_SOURCE)

# ── Warning flags ──────────────────────────────────────────────────────────────
target_compile_options(ultrahttp PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wstrict-aliasing=2
    -Wcast-align
    -Wundef
    -Wmissing-prototypes
    -Wno-unused-parameter
    -Werror
)

# ── Release optimisations ──────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
    target_compile_options(ultrahttp PRIVATE
        -O3
        -march=native
        -mtune=native
        -fno-plt
        -fomit-frame-pointer
        -funroll-loops
        -fstrict-aliasing
        -fvisibility=hidden
    )
    target_link_options(ultrahttp PRIVATE
        -O3
        -flto
        -fno-plt
        -Wl,--strip-all
        -Wl,-z,now
        -Wl,-z,relro
    )
    # LTO
    set_property(TARGET ultrahttp PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ── Debug ──────────────────────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(ultrahttp PRIVATE
        -O0 -g3 -fsanitize=address,undefined
    )
    target_link_options(ultrahttp PRIVATE
        -fsanitize=address,undefined
    )
endif()

# ── PGO: instrument phase ──────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "PGOInstrument")
    target_compile_options(ultrahttp PRIVATE
        -O2 -fprofile-generate=${CMAKE_BINARY_DIR}/pgo-data
    )
    target_link_options(ultrahttp PRIVATE
        -fprofile-generate=${CMAKE_BINARY_DIR}/pgo-data
    )
endif()

# ── PGO: use phase ─────────────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "PGOUse")
    target_compile_options(ultrahttp PRIVATE
        -O3 -march=native -fno-plt
        -fprofile-use=${CMAKE_BINARY_DIR}/pgo-data
        -fprofile-correction
    )
    target_link_options(ultrahttp PRIVATE
        -flto
        -fprofile-use=${CMAKE_BINARY_DIR}/pgo-data
    )
    set_property(TARGET ultrahttp PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ── Link libraries ─────────────────────────────────────────────────────────────
target_link_libraries(ultrahttp PRIVATE pthread)

if(USE_URING)
    find_library(URING_LIB uring REQUIRED)
    target_link_libraries(ultrahttp PRIVATE ${URING_LIB})
endif()

# ── Install ────────────────────────────────────────────────────────────────────
install(TARGETS ultrahttp RUNTIME DESTINATION bin)
