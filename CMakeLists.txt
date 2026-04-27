cmake_minimum_required(VERSION 3.16)
project(DinoBoard CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(engine)
add_subdirectory(games/tictactoe)
add_subdirectory(games/splendor)
