add_executable(lqf_bench lqf_bench.cpp)
target_link_libraries(lqf_bench PRIVATE lqf::lqf)
lqf_apply_warnings(lqf_bench)
