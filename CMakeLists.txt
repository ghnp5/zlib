# CMake 3.21 or later is required for PROJECT_IS_TOP_LEVEL
cmake_minimum_required(VERSION 3.21...3.30)

project(zlib VERSION 1.3.1.1 LANGUAGES C)

# Where possible, projects should require CMake 3.21 or later and ensure policies CMP0077 and CMP0126 are set to NEW.
# This will prevent the project from discarding values set by parent projects as non-cache variables.
# XXX set(BUILD_SHARED_LIBS ON CACHE BOOL "Enable to build a shared library(DLL) instead a static one")
option(BUILD_SHARED_LIBS "Enable to build a shared library(DLL) instead a static one." ${PROJECT_IS_TOP_LEVEL})
option(ZLIB_BUILD_TESTING "Enable to build the test too." ${PROJECT_IS_TOP_LEVEL})
option(ZLIB_BUILD_EXAMPLES "Enable to build the examples too." ${PROJECT_IS_TOP_LEVEL})

include(CheckTypeSize)
include(CheckFunctionExists)
include(CheckIncludeFile)

check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(stdint.h HAVE_STDINT_H)
check_include_file(stddef.h HAVE_STDDEF_H)

#
# Check to see if we have large file support
#
set(CMAKE_REQUIRED_DEFINITIONS -D_LARGEFILE64_SOURCE=1)
if(HAVE_SYS_TYPES_H)
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -DHAVE_SYS_TYPES_H)
endif()
if(HAVE_STDINT_H)
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -DHAVE_STDINT_H)
endif()
if(HAVE_STDDEF_H)
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -DHAVE_STDDEF_H)
endif()
check_type_size(off64_t OFF64_T)
if(HAVE_OFF64_T)
    add_definitions(-D_LARGEFILE64_SOURCE=1)
endif()
set(CMAKE_REQUIRED_DEFINITIONS) # clear variable

#
# Check for fseeko
#
check_function_exists(fseeko HAVE_FSEEKO)
if(NOT HAVE_FSEEKO)
    add_definitions(-DNO_FSEEKO)
endif()

#
# Check for unistd.h
#
check_include_file(unistd.h Z_HAVE_UNISTD_H)

if(MSVC)
    set(CMAKE_DEBUG_POSTFIX "d")
    add_definitions(-D_CRT_SECURE_NO_DEPRECATE)
    add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR})
endif()

option(RENAME_ZCONF "Rename the zconf when building out of source" ON)
if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR AND RENAME_ZCONF)
    # If we're doing an out of source build and the user has a zconf.h
    # in their source tree...
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/zconf.h)
        message(STATUS "Renaming")
        message(STATUS "    ${CMAKE_CURRENT_SOURCE_DIR}/zconf.h")
        message(STATUS "to 'zconf.h.included' because this file is included with zlib")
        message(STATUS "but CMake generates it automatically in the build directory.")
        file(RENAME ${CMAKE_CURRENT_SOURCE_DIR}/zconf.h ${CMAKE_CURRENT_SOURCE_DIR}/zconf.h.included)
    endif()
endif()

set(ZLIB_PC ${CMAKE_CURRENT_BINARY_DIR}/zlib.pc)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/zlib.pc.cmakein ${ZLIB_PC} @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/zconf.h.cmakein ${CMAKE_CURRENT_BINARY_DIR}/zconf.h @ONLY)
# XXX include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR})

#============================================================================
# zlib
#============================================================================

set(ZLIB_PUBLIC_HDRS ${CMAKE_CURRENT_BINARY_DIR}/zconf.h zlib.h)
set(ZLIB_PRIVATE_HDRS
    crc32.h
    deflate.h
    gzguts.h
    inffast.h
    inffixed.h
    inflate.h
    inftrees.h
    trees.h
    zutil.h
)
set(ZLIB_SRCS
    adler32.c
    compress.c
    crc32.c
    deflate.c
    gzclose.c
    gzlib.c
    gzread.c
    gzwrite.c
    inflate.c
    infback.c
    inftrees.c
    inffast.c
    trees.c
    uncompr.c
    zutil.c
)

if(NOT MINGW AND NOT MSYS)
    set(ZLIB_DLL_SRCS win32/zlib1.rc # If present will override custom build rule below.
    )
endif()

# parse the full version number from zlib.h and include in ZLIB_FULL_VERSION
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/zlib.h _zlib_h_contents)
string(REGEX REPLACE ".*#define[ \t]+ZLIB_VERSION[ \t]+\"([-0-9A-Za-z.]+)\".*" "\\1" ZLIB_FULL_VERSION ${_zlib_h_contents})

# TODO: add_library without SHARED/STATIC arg picks which one to use based on
# the variable BUILD_SHARED_LIBS. Maybe this could be simplified? (Herry Mallon)
if(BUILD_SHARED_LIBS AND MINGW)
    # This gets us DLL resource information when compiling on MinGW.
    if(NOT CMAKE_RC_COMPILER)
        set(CMAKE_RC_COMPILER windres.exe)
    endif()

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/zlib1rc.obj
        COMMAND ${CMAKE_RC_COMPILER} -D GCC_WINDRES -I ${CMAKE_CURRENT_SOURCE_DIR} -I ${CMAKE_CURRENT_BINARY_DIR} -o
                ${CMAKE_CURRENT_BINARY_DIR}/zlib1rc.obj -i ${CMAKE_CURRENT_SOURCE_DIR}/win32/zlib1.rc
    )
    set(ZLIB_DLL_SRCS ${CMAKE_CURRENT_BINARY_DIR}/zlib1rc.obj)
endif()

if(BUILD_SHARED_LIBS)
    add_library(zlib SHARED ${ZLIB_SRCS} ${ZLIB_DLL_SRCS} ${ZLIB_PUBLIC_HDRS} ${ZLIB_PRIVATE_HDRS})
    set_target_properties(zlib PROPERTIES DEFINE_SYMBOL ZLIB_DLL)
    set_target_properties(zlib PROPERTIES SOVERSION 1)
else()
    add_library(zlib STATIC ${ZLIB_SRCS} ${ZLIB_PUBLIC_HDRS} ${ZLIB_PRIVATE_HDRS})
endif()

target_include_directories(
    zlib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

if(BUILD_SHARED_LIBS AND NOT CYGWIN)
    # This property causes shared libraries on Linux to have the full version
    # encoded into their final filename.  We disable this on Cygwin because
    # it causes cygz-${ZLIB_FULL_VERSION}.dll to be created when cygz.dll
    # seems to be the default.
    #
    # This has no effect with MSVC, on that platform the version info for
    # the DLL comes from the resource file win32/zlib1.rc
    set_target_properties(zlib PROPERTIES VERSION ${ZLIB_FULL_VERSION})
endif()

if(UNIX)
    # On unix-like platforms the library is almost always called libz
    set_target_properties(zlib PROPERTIES OUTPUT_NAME z)
    if(NOT APPLE AND NOT (CMAKE_SYSTEM_NAME STREQUAL AIX))
        set_target_properties(zlib PROPERTIES LINK_FLAGS "-Wl,--version-script,\"${CMAKE_CURRENT_SOURCE_DIR}/zlib.map\"")
    endif()
elseif(BUILD_SHARED_LIBS AND WIN32)
    # Creates zlib1.dll when building shared library version
    set_target_properties(zlib PROPERTIES SUFFIX "1.dll")
endif()

add_library(zlib::zlib ALIAS zlib)
# Note also that CMake's own FindZLIB.cmake module defines a slightly different
# imported target named ZLIB::ZLIB (the upper/lowercase differences are meaningful).
add_library(ZLIB::ZLIB ALIAS zlib)

#============================================================================
# Example binaries and ctests locatet at test subdir
# TODO: add_subdirectory(test)
#============================================================================
if(ZLIB_BUILD_TESTING)
    enable_testing()

    add_executable(example test/example.c)
    target_link_libraries(example zlib)
    add_test(example example)

    if(HAVE_OFF64_T)
        add_executable(example64 test/example.c)
        target_link_libraries(example64 zlib)
        set_target_properties(example64 PROPERTIES COMPILE_FLAGS "-D_FILE_OFFSET_BITS=64")
        add_test(example64 example64)
    endif()
endif()

if(ZLIB_BUILD_EXAMPLES)
    add_executable(minigzip test/minigzip.c)
    target_link_libraries(minigzip zlib)

    if(HAVE_OFF64_T)
        add_executable(minigzip64 test/minigzip.c)
        target_link_libraries(minigzip64 zlib)
        set_target_properties(minigzip64 PROPERTIES COMPILE_FLAGS "-D_FILE_OFFSET_BITS=64")
    endif()
endif()

#============================================================================
# optional install part
#============================================================================
if(SKIP_INSTALL_ALL)
    return()
endif()

if(NOT SKIP_INSTALL_LIBRARIES)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(ZLIB_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/zlib")
    set(ZLIB_INSTALL_PKGCONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/share/pkgconfig")
    write_basic_package_version_file("zlibConfigVersion.cmake" COMPATIBILITY SameMajorVersion)

    target_include_directories(zlib PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    install(FILES "${PROJECT_BINARY_DIR}/zlibConfigVersion.cmake" DESTINATION "${ZLIB_INSTALL_CMAKEDIR}")
    install(TARGETS zlib
            EXPORT zlibConfig
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
    install(EXPORT zlibConfig NAMESPACE zlib:: DESTINATION "${ZLIB_INSTALL_CMAKEDIR}")
endif()
if(NOT SKIP_INSTALL_HEADERS)
    install(FILES ${ZLIB_PUBLIC_HDRS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()
if(NOT SKIP_INSTALL_FILES)
    install(FILES zlib.3 DESTINATION "${CMAKE_INSTALL_MANDIR}/man3")
    install(FILES ${ZLIB_PC} DESTINATION "${ZLIB_INSTALL_PKGCONFIG_DIR}")
endif()
