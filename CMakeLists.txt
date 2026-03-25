cmake_minimum_required(VERSION 3.19)
project(SD_LoRA_Manager LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network concurrent)

find_package(OpenSSL REQUIRED)

qt_standard_project_setup()

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

qt_add_executable(SD_LoRA_Manager
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    pathlistdialog.h
    pathlistdialog.cpp
    pathlistdialog.ui
    tagbrowserwidget.h
    tagbrowserwidget.cpp
    tagbrowserwidget.ui
    llmpromptwidget.h
    llmpromptwidget.cpp
    llmpromptwidget.ui
    continueconversationdialog.h
    continueconversationdialog.cpp
    continueconversationdialog.ui
    imageloader.h
    resources.qrc
    app.rc
    tagflowwidget.h
    syncwidget.h syncwidget.cpp syncwidget.ui
    promptparserwidget.h promptparserwidget.cpp promptparserwidget.ui
)

target_link_libraries(SD_LoRA_Manager
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt6::Network
        OpenSSL::SSL
        OpenSSL::Crypto
        Qt6::Concurrent
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
