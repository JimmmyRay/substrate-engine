# One line, and that is the target: a game is a name and a list of sources.
# `substrate_game` is defined in the root CMakeLists and supplies the link, the warning
# set and the shader dependency.
substrate_game(demo DemoGame.cpp DemoWorld.cpp)
