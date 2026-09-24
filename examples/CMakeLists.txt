# Examples are built as ordinary consumers of the installed-style target so a
# reader can copy one file and link it against lqf::lqf.
foreach(example quickstart embedded_persistence serve_and_query)
  add_executable(lqf_example_${example} ${example}.cpp)
  target_link_libraries(lqf_example_${example} PRIVATE lqf::lqf)
  lqf_apply_warnings(lqf_example_${example})
endforeach()
