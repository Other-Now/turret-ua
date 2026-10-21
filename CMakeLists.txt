cmake_minimum_required(VERSION 3.20)
project(turret_ua CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TU_GUI "Build the Qt/QML front end" ON)
option(TU_TESTS "Build gtest/gmock tests" ON)
option(TU_TSAN "Build with ThreadSanitizer" OFF)
option(TU_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(TU_FUZZ "Build libFuzzer targets (clang only)" OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)
if(TU_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()
if(TU_FUZZ)
  # Instrument the whole library for coverage, not just the harness.
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -g -O1)
  add_link_options(-fsanitize=address,undefined)
endif()
if(TU_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

add_library(turret_core STATIC
  src/sip/message.cpp
  src/sip/sdp.cpp
  src/sip/digest.cpp
  src/sip/transaction.cpp
  src/ua/user_agent.cpp
  src/core/event_loop.cpp
  src/core/udp_transport.cpp
  src/rtp/g711.cpp
  src/rtp/rtp.cpp
  src/rtp/jitter_buffer.cpp
  src/rtp/media.cpp
  src/util/pcap_writer.cpp
)
target_include_directories(turret_core PUBLIC src)
target_link_libraries(turret_core PUBLIC Threads::Threads)

add_executable(turret-cli apps/cli/main.cpp)
target_link_libraries(turret-cli PRIVATE turret_core)

add_executable(impair-relay tools/impair_relay.cpp)
target_link_libraries(impair-relay PRIVATE turret_core)

add_executable(jb-sweep bench/jb_sweep.cpp)
target_link_libraries(jb-sweep PRIVATE turret_core)

if(TU_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(unit_tests
    tests/test_message.cpp
    tests/test_sdp.cpp
    tests/test_digest.cpp
    tests/test_transaction.cpp
    tests/test_races.cpp
    tests/test_rtp.cpp
    tests/test_threaded.cpp
  )
  target_link_libraries(unit_tests PRIVATE turret_core GTest::gmock GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(unit_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
endif()

if(TU_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)
  set(CMAKE_AUTOMOC ON)
  # Model + QML in one static lib, shared by the app and the Qt tests.
  qt_add_library(turret_gui_lib STATIC apps/gui/call_model.cpp apps/gui/call_model.hpp)
  qt_add_resources(turret_gui_lib "qml" PREFIX "/TurretUa" FILES apps/gui/Main.qml)
  target_include_directories(turret_gui_lib PUBLIC apps/gui)
  target_link_libraries(turret_gui_lib PUBLIC turret_core Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
  qt_add_executable(turret-gui apps/gui/main.cpp)
  target_link_libraries(turret-gui PRIVATE turret_gui_lib)
  if(TU_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(gui_tests tests/test_gui.cpp)
    target_link_libraries(gui_tests PRIVATE turret_gui_lib Qt6::Test)
    add_test(NAME gui_tests COMMAND gui_tests)
    set_tests_properties(gui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
  endif()
endif()

if(TU_FUZZ)
  foreach(t fuzz_sip_parser fuzz_ua)
    add_executable(${t} fuzz/${t}.cpp)
    target_link_options(${t} PRIVATE -fsanitize=fuzzer)
    target_link_libraries(${t} PRIVATE turret_core)
  endforeach()
endif()
