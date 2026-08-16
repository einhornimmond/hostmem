# One executable per bench_*.c, the same set build.zig produces. Globbed rather than listed:
# a benchmark is a single self contained file, so a new one needs no entry anywhere.
#
# Numbers from a debug build answer a different question — configure Release before reporting
# any of them. See AGENTS.md.

FILE(GLOB BENCH_SOURCES "src/bench_*.c")

foreach(bench_source ${BENCH_SOURCES})
  get_filename_component(bench_name ${bench_source} NAME_WE)
  add_executable(${bench_name} ${bench_source})
  target_link_libraries(${bench_name} PRIVATE hostmem)
  # bench_report.h sits beside the sources and is shared by all of them
  target_include_directories(${bench_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(${bench_name} PROPERTIES FOLDER benchmarks)
endforeach()
