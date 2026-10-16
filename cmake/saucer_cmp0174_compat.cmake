# 兼容 saucer-embed v1.1.1：其 cmake/embed.cmake 首行直接执行
# `cmake_policy(SET CMP0174 NEW)`，但 CMP0174 在 CMake < 3.31 中尚不存在，
# 会导致 configure 报错。此脚本通过 CMAKE_PROJECT_INCLUDE 在每次 project()
# 调用末尾执行，在 include(embed.cmake) 之前自动加上 if(POLICY) 保护。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake" _saucer_embed_cmake)
    if(_saucer_embed_cmake MATCHES "cmake_policy\\(SET CMP0174 NEW\\)" AND
       NOT _saucer_embed_cmake MATCHES "if\\(POLICY CMP0174\\)")
        string(REPLACE
            "cmake_policy(SET CMP0174 NEW)"
            "if(POLICY CMP0174)\n    cmake_policy(SET CMP0174 NEW)\nendif()"
            _saucer_embed_cmake "${_saucer_embed_cmake}")
        file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake" "${_saucer_embed_cmake}")
    endif()
endif()
