add_library(game_tictactoe STATIC
    tictactoe_state.cpp
    tictactoe_rules.cpp
    tictactoe_net_adapter.cpp
)
target_include_directories(game_tictactoe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game_tictactoe PUBLIC dinoboard_core dinoboard_search)
