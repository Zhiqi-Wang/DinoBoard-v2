add_library(game_azul STATIC
    azul_state.cpp
    azul_rules.cpp
    azul_net_adapter.cpp
    azul_register.cpp
)
target_include_directories(game_azul PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game_azul PUBLIC dinoboard_core)
