add_library(game_loveletter STATIC
    loveletter_state.cpp
    loveletter_rules.cpp
    loveletter_net_adapter.cpp
    loveletter_register.cpp
)
target_include_directories(game_loveletter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game_loveletter PUBLIC dinoboard_core)
