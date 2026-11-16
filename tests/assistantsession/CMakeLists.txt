# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.20)
project(AssistantSessionTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Network Test)
enable_testing()
add_executable(assistantsessiontest test.cpp
    ../../src/assistant/assistantsession.cpp
    ../../src/assistant/codexclient.cpp
    ../../src/assistant/chatcompletionsclient.cpp
    ../../src/assistant/workbuddybridge.cpp
    ../../src/assistant/workspacepaths.cpp)
target_link_libraries(assistantsessiontest PRIVATE Qt6::Core Qt6::Network Qt6::Test)
target_compile_definitions(assistantsessiontest PRIVATE QT_USE_QSTRINGBUILDER)
add_test(NAME assistantsessiontest COMMAND assistantsessiontest)
