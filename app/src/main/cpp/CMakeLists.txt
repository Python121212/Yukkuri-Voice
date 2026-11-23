cmake_minimum_required(VERSION 3.22.1)
project(yukkuri_voice C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS_RELEASE "-O2 -ffast-math -fvisibility=hidden")

# リポジトリ直下の core/ をそのまま使う (エンジンは Android 非依存)
set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core)

add_library(yukkuri SHARED
    ${CORE_DIR}/src/plan.c
    ${CORE_DIR}/src/phones.c
    ${CORE_DIR}/src/presets.c
    ${CORE_DIR}/src/synth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/jni.c
)
target_include_directories(yukkuri PRIVATE ${CORE_DIR}/include)
target_link_libraries(yukkuri log m)
