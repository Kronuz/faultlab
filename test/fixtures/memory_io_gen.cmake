# faultlab_memory_io_gen: records test/fixtures/memory_io_states.txt from journal's MemoryIO.
# journal:a412175 has MemoryIO (journal:9f5e846 removes it), so this test-only
# reference pin stays; it fetches headers only (SOURCE_SUBDIR names no CMakeLists).
FetchContent_Declare(journal_src GIT_REPOSITORY https://github.com/Kronuz/journal.git GIT_TAG a412175fe437b59463bcaa97ebc75429643bdc65 SOURCE_SUBDIR faultlab-headers-only)
FetchContent_MakeAvailable(journal_src)
add_executable(faultlab_memory_io_gen ${CMAKE_CURRENT_LIST_DIR}/memory_io_gen.cc)
target_link_libraries(faultlab_memory_io_gen PRIVATE faultlab::faultlab)
target_include_directories(faultlab_memory_io_gen PRIVATE "${journal_src_SOURCE_DIR}/include")
target_compile_definitions(faultlab_memory_io_gen PRIVATE FAULTLAB_JOURNAL_COMMIT="a412175fe437b59463bcaa97ebc75429643bdc65")
