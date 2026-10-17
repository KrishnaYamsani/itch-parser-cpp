add_executable(engine_main engine_main.cpp)
add_executable(replay_tool replay_tool.cpp)

target_include_directories(engine_main PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(replay_tool PRIVATE ${CMAKE_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(engine_main PRIVATE Threads::Threads)

target_link_libraries(engine_main PRIVATE uring)
target_link_libraries(engine_main PRIVATE uring Threads::Threads)

add_executable(book_check book_check.cpp)
target_include_directories(book_check PRIVATE ${CMAKE_SOURCE_DIR}/include)