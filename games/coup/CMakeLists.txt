add_library(game_coup STATIC
    coup_state.cpp
    coup_rules.cpp
    coup_net_adapter.cpp
    coup_register.cpp
)
target_include_directories(game_coup PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game_coup PUBLIC dinoboard_core)
