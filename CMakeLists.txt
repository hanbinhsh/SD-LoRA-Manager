cmake_minimum_required(VERSION 3.19)
project(SD_LoRA_Manager LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network concurrent)

qt_standard_project_setup()

set(CMAKE_AUTORCC ON)

qt_add_executable(SD_LoRA_Manager
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    imageloader.h
    resources.qrc
    app.rc
)

target_link_libraries(SD_LoRA_Manager
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt6::Network
)

include(GNUInstallDirs)

install(TARGETS SD_LoRA_Manager
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET SD_LoRA_Manager
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
