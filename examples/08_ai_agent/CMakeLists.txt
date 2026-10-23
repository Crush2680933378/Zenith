# 08_ai_agent —— AI Agent 后端全链路（推理+编排+MCP+向量）
add_executable(ai_agent_demo main.cpp)
target_link_libraries(ai_agent_demo PRIVATE zenith Threads::Threads)
zenith_set_warnings(ai_agent_demo)
