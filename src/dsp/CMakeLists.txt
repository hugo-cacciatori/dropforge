add_library(dropforge_dsp STATIC Engine.cpp Engine.h BassNotes.h BassVoice.h RhythmOscillators.h OutputGain.h)

# NMake can lose compiler-generated header dependencies after a failed compile.
# Keep a small explicit fallback so changing a voice layout cannot accidentally
# link a stale Engine object against newly compiled processor/test headers.
file(GLOB instrument_dsp_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/dsp-core/include/dsp_core/*.h")
set_property(SOURCE Engine.cpp APPEND PROPERTY OBJECT_DEPENDS ${instrument_dsp_headers})
target_include_directories(dropforge_dsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(dropforge_dsp PUBLIC dsp-core::dsp-core)
set_target_properties(dropforge_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
