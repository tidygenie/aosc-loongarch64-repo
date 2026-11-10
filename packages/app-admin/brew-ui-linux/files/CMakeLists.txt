cmake_minimum_required(VERSION 3.20)
project(brew-ui-linux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(brew_backend STATIC src/brew_backend.cpp)
target_include_directories(brew_backend PUBLIC src)
target_link_libraries(brew_backend PUBLIC Qt6::Core)

add_executable(brew-ui-linux
    src/main.cpp
    src/main_window.cpp
    src/main_window.h)
target_link_libraries(brew-ui-linux PRIVATE brew_backend Qt6::Widgets)

include(GNUInstallDirs)
install(TARGETS brew-ui-linux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES resources/brew-ui-linux.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_brew_backend tests/test_brew_backend.cpp)
    target_link_libraries(test_brew_backend PRIVATE brew_backend Qt6::Core)
    add_test(NAME backend_json COMMAND test_brew_backend)
    add_test(NAME gui_smoke COMMAND brew-ui-linux --smoke-test)
    set_tests_properties(gui_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
