# This library provides WebAssembly bindings for the FreeCAD's geometric solver library planegcs.
# Copyright (C) 2023  Miroslav Šerý, Salusoft89 <miroslav.sery@salusoft89.cz>  

# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.

# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.

# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

cmake_minimum_required(VERSION 3.3)
project(PlaneGCS)

set(EXECUTABLE_OUTPUT_PATH "bin")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(LibsModule
    GCS.cpp
    Geo.cpp
    Constraints.cpp
    SubSystem.cpp
    qp_eq.cpp
)

# Kapy: the same sources with KAPY_SEQ_ORDER, for the C API only (see
# Constraints.h). The definition is PUBLIC so every translation unit of
# planegcs-capi sees the same `Constraint` layout as the library.
add_library(LibsModuleSeq
    GCS.cpp
    Geo.cpp
    Constraints.cpp
    SubSystem.cpp
    qp_eq.cpp
)
target_compile_definitions(LibsModuleSeq PUBLIC KAPY_SEQ_ORDER EIGEN_INITIALIZE_MATRICES_BY_ZERO)
target_compile_options(LibsModuleSeq PUBLIC -ftrivial-auto-var-init=zero)

# Both default to where the Docker image puts them; `build-native.sh` points
# them at pinned header trees instead (see services/planegcs/README.md).
set(Boost_INCLUDE_DIR "/inc" CACHE PATH "Boost headers")
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIR})

set(EIGEN3_INCLUDE_DIR "/usr/include/eigen3" CACHE PATH "Eigen headers")
include_directories(${EIGEN3_INCLUDE_DIR})

add_executable(planegcs bindings.cpp)

include_directories(planegcs headers)
target_link_libraries(planegcs LibsModule)

# Kapy: the C API (services/planegcs/build/capi), the second artifact the
# embind build above sits beside until the app moves off embind. Same LibsModule,
# no embind, no closure, no JS-side wrapper: the exports are scalars only.
add_executable(planegcs-capi
    ../capi/gcs_capi.cpp
    ../capi/gcs_capi_constraints.cpp
    ../capi/gcs_capi_alloc.cpp
)
target_include_directories(planegcs-capi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../capi)
target_link_libraries(planegcs-capi LibsModuleSeq)
set_target_properties(planegcs-capi PROPERTIES OUTPUT_NAME planegcs_capi)

# note: exceptions are by default enabled in -O0, but disabled in -O1 and above
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(BUILD_TYPE_FLAGS "-O3 -s WASM_BIGINT=0 -s ASSERTIONS=0 -s ERROR_ON_UNDEFINED_SYMBOLS=0 -s ELIMINATE_DUPLICATE_FUNCTIONS=1")
else()
    set(BUILD_TYPE_FLAGS "-g -O0 -fwasm-exceptions -s ASSERTIONS=1")
endif()

# note: STACK_SIZE fixes memory issue with sketches of > 100 items and (interestingly) temporary constraints
# maybe there could be some optimization in planegcs?
set_target_properties(planegcs PROPERTIES LINK_FLAGS "${BUILD_TYPE_FLAGS} -lembind --closure 1 -s EXPORT_ES6=1 -s MODULARIZE=1 -s MALLOC=emmalloc -s NO_FILESYSTEM=1 -s STACK_SIZE=5MB")

# The C API's link line. Beside what the embind build has (same optimizer,
# allocator and 5 MB stack):
#   - ALLOW_MEMORY_GROWTH: the embind build has a fixed 16 MB heap and a chain
#     of ~400 points aborts it with «OOM» (D169).
#   - EXPORTED_FUNCTIONS from gcs_capi.exports, and -lexports.js: at -O3 emcc
#     3.1.45 renames every export to a letter, and only the JS glue knows the
#     map. MINIFY_WASM_EXPORT_NAMES is an internal setting emcc refuses on the
#     command line, but linking library_exports.js (the runtime lookup of
#     exports by name) is one of the two documented things that turn the
#     minification off. With the names kept in the .wasm itself, a second wasm
#     (Rust) or a test can read `gcs_*` out of the binary directly.
#   - --no-entry: it is a library, there is no main().
# No -lembind and no --closure: there is no embind and nothing to minify.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # The last -s wins: unlike the embind build, an undefined symbol is an error here.
    set(CAPI_LINK_FLAGS "${BUILD_TYPE_FLAGS} -s ERROR_ON_UNDEFINED_SYMBOLS=1")
else()
    set(CAPI_LINK_FLAGS "${BUILD_TYPE_FLAGS}")
endif()
set_target_properties(planegcs-capi PROPERTIES
    LINK_FLAGS "${CAPI_LINK_FLAGS} --no-entry -s EXPORT_ES6=1 -s MODULARIZE=1 -s MALLOC=emmalloc -s NO_FILESYSTEM=1 -s STACK_SIZE=5MB -s ALLOW_MEMORY_GROWTH=1 -lexports.js -s EXPORTED_FUNCTIONS=@${CMAKE_CURRENT_SOURCE_DIR}/../capi/gcs_capi.exports"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../capi/gcs_capi.exports")

# turn off deprecated warnings coming from Boost
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated")

# Kapy: the clang of emsdk 3.1.45 does not compile GCS.cpp deterministically.
# Dead-store elimination's partial-overwrite tracking shortens the zeroing
# memset of an Eigen::SparseMatrix differently from one run to the next, so
# the same sources gave a different planegcs.wasm on every other build.
# Turning that one tracking off makes the artifact reproducible.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mllvm -enable-dse-partial-overwrite-tracking=false")

# todo: possible further flags?
# -s ENVIRONMENT=web
# -s ALLOW_MEMORY_GROWTH=1
# -s DISABLE_EXCEPTION_CATCHING=0
# -Wno-switch