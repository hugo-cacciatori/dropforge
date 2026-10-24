add_executable(render_tool main.cpp)
target_link_libraries(render_tool PRIVATE dropforge_core juce::juce_audio_formats
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
target_compile_definitions(render_tool PRIVATE
    DROPFORGE_VERSION="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_USE_FLAC=0 JUCE_USE_OGGVORBIS=0 JUCE_USE_OPUS=0
    JUCE_USE_MP3AUDIOFORMAT=0 JUCE_USE_LAME_AUDIO_FORMAT=0
    JUCE_USE_WINDOWS_MEDIA_FORMAT=0)
