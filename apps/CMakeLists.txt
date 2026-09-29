add_executable(engine_main engine_main.cpp)
add_executable(replay_tool replay_tool.cpp)

target_include_directories(engine_main PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(replay_tool PRIVATE ${CMAKE_SOURCE_DIR}/include)