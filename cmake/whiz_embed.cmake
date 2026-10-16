# waty_embed：将指定目录下的所有文件生成嵌入式资源源文件，并创建独立资源目标。
#
# 用法：
#   waty_embed(<directory> [NAME <name>])
#
# 默认目标别名 waty::embedded；通过 target_link_libraries(my_app PRIVATE waty::embedded) 链接。
# 支持通过 NAME 创建多个独立资源目标，如 waty_embed("${dir}" NAME ui)。

function(_waty_generate_embedded_sources directory target out_var)
    get_filename_component(web_root "${directory}" ABSOLUTE)
    if(NOT EXISTS "${web_root}")
        message(FATAL_ERROR "waty_embed: 目录不存在: ${web_root}")
    endif()

    file(GLOB_RECURSE waty_embed_files CONFIGURE_DEPENDS "${web_root}/*")

    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/waty_embedded/${target}")
    file(MAKE_DIRECTORY "${gen_dir}")
    set(gen_cpp "${gen_dir}/resources.cpp")

    set(declarations "")
    set(registrations "")
    set(index 0)

    foreach(file IN LISTS waty_embed_files)
        if(IS_DIRECTORY "${file}")
            continue()
        endif()

        file(RELATIVE_PATH rel "${web_root}" "${file}")
        string(REPLACE "\\" "/" rel "${rel}")

        file(READ "${file}" content HEX)
        string(REGEX MATCHALL ".." bytes "${content}")
        list(TRANSFORM bytes PREPEND "0x")
        list(JOIN bytes ", " bytes_str)

        string(APPEND declarations "static const unsigned char waty_data_${index}[] = {${bytes_str}};\n")
        string(APPEND registrations
               "    waty::detail::register_embedded_resource(\"${rel}\", std::string(reinterpret_cast<const char *>(waty_data_${index}), sizeof(waty_data_${index})));\n")

        math(EXPR index "${index} + 1")
    endforeach()

    if(index EQUAL 0)
        # 空目录：生成空的注册函数即可
        set(registrations "    (void)0;\n")
    endif()

    file(WRITE "${gen_cpp}"
"// 由 waty_embed 自动生成，请勿手动修改。\n"
"#include <waty/detail/embedded.hpp>\n"
"#include <string>\n"
"\n"
"namespace\n"
"{\n"
"${declarations}"
"void waty_embed_register_all()\n"
"{\n"
"${registrations}"
"}\n"
"\n"
"struct WatyEmbedInitializer\n"
"{\n"
"    WatyEmbedInitializer()\n"
"    {\n"
"        waty_embed_register_all();\n"
"    }\n"
"};\n"
"\n"
"static WatyEmbedInitializer g_waty_embed_initializer;\n"
"} // namespace\n"
    )

    set(${out_var} "${gen_cpp}" PARENT_SCOPE)
endfunction()

function(waty_embed directory)
    cmake_parse_arguments(PARSE_ARGV 1 EMBED "" "NAME" "")

    if(NOT EMBED_NAME)
        set(EMBED_NAME "embedded")
    endif()

    get_filename_component(embed_dir "${directory}" ABSOLUTE)
    if(NOT EXISTS "${embed_dir}")
        message(FATAL_ERROR "waty_embed: 目录不存在: ${embed_dir}")
    endif()

    set(embed_lib "waty_${EMBED_NAME}")

    # 使用 OBJECT 库而非 STATIC 库：生成的 resources.cpp 仅含静态初始化器、没有外部符号，
    # STATIC 库会被链接器整段丢弃（不会拉取该 .o），导致资源静默丢失。
    add_library(${embed_lib} OBJECT)
    add_library("waty::${EMBED_NAME}" ALIAS ${embed_lib})

    _waty_generate_embedded_sources("${embed_dir}" "${embed_lib}" embed_sources)
    target_sources(${embed_lib} PRIVATE ${embed_sources})

    target_include_directories(${embed_lib} PUBLIC "${embed_dir}")

    # 生成的 resources.cpp 需要包含 <waty/detail/embedded.hpp>
    get_filename_component(_waty_include_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include" ABSOLUTE)
    target_include_directories(${embed_lib} PRIVATE "${_waty_include_dir}")

    target_compile_features(${embed_lib} PRIVATE cxx_std_23)

    message(STATUS "waty: Created embedded resource target 'waty::${EMBED_NAME}' from '${embed_dir}'")
endfunction()
