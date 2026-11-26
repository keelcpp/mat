cmake_minimum_required(VERSION 3.20)
project(keel_matfile VERSION 0.2.0 LANGUAGES CXX)

option(KEEL_MATFILE_NO_EXCEPTIONS_RTTI "Compile keel_matfile without C++ exceptions and RTTI" OFF)
option(KEEL_MATFILE_BUILD_TESTS "Build keel_matfile unit tests and header purity checks" ${PROJECT_IS_TOP_LEVEL})

if(NOT TARGET keel::keel)
    find_package(keel QUIET CONFIG)
endif()
if(NOT TARGET keel::keel)
    include(FetchContent)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../keel/CMakeLists.txt" AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_KEEL)
        set(FETCHCONTENT_SOURCE_DIR_KEEL "${CMAKE_CURRENT_SOURCE_DIR}/../keel" CACHE PATH "Local keel checkout")
    endif()
    if(KEEL_MATFILE_NO_EXCEPTIONS_RTTI)
        set(KEEL_NO_EXCEPTIONS_RTTI ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        keel
        GIT_REPOSITORY https://github.com/keelcpp/keel.git
        GIT_TAG        4b37783925fe7af7a64daaff90f268df267fbbf3
    )
    FetchContent_MakeAvailable(keel)
endif()

if(NOT TARGET keel::compress)
    find_package(keel_compress QUIET CONFIG)
    if(TARGET keel::keel_compress AND NOT TARGET keel::compress)
        add_library(keel::compress ALIAS keel::keel_compress)
    endif()
endif()
if(NOT TARGET keel::compress)
    include(FetchContent)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../compress/CMakeLists.txt" AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_KEEL_COMPRESS)
        set(FETCHCONTENT_SOURCE_DIR_KEEL_COMPRESS "${CMAKE_CURRENT_SOURCE_DIR}/../compress" CACHE PATH "Local keel_compress checkout")
    endif()
    if(KEEL_MATFILE_NO_EXCEPTIONS_RTTI)
        set(KEEL_COMPRESS_NO_EXCEPTIONS_RTTI ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        keel_compress
        GIT_REPOSITORY https://github.com/keelcpp/compress.git
        GIT_TAG        6378540afae060da68da60fe78685954a8799e6c
    )
    FetchContent_MakeAvailable(keel_compress)
endif()

add_library(keel_matfile STATIC
    src/mat.cpp
)
add_library(keel::matfile ALIAS keel_matfile)

target_link_libraries(keel_matfile
    PUBLIC keel::keel
    PRIVATE keel::compress
)

target_compile_features(keel_matfile PUBLIC cxx_std_20)
set_target_properties(keel_matfile PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(keel_matfile PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(keel_matfile PRIVATE /W4 /utf-8)
    if(KEEL_MATFILE_NO_EXCEPTIONS_RTTI)
        string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        string(REGEX REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        target_compile_options(keel_matfile PUBLIC /EHs-c- /GR-)
        target_compile_definitions(keel_matfile PUBLIC _HAS_EXCEPTIONS=0)
    endif()
else()
    target_compile_options(keel_matfile PRIVATE -Wall -Wextra -Wpedantic)
    if(KEEL_MATFILE_NO_EXCEPTIONS_RTTI)
        target_compile_options(keel_matfile PUBLIC -fno-exceptions -fno-rtti)
    endif()
endif()

if(COMMAND keel_enforce_guidelines)
    keel_enforce_guidelines(keel_matfile
        SCOPE ALL
        BAN_RAW_INTS ALL
        BAN_RAW_FLOATS ALL
        HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
        STANDALONE_HEADERS ${KEEL_MATFILE_BUILD_TESTS}
        TEST_NAME test_guidelines_headers
        WARNINGS OFF
    )
endif()

include(CMakePackageConfigHelpers)
install(TARGETS keel_matfile EXPORT keel_matfileTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/keel DESTINATION include)
install(EXPORT keel_matfileTargets
    FILE keel_matfileTargets.cmake
    NAMESPACE keel::
    DESTINATION lib/cmake/keel_matfile
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/keel_matfileConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/keel_matfileConfig.cmake"
    INSTALL_DESTINATION lib/cmake/keel_matfile
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/keel_matfileConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/keel_matfileConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/keel_matfileConfigVersion.cmake"
    DESTINATION lib/cmake/keel_matfile
)

if(KEEL_MATFILE_BUILD_TESTS)
    enable_testing()

    add_executable(test_matfile tests/test_matfile.cpp)
    target_link_libraries(test_matfile PRIVATE keel::matfile)
    if(MSVC)
        target_compile_options(test_matfile PRIVATE /W4 /utf-8)
    else()
        target_compile_options(test_matfile PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(COMMAND keel_enforce_guidelines)
        keel_enforce_guidelines(test_matfile
            SCOPE ALL
            BAN_RAW_INTS ALL
            BAN_RAW_FLOATS ALL
            WARNINGS OFF
        )
    endif()
    add_test(NAME test_matfile COMMAND test_matfile)
endif()
