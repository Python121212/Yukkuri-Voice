cmake_minimum_required(VERSION 3.22.1)
project(yukkuri_voice C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS_RELEASE "-O2 -ffast-math -fvisibility=hidden")

# エンジン本体はリポジトリ直下の core/ (Android 非依存の C99)
#   <repo>/app/src/main/cpp -> ../../../../ = <repo>
get_filename_component(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core" ABSOLUTE)

if(NOT EXISTS "${CORE_DIR}/src/synth.c")
    message(FATAL_ERROR "core/ が見つかりません: ${CORE_DIR}")
endif()

add_library(yukkuri SHARED
    ${CORE_DIR}/src/plan.c
    ${CORE_DIR}/src/phones.c
    ${CORE_DIR}/src/presets.c
    ${CORE_DIR}/src/synth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/jni.c
)
target_include_directories(yukkuri PRIVATE ${CORE_DIR}/include)
target_link_libraries(yukkuri log m)
