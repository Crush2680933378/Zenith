# 09_crew —— 多 Agent SDLC 集群示例
add_executable(crew_demo main.cpp)
target_link_libraries(crew_demo PRIVATE zenith Threads::Threads)
zenith_set_warnings(crew_demo)
