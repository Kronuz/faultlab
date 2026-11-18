cmake_minimum_required(VERSION 3.18)
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
	# journal's MemoryIO, the power-loss model sector_test compares faultfs against; its headers
	# only (SOURCE_SUBDIR names no CMakeLists, so journal's build is not added).
	FetchContent_Declare(journal_memory_io GIT_REPOSITORY https://github.com/Kronuz/journal.git GIT_TAG a412175fe437b59463bcaa97ebc75429643bdc65 SOURCE_SUBDIR faultlab-headers-only)
	FetchContent_MakeAvailable(journal_memory_io)
	set(FAULTLAB_TESTS faultfs policy seeds sector recorder scenario events)
	foreach(t ${FAULTLAB_TESTS})
		add_executable(faultlab_${t}_test test/${t}_test.cc)
		target_link_libraries(faultlab_${t}_test PRIVATE faultlab::faultlab)
		add_test(NAME faultlab_${t} COMMAND faultlab_${t}_test)
		set_tests_properties(faultlab_${t} PROPERTIES TIMEOUT 300)
	endforeach()
	target_include_directories(faultlab_sector_test PRIVATE "${journal_memory_io_SOURCE_DIR}/include")
endif()
