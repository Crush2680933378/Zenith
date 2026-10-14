# 03_websocket —— WebSocket 广播 + REST 示例
add_executable(ws_demo main.cpp)

target_link_libraries(ws_demo PRIVATE zenith Threads::Threads)

zenith_set_warnings(ws_demo)
