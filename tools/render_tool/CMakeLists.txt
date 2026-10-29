add_executable(render_tool main.cpp)
target_link_libraries(render_tool PRIVATE dropforge_core dropforge_juce_headless)
target_compile_definitions(render_tool PRIVATE DROPFORGE_VERSION="${PROJECT_VERSION}")
