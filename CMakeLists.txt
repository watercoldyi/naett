cmake_minimum_required(VERSION 3.15)

project(naett VERSION 1.0.0 LANGUAGES C)

include(GNUInstallDirs)

# 默认构建静态库。naett 的 Windows 后端没有显式导出宏，
# 开启共享库时通过 WINDOWS_EXPORT_ALL_SYMBOLS 自动导出符号。
option(NAETT_BUILD_SHARED "Build naett as a shared library" OFF)

if(NAETT_BUILD_SHARED)
    add_library(naett SHARED naett.c)
    if(WIN32)
        set_target_properties(naett PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()
else()
    add_library(naett STATIC naett.c)
endif()

# 供 add_subdirectory / FetchContent 使用时的命名空间别名
add_library(naett::naett ALIAS naett)

target_include_directories(naett
    PUBLIC
        # 源码树内：头文件就在仓库根目录
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        # 安装后：头文件位于 <prefix>/include
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(WIN32)
    # Windows 后端基于 WinHTTP
    target_link_libraries(naett PUBLIC winhttp)
    if(MSVC)
        # 复合字面量等 C99/C11 特性需要 VS2019 16.8+
        target_compile_options(naett PRIVATE /W3 /std:c11)
        target_compile_definitions(naett PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(naett PRIVATE -Wall)
    endif()
elseif(APPLE)
    # macOS/iOS 后端通过 objc runtime 调用 NSURLSession。
    # 这里用 "-framework Foundation" 而不是 find_library 的绝对路径，
    # 避免导出的 CMake package 绑定到构建机上的 Xcode SDK 路径。
    target_link_libraries(naett PUBLIC "-framework Foundation" "-framework CoreFoundation")
    # naett.h 的 countOptions 使用了 GNU 的 ({ ... }) 变参宏扩展
    target_compile_options(naett PRIVATE
        -Wall
        -fblocks
        -Wno-gnu-zero-variadic-macro-arguments
    )
else()
    message(FATAL_ERROR "naett: only Windows and macOS are supported for now")
endif()

# ----- 安装规则 -----
install(TARGETS naett EXPORT naettTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES naett.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(NAETT_BUILD_SHARED)
    install(FILES naett.lua DESTINATION ${CMAKE_INSTALL_DATADIR}/lua/5.1)
endif()

# 导出 CMake package，安装后支持 find_package(naett CONFIG)
include(CMakePackageConfigHelpers)

set(NAETT_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/naett)

configure_package_config_file(
    cmake/naettConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/naettConfig.cmake
    INSTALL_DESTINATION ${NAETT_CMAKE_INSTALL_DIR}
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/naettConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT naettTargets
    FILE naettTargets.cmake
    NAMESPACE naett::
    DESTINATION ${NAETT_CMAKE_INSTALL_DIR}
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/naettConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/naettConfigVersion.cmake
    DESTINATION ${NAETT_CMAKE_INSTALL_DIR}
)
