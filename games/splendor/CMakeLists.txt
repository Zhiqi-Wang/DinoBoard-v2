add_library(game_splendor STATIC
    splendor_state.cpp
    splendor_rules.cpp
    splendor_net_adapter.cpp
    splendor_register.cpp
)
target_include_directories(game_splendor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game_splendor PUBLIC dinoboard_core)
