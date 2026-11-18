cmake_minimum_required(VERSION 3.16)
project(faultlab LANGUAGES CXX)

# Fault injection for storage and cluster tests: faultfs, the in-process crash and media model
# over completion's IO seam, a recording IO, the seeded scenario line and the JSON-line event
# log. Header-only. The Python half (python/faultlab) is stdlib only.
include(FetchContent)
# completion's io.h: the File and IO seam faultfs implements, with preallocate, advise,
# map_shared and link (the faultlab seam, completion:d8b538c).
FetchContent_Declare(completion GIT_REPOSITORY https://github.com/Kronuz/completion.git GIT_TAG d8b538c6778f0a1b969e42c8428995d1d29d4731)
FetchContent_MakeAvailable(completion)

add_library(faultlab INTERFACE)
target_include_directories(faultlab INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(faultlab INTERFACE cxx_std_20)
target_link_libraries(faultlab INTERFACE completion::completion)
add_library(faultlab::faultlab ALIAS faultlab)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	enable_testing()
	set(FAULTLAB_TESTS)
	foreach(t ${FAULTLAB_TESTS})
		add_executable(faultlab_${t}_test test/${t}_test.cc)
		target_link_libraries(faultlab_${t}_test PRIVATE faultlab::faultlab)
		add_test(NAME faultlab_${t} COMMAND faultlab_${t}_test)
		set_tests_properties(faultlab_${t} PROPERTIES TIMEOUT 300)
	endforeach()
endif()
