cmake_minimum_required(VERSION 3.20)
project(TurboMediaInstalledVoiceConsumer LANGUAGES C CXX)

option(TEST_SIP_CLIENT "Exercise the installed SIP client component" OFF)
if(TEST_SIP_CLIENT)
  find_package(TurboMedia CONFIG REQUIRED COMPONENTS SIPClient)
  add_executable(tm_installed_sip_client ../test_sip_client.c)
  target_compile_features(tm_installed_sip_client PRIVATE c_std_11)
  target_link_libraries(tm_installed_sip_client PRIVATE TurboMedia::SIPClient Salts::TinyTest)
  enable_testing()
  add_test(NAME installed_sip_client COMMAND tm_installed_sip_client)
  return()
endif()

find_package(TurboMedia CONFIG REQUIRED COMPONENTS SIP RTP SDP AudioCodec)
add_executable(tm_installed_voice voice_components.c)
target_compile_features(tm_installed_voice PRIVATE c_std_11)
target_link_libraries(tm_installed_voice PRIVATE
  TurboMedia::SIP TurboMedia::RTP TurboMedia::SDP TurboMedia::AudioCodec)

add_executable(tm_installed_voice_cpp voice_components.cpp)
target_compile_features(tm_installed_voice_cpp PRIVATE cxx_std_17)
set_target_properties(tm_installed_voice_cpp PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(tm_installed_voice_cpp PRIVATE TurboMedia::SIP TurboMedia::AudioCodec)

enable_testing()
add_test(NAME installed_voice_components COMMAND tm_installed_voice)
add_test(NAME installed_voice_components_cpp COMMAND tm_installed_voice_cpp)
