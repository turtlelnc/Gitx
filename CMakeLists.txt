cmake_minimum_required(VERSION 3.24)

project(gitx VERSION 1.0.0 LANGUAGES CXX)

option(GITX_FETCH_DEPS "Download libgit2 when it is not installed" ON)
option(GITX_ENABLE_SSH "Enable SSH transport support (requires libssh2)" ON)
option(GITX_ENABLE_HTTPS "Enable HTTPS transport support (OpenSSL/SecureTransport/WinHTTP)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(libgit2 CONFIG QUIET)

if(NOT TARGET libgit2::libgit2 AND NOT TARGET libgit2)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libgit2-1.8.4/CMakeLists.txt")
    set(BUILD_CLAR OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(USE_SSH ${GITX_ENABLE_SSH} CACHE BOOL "" FORCE)
    set(USE_HTTPS ${GITX_ENABLE_HTTPS} CACHE BOOL "" FORCE)
    add_subdirectory(third_party/libgit2-1.8.4 EXCLUDE_FROM_ALL)
  elseif(NOT GITX_FETCH_DEPS)
    message(FATAL_ERROR "libgit2 was not found. Install it or configure with -DGITX_FETCH_DEPS=ON.")
  else()
    include(FetchContent)
    set(BUILD_CLAR OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(USE_SSH ${GITX_ENABLE_SSH} CACHE BOOL "" FORCE)
    set(USE_HTTPS ${GITX_ENABLE_HTTPS} CACHE BOOL "" FORCE)
    FetchContent_Declare(
      libgit2
      URL https://github.com/libgit2/libgit2/archive/refs/tags/v1.8.4.zip
    )
    FetchContent_MakeAvailable(libgit2)
  endif()
endif()

if(TARGET zlib)
  set(GITX_ZLIB_TARGET zlib)
else()
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  include(FetchContent)
  FetchContent_Declare(zlib URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.zip)
  FetchContent_MakeAvailable(zlib)
  set(GITX_ZLIB_TARGET zlibstatic)
else()
  set(GITX_ZLIB_TARGET ZLIB::ZLIB)
endif()
endif()

if(TARGET libgit2::libgit2)
  set(GITX_LIBGIT2_TARGET libgit2::libgit2)
elseif(TARGET libgit2package)
  set(GITX_LIBGIT2_TARGET libgit2package)
else()
  set(GITX_LIBGIT2_TARGET libgit2)
endif()

add_library(gitx_core
  src/config.cpp
  src/command_names.cpp
  src/git_repository.cpp
  src/bundle.cpp
)
target_include_directories(gitx_core PUBLIC include)
target_compile_definitions(gitx_core PUBLIC GITX_VERSION="${PROJECT_VERSION}")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libgit2-1.8.4/include/git2.h")
  target_include_directories(gitx_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libgit2-1.8.4/include")
endif()
if(TARGET zlib)
  get_target_property(GITX_ZLIB_INCLUDE_DIRS zlib INCLUDE_DIRECTORIES)
  target_include_directories(gitx_core PRIVATE ${GITX_ZLIB_INCLUDE_DIRS})
endif()
target_link_libraries(gitx_core PUBLIC ${GITX_LIBGIT2_TARGET} ${GITX_ZLIB_TARGET})

add_executable(gitx src/main.cpp)
target_link_libraries(gitx PRIVATE gitx_core)

include(CTest)
if(BUILD_TESTING)
  add_executable(gitx_tests tests/config_tests.cpp)
  target_link_libraries(gitx_tests PRIVATE gitx_core)
  add_test(NAME gitx.config COMMAND gitx_tests)

  add_executable(gitx_bundle_launcher tests/bundle_launcher.cpp)
  target_link_libraries(gitx_bundle_launcher PRIVATE gitx_core)

  add_executable(gitx_repo_tests tests/git_repository_tests.cpp)
  target_link_libraries(gitx_repo_tests PRIVATE gitx_core)
  add_test(NAME gitx.repository COMMAND gitx_repo_tests)

  add_executable(gitx_bundle_tests tests/bundle_tests.cpp)
  target_link_libraries(gitx_bundle_tests PRIVATE gitx_core)
  target_compile_definitions(gitx_bundle_tests PRIVATE
    BUNDLE_LAUNCHER="$<TARGET_FILE:gitx_bundle_launcher>")
  add_test(NAME gitx.bundle COMMAND gitx_bundle_tests)
endif()
