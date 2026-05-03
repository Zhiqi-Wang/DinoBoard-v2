add_library(game_quoridor STATIC
    quoridor_state.cpp
    quoridor_rules.cpp
    quoridor_net_adapter.cpp
    quoridor_register.cpp
)
target_include_directories(game_quoridor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game_quoridor PUBLIC dinoboard_core)
