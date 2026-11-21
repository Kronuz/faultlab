cmake_minimum_required(VERSION 3.18)
project(faultlab LANGUAGES CXX)

# Fault injection for storage and cluster tests: faultfs, the in-process crash and media model
# over completion's IO seam, a recording IO, the seeded scenario line and the JSON-line event
# log. Header-only. The Python half (python/faultlab) is stdlib only.
include(FetchContent)
# completion's io.h: the File and IO seam faultfs implements, with preallocate, advise,
# map_shared and link (the faultlab seam, completion:d8b538c).
# A consumer that already defines completion (completion's own tests) keeps its own copy.
if(NOT TARGET completion::completion)
	FetchContent_Declare(completion GIT_REPOSITORY https://github.com/Kronuz/completion.git GIT_TAG d8b538c6778f0a1b969e42c8428995d1d29d4731)
	FetchContent_MakeAvailable(completion)
endif()

add_library(faultlab INTERFACE)
target_include_directories(faultlab INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(faultlab INTERFACE cxx_std_20)
target_link_libraries(faultlab INTERFACE completion::completion)
add_library(faultlab::faultlab ALIAS faultlab)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	enable_testing()
	set(FAULTLAB_TESTS faultfs model policy seeds sector recorder scenario events)
	foreach(t ${FAULTLAB_TESTS})
		add_executable(faultlab_${t}_test test/${t}_test.cc)
		target_link_libraries(faultlab_${t}_test PRIVATE faultlab::faultlab)
		add_test(NAME faultlab_${t} COMMAND faultlab_${t}_test)
		set_tests_properties(faultlab_${t} PROPERTIES TIMEOUT 300)
	endforeach()
	# sector_test checks the MemoryIO states recorded in test/fixtures/memory_io_states.txt.
	target_compile_definitions(faultlab_sector_test PRIVATE FAULTLAB_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
	# Off by default: the generator that records the fixture from journal's live MemoryIO. Only
	# this option fetches journal (headers only: SOURCE_SUBDIR names no CMakeLists).
	option(FAULTLAB_REGENERATE_MEMORY_IO "Build faultlab_memory_io_gen, which records memory_io_states.txt from journal" OFF)
	if(FAULTLAB_REGENERATE_MEMORY_IO)
		FetchContent_Declare(journal_memory_io GIT_REPOSITORY https://github.com/Kronuz/journal.git GIT_TAG a412175fe437b59463bcaa97ebc75429643bdc65 SOURCE_SUBDIR faultlab-headers-only)
		FetchContent_MakeAvailable(journal_memory_io)
		add_executable(faultlab_memory_io_gen test/fixtures/memory_io_gen.cc)
		target_link_libraries(faultlab_memory_io_gen PRIVATE faultlab::faultlab)
		target_include_directories(faultlab_memory_io_gen PRIVATE "${journal_memory_io_SOURCE_DIR}/include")
		target_compile_definitions(faultlab_memory_io_gen PRIVATE FAULTLAB_JOURNAL_COMMIT="a412175fe437b59463bcaa97ebc75429643bdc65")
	endif()
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.scratch")  # the tests' state
	add_executable(faultlab_demo examples/demo.cc)
	target_link_libraries(faultlab_demo PRIVATE faultlab::faultlab)
	add_test(NAME faultlab_demo COMMAND faultlab_demo)

	# The Python half's suites, isolated (python3 -I) as consumers run it.
	find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
	foreach(t selftest proxy_test scenario_test netlab_test disklab_test)
		add_test(NAME faultlab_py_${t} COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/${t}.py")
		set_tests_properties(faultlab_py_${t} PROPERTIES TIMEOUT 600)
	endforeach()
	# The C++ event log, read by the Python checker: a clean history passes, a stale read fails
	# and prints its replay line.
	set(log "${CMAKE_CURRENT_BINARY_DIR}/.scratch/events_check")
	add_test(NAME faultlab_events_write COMMAND faultlab_events_test "${log}.jsonl")
	add_test(NAME faultlab_events_write_stale COMMAND faultlab_events_test "${log}_stale.jsonl" stale)
	set_tests_properties(faultlab_events_write faultlab_events_write_stale PROPERTIES FIXTURES_SETUP events_logs)
	add_test(NAME faultlab_events_check COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/python/faultlab/history.py" "${log}.jsonl")
	add_test(NAME faultlab_events_check_stale COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/python/faultlab/history.py" "${log}_stale.jsonl")
	set_tests_properties(faultlab_events_check faultlab_events_check_stale PROPERTIES FIXTURES_REQUIRED events_logs)
	set_tests_properties(faultlab_events_check_stale PROPERTIES PASS_REGULAR_EXPRESSION "faults inside: partition.*replay: FAULTLAB_SCENARIO='faultlab/1 name=events seed=3'")
endif()
