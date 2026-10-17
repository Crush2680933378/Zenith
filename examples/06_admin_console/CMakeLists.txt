# 06_admin_console —— DB 管理台（需 uORM；仅 ZENITH_ENABLE_ORM=ON 时构建）
if (NOT TARGET uORM::uorm)
    message(STATUS "06_admin_console: uORM 未集成，跳过")
    return()
endif()

add_executable(admin_demo main.cpp)
target_link_libraries(admin_demo PRIVATE zenith Threads::Threads)
zenith_set_warnings(admin_demo)
