cmake_minimum_required(VERSION 3.10)
project(MatchText LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MATCHTEXT_VERSION "1.0.0")

include(CTest)

add_library(matchtextlib
  MatchText/Statistics.cpp
  MatchText/Utils.cpp
  MatchText/miniz.c
  MatchText/miniz_tdef.c
  MatchText/miniz_tinfl.c
  MatchText/miniz_zip.c
)

target_include_directories(matchtextlib PUBLIC MatchText)

find_package(Threads REQUIRED)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(POPPLER_CPP QUIET poppler-cpp)
endif()

add_executable(matchtext
  MatchText/MatchText.cpp
)

set(MATCHTEXT_FS_LIB "")
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  set(MATCHTEXT_FS_LIB stdc++fs)
endif()

target_link_libraries(matchtext PRIVATE matchtextlib Threads::Threads ${MATCHTEXT_FS_LIB})

if (POPPLER_CPP_FOUND)
  target_compile_definitions(matchtextlib PUBLIC HAVE_POPPLER_CPP=1)
  target_include_directories(matchtextlib PUBLIC ${POPPLER_CPP_INCLUDE_DIRS})
  target_link_libraries(matchtextlib PUBLIC ${POPPLER_CPP_LIBRARIES})
endif()

add_executable(mostsimilar
  MatchText/MostSimilar.cpp
)
target_link_libraries(mostsimilar PRIVATE matchtextlib ${MATCHTEXT_FS_LIB})

install(TARGETS matchtext mostsimilar RUNTIME DESTINATION bin)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "textmatching")
set(CPACK_PACKAGE_VERSION "${MATCHTEXT_VERSION}")
set(CPACK_PACKAGE_CONTACT "TextMatching Maintainers")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "TextMatching Maintainers")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libstdc++6, libc6")
include(CPack)

add_custom_target(deb ALL
  COMMAND ${CMAKE_COMMAND}
          -DCPACK_CMD=${CMAKE_CPACK_COMMAND}
          -DCPACK_OUT_DIR=${CMAKE_BINARY_DIR}/packages
          -P ${CMAKE_SOURCE_DIR}/cmake/RunCpack.cmake
  DEPENDS matchtext mostsimilar
  COMMENT "Building DEB package"
)

if (BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
  )
  FetchContent_MakeAvailable(doctest)

  add_executable(matchtext_tests
    tests/StatisticsTests.cpp
  )
  target_compile_definitions(matchtext_tests PRIVATE MATCHTEXT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_link_libraries(matchtext_tests PRIVATE doctest::doctest matchtextlib ${MATCHTEXT_FS_LIB})

  add_test(NAME matchtext_tests COMMAND matchtext_tests)
endif()
