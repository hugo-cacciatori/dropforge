add_executable(dropforge_scaffold_tests ScaffoldTests.cpp)
target_link_libraries(dropforge_scaffold_tests PRIVATE dropforge_core)
add_test(NAME scaffold_silence COMMAND dropforge_scaffold_tests)

add_executable(dropforge_note_router_tests NoteRouterTests.cpp)
target_link_libraries(dropforge_note_router_tests PRIVATE dropforge_core)
add_test(NAME note_router COMMAND dropforge_note_router_tests)

add_executable(dropforge_mode_tests ModeTests.cpp)
target_link_libraries(dropforge_mode_tests PRIVATE dropforge_core)
add_test(NAME mode_switching COMMAND dropforge_mode_tests)

# Blade, the lead (SPEC sections 3, 4).
add_executable(dropforge_blade_tests BladeTests.cpp)
target_link_libraries(dropforge_blade_tests PRIVATE dropforge_core)
add_test(NAME blade_voice COMMAND dropforge_blade_tests)
set_tests_properties(blade_voice PROPERTIES TIMEOUT 300)

# Performance controls: Bend Range and Glide (SPEC section 3).
add_executable(dropforge_performance_tests PerformanceTests.cpp)
target_link_libraries(dropforge_performance_tests PRIVATE dropforge_core)
add_test(NAME performance COMMAND dropforge_performance_tests)

# The FX page's chain: sends, Delay, tempo, tails (SPEC section 8).
add_executable(dropforge_fx_tests FxTests.cpp)
target_link_libraries(dropforge_fx_tests PRIVATE dropforge_core)
add_test(NAME fx_chain COMMAND dropforge_fx_tests)

# Gates for every Anvil x Furnace mode pair, clean and destroyed (SPEC section 12).
add_executable(dropforge_combination_tests CombinationTests.cpp)
target_include_directories(dropforge_combination_tests PRIVATE "${CMAKE_SOURCE_DIR}/shared/dsp-core/tests")
target_link_libraries(dropforge_combination_tests PRIVATE dropforge_core)
add_test(NAME combination_gates COMMAND dropforge_combination_tests)
set_tests_properties(combination_gates PROPERTIES TIMEOUT 300)

add_executable(dropforge_master_tests MasterTests.cpp)
target_link_libraries(dropforge_master_tests PRIVATE dropforge_core)
add_test(NAME master_chain COMMAND dropforge_master_tests)

add_executable(dropforge_layer_mode_tests LayerModeTests.cpp)
target_link_libraries(dropforge_layer_mode_tests PRIVATE dropforge_core)
add_test(NAME layer_modes COMMAND dropforge_layer_mode_tests)

add_executable(dropforge_bass_audio_tests BassAudioTests.cpp)
target_link_libraries(dropforge_bass_audio_tests PRIVATE dropforge_core)
add_test(NAME bass_audio COMMAND dropforge_bass_audio_tests)

add_executable(dropforge_rhythm_tests RhythmTests.cpp)
target_link_libraries(dropforge_rhythm_tests PRIVATE dropforge_core)
add_test(NAME rhythm_voices COMMAND dropforge_rhythm_tests)

add_executable(dropforge_rhythm_bus_tests RhythmBusTests.cpp)
target_link_libraries(dropforge_rhythm_bus_tests PRIVATE dropforge_core)
add_test(NAME rhythm_bus COMMAND dropforge_rhythm_bus_tests)

add_executable(dropforge_rhythm_mix_tests RhythmMixTests.cpp)
target_include_directories(dropforge_rhythm_mix_tests PRIVATE "${CMAKE_SOURCE_DIR}/shared/dsp-core/tests")
target_compile_definitions(dropforge_rhythm_mix_tests PRIVATE
    DROPFORGE_COMMIT="${DROPFORGE_CONFIGURED_COMMIT}" DROPFORGE_DIRTY=${DROPFORGE_CONFIGURED_DIRTY})
target_link_libraries(dropforge_rhythm_mix_tests PRIVATE dropforge_core dropforge_juce_headless)
add_test(NAME rhythm_mix COMMAND dropforge_rhythm_mix_tests "${CMAKE_SOURCE_DIR}/tests/fixtures/sequences.json"
    "${CMAKE_BINARY_DIR}/rhythm-mix.json")
set_tests_properties(rhythm_mix PROPERTIES TIMEOUT 300)

add_executable(dropforge_routing_tests RoutingTests.cpp)
target_link_libraries(dropforge_routing_tests PRIVATE dropforge_core)
add_test(NAME routing_matrix COMMAND dropforge_routing_tests)

# Exercise the same raw MIDI decoder and sample-offset bridge as the plugin,
# without pulling any editor/GUI code into the test executable.
add_executable(dropforge_oscillator_tests OscillatorTests.cpp)
target_include_directories(dropforge_oscillator_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(dropforge_oscillator_tests PRIVATE dropforge_dsp dropforge_juce_headless)
add_test(NAME oscillator_midi COMMAND dropforge_oscillator_tests)

# Link the actual processor/editor objects, reusing the compiled JUCE code.
add_executable(dropforge_processor_tests ProcessorTests.cpp)
target_include_directories(dropforge_processor_tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${juce_SOURCE_DIR}/modules")
target_link_libraries(dropforge_processor_tests PRIVATE Dropforge dropforge_dsp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Reuse the existing system X11 dependency only in the GUI test fixture.
    find_package(X11 REQUIRED)
    target_sources(dropforge_processor_tests PRIVATE X11TestDisplay.cpp X11TestDisplay.h)
    target_link_libraries(dropforge_processor_tests PRIVATE X11::X11)
endif()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ui-snapshots")
add_test(NAME processor_state_editor COMMAND dropforge_processor_tests "${CMAKE_BINARY_DIR}/ui-snapshots/editor.png")
set_tests_properties(processor_state_editor PROPERTIES TIMEOUT 60)

# The shipped Standalone checks itself offscreen: no window, no audio device.
add_test(NAME standalone_self_test COMMAND Dropforge_Standalone --self-test)
set_tests_properties(standalone_self_test PROPERTIES TIMEOUT 120)

# End-to-end file/CLI checks use only JUCE's non-GUI audio/core modules.
add_executable(dropforge_render_tests RenderTests.cpp)
target_link_libraries(dropforge_render_tests PRIVATE dropforge_core dropforge_juce_headless)
add_test(NAME renderer_offline COMMAND dropforge_render_tests "$<TARGET_FILE:render_tool>" "${CMAKE_BINARY_DIR}")
add_test(NAME standard_sequences COMMAND dropforge_render_tests "$<TARGET_FILE:render_tool>"
    "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}/tests/fixtures/sequences.json")
add_test(NAME bass_sequences COMMAND dropforge_render_tests "$<TARGET_FILE:render_tool>"
    "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}/tests/fixtures/sequences.json" "--bass")
