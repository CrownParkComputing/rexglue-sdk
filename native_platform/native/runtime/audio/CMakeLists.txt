# xenon-native audio (W4). Two targets:
#   xn_audio          - XMA decoder engine, XACT wave-bank reader, XAudio render
#                       driver pump, SDL3 sink. No kernel dependency.
#   xn_audio_imports  - the __imp__XMA*/__imp__XAudio* kernel imports (object
#                       library; needs the kernel headers). Link it into the
#                       title executable: target_link_libraries(<title> PRIVATE
#                       xn_audio_imports). See docs/AUDIO.md.
find_package(PkgConfig REQUIRED)
pkg_check_modules(XN_FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libavformat libswresample)

add_library(xn_audio STATIC
  xma.cpp
  xma_registers.cpp
  xwb.cpp
  render_driver.cpp
  music_decoder.cpp
  sdl_output.cpp)
target_include_directories(xn_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(xn_audio PUBLIC PkgConfig::XN_FFMPEG SDL3::SDL3 pthread)
target_compile_options(xn_audio PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(TARGET xn_runtime)
  add_library(xn_audio_imports OBJECT imports_audio.cpp)
  target_link_libraries(xn_audio_imports PUBLIC xn_audio xn_runtime)
  target_compile_options(xn_audio_imports PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(XN_BUILD_TESTS)
  add_executable(xma_context_pool_test tests/xma_context_pool_test.cpp)
  target_link_libraries(xma_context_pool_test PRIVATE xn_audio)
  add_test(NAME audio.xma_context_pool COMMAND xma_context_pool_test)
  set(XN_GW_ASSETS "${CMAKE_SOURCE_DIR}/ports/geometrywars/assets"
      CACHE PATH "Geometry Wars wave-bank assets for audio tests")
  set(XN_AUDIO_TEST_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/test-output")
  file(MAKE_DIRECTORY "${XN_AUDIO_TEST_OUTPUT}")
  add_executable(xma_decode_test tests/xma_decode_test.cpp)
  target_link_libraries(xma_decode_test PRIVATE xn_audio)
  add_executable(render_pump_test tests/render_pump_test.cpp)
  target_link_libraries(render_pump_test PRIVATE xn_audio)
  if(EXISTS "${XN_GW_ASSETS}/GW2_Music.xwb")
    add_test(NAME audio.xma_decode.gw2_music
      COMMAND xma_decode_test ${XN_GW_ASSETS} ${XN_AUDIO_TEST_OUTPUT} GW2_Music.xwb 1)
  endif()
  # Boom Boom Rocket ships RIFF/WAVE files tagged 0x0165 (XMA1) rather than a
  # wave bank, and our engine opens libavcodec's xma2 decoder. This keeps that
  # question answered rather than remembered.
  set(XN_BBR_ASSETS "${CMAKE_SOURCE_DIR}/ports/boomboomrocket/assets"
      CACHE PATH "Boom Boom Rocket assets for the XMA1 decode test")
  if(EXISTS "${XN_BBR_ASSETS}/Sounds/UI_select.xma")
    add_test(NAME audio.xma_decode.bbr_xma1
      COMMAND xma_decode_test ${XN_BBR_ASSETS}/Sounds ${XN_AUDIO_TEST_OUTPUT} UI_select.xma 0)
  endif()
  if(EXISTS "${XN_GW_ASSETS}/GW1.xwb")
    add_test(NAME audio.xwb_pcm.gw1
      COMMAND xma_decode_test ${XN_GW_ASSETS} ${XN_AUDIO_TEST_OUTPUT} GW1.xwb 3)
    add_test(NAME audio.render_pump
      COMMAND render_pump_test ${XN_GW_ASSETS} ${XN_AUDIO_TEST_OUTPUT} 3 --silent)
  endif()
endif()
