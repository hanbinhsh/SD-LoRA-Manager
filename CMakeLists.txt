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
    imagemetadataparser.h
    imagemetadataparser.cpp
    tools/comfyworkflowviewer.h
    tools/comfyworkflowviewer.cpp
    tableviewstylehelper.h
    tableviewstylehelper.cpp
    pathlistdialog.h
    pathlistdialog.cpp
    pathlistdialog.ui
    tools/tagbrowserwidget.h
    tools/tagbrowserwidget.cpp
    tools/tagbrowserwidget.ui
    tools/llmpromptwidget.h
    tools/llmpromptwidget.cpp
    tools/llmpromptwidget.ui
    tools/prompttemplatelibrarywidget.h
    tools/prompttemplatelibrarywidget.cpp
    tools/prompttemplatelibrarywidget.ui
    modelnotedialog.h
    modelnotedialog.cpp
    modelnotedialog.ui
    tools/usageanalysiswidget.h
    tools/usageanalysiswidget.cpp
    tools/usageanalysiswidget.ui
    imageloader.h
    resources.qrc
    app.rc
    tagflowwidget.h
    tools/syncwidget.h
    tools/syncwidget.cpp
    tools/syncwidget.ui
    tools/promptparserwidget.h
    tools/promptparserwidget.cpp
    tools/promptparserwidget.ui
)

target_include_directories(SD_LoRA_Manager
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        ${CMAKE_CURRENT_SOURCE_DIR}/pages
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

add_custom_command(TARGET SD_LoRA_Manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
        "$<TARGET_FILE_DIR:SD_LoRA_Manager>/scripts")

include(GNUInstallDirs)

install(TARGETS SD_LoRA_Manager
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY scripts DESTINATION .)

qt_generate_deploy_app_script(
    TARGET SD_LoRA_Manager
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

