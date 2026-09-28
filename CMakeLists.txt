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
    windows/mainwindow.cpp
    windows/mainwindow.h
    windows/mainwindow.ui
    core/modeltypes.h
    core/modelupdateinfo.h
    utils/downloadcache.h
    utils/downloadcache.cpp
    utils/downloadstatus.h
    utils/downloadstatus.cpp
    core/pathmodels.h
    core/metadatascantypes.h
    utils/modelmetadatacodec.h
    utils/modelmetadatacodec.cpp
    utils/modelscanner.h
    utils/modelscanner.cpp
    utils/modelimagematcher.h
    utils/modelimagematcher.cpp
    utils/civarchiveparser.h
    utils/civarchiveparser.cpp
    utils/previewimagestore.h
    utils/previewimagestore.cpp
    utils/pathutils.h
    utils/pathutils.cpp
    utils/modelfilter.h
    utils/modelfilter.cpp
    utils/gallerymetadata.h
    utils/gallerymetadata.cpp
    utils/metadatainspection.h
    utils/metadatainspection.cpp
    utils/imagepresentation.h
    utils/imagepresentation.cpp
    utils/styleconstants.h
    utils/styleconstants.cpp
    utils/tagutils.h
    utils/tagutils.cpp
    utils/translationcsv.h
    utils/translationcsv.cpp
    utils/tagsearchindex.h
    utils/tagsearchindex.cpp
    widgets/tagsearchproxymodel.h
    widgets/tagsearchproxymodel.cpp
    utils/launchscriptparser.h
    utils/launchscriptparser.cpp
    utils/fileutils.h
    utils/fileutils.cpp
    utils/modellistcache.h
    utils/modellistcache.cpp
    pages/downloadspage.h
    pages/downloadspage.cpp
    pages/downloadspage.ui
    pages/downloadmanager.h
    pages/downloadmanager.cpp
    pages/settingspage.h
    pages/settingspage.cpp
    pages/settingspage.ui
    pages/aboutpage.h
    pages/aboutpage.cpp
    pages/aboutpage.ui
    utils/imagemetadataparser.h
    utils/imagemetadataparser.cpp
    tools/comfyworkflowviewer.h
    tools/comfyworkflowviewer.cpp
    utils/tableviewstylehelper.h
    utils/tableviewstylehelper.cpp
    dialogs/pathlistdialog.h
    dialogs/pathlistdialog.cpp
    dialogs/pathlistdialog.ui
    tools/tagbrowserwidget.h
    tools/tagbrowserwidget.cpp
    tools/tagbrowserwidget.ui
    tools/llmpromptwidget.h
    tools/llmpromptwidget.cpp
    tools/llmpromptwidget.ui
    tools/prompttemplatelibrarywidget.h
    tools/prompttemplatelibrarywidget.cpp
    tools/prompttemplatelibrarywidget.ui
    dialogs/modelnotedialog.h
    dialogs/modelnotedialog.cpp
    dialogs/modelnotedialog.ui
    dialogs/imageviewerdialog.h
    dialogs/imageviewerdialog.cpp
    dialogs/imageviewerdialog.ui
    dialogs/launcherimportdialog.h
    dialogs/launcherimportdialog.cpp
    dialogs/launcherimportdialog.ui
    tools/usageanalysiswidget.h
    tools/usageanalysiswidget.cpp
    tools/usageanalysiswidget.ui
    utils/imageloader.h
    resources.qrc
    app.rc
    widgets/tagflowwidget.h
    widgets/chartwidgets.h
    tools/syncwidget.h
    tools/syncwidget.cpp
    tools/syncwidget.ui
    tools/promptparserwidget.h
    tools/promptparserwidget.cpp
    tools/promptparserwidget.ui
    tools/wd14historymodel.h
    tools/wd14historymodel.cpp
    tools/wd14batchmodel.h
    tools/wd14batchmodel.cpp
    pages/launcherwidget.h
    pages/launcherwidget.cpp
    pages/launcherwidget.ui
    dialogs/themeeditordialog.h
    dialogs/themeeditordialog.cpp
)

target_include_directories(SD_LoRA_Manager
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/windows
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        ${CMAKE_CURRENT_SOURCE_DIR}/pages
        ${CMAKE_CURRENT_SOURCE_DIR}/dialogs
        ${CMAKE_CURRENT_SOURCE_DIR}/utils
        ${CMAKE_CURRENT_SOURCE_DIR}/widgets
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

option(BUILD_MODEL_LIST_CACHE_TESTS "Build model list cache regression tests" OFF)
option(BUILD_CORE_UTIL_TESTS "Build headless core utility regression tests" OFF)
if(BUILD_CORE_UTIL_TESTS)
    enable_testing()
    add_executable(core_utils_test
        tests/core_utils_test.cpp
        utils/civarchiveparser.cpp
        utils/downloadcache.cpp
        utils/downloadstatus.cpp
        utils/fileutils.cpp
        utils/gallerymetadata.cpp
        utils/imagemetadataparser.cpp
        utils/metadatainspection.cpp
        utils/modelfilter.cpp
        utils/modelimagematcher.cpp
        utils/modellistcache.cpp
        utils/modelmetadatacodec.cpp
        utils/modelscanner.cpp
        utils/pathutils.cpp
        utils/previewimagestore.cpp
        utils/tagutils.cpp)
    target_include_directories(core_utils_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(core_utils_test PRIVATE Qt6::Core Qt6::Gui)
    foreach(group metadata matching scanner preview archive paths_filters inspection downloads)
        add_test(NAME core_utils_${group} COMMAND core_utils_test ${group})
    endforeach()
endif()

option(BUILD_TAG_BROWSER_TESTS "Build offscreen tag search and editing tests" OFF)
if(BUILD_TAG_BROWSER_TESTS)
    enable_testing()
    add_executable(tag_browser_test
        tests/tag_browser_test.cpp
        tools/tagbrowserwidget.cpp
        tools/tagbrowserwidget.h
        tools/tagbrowserwidget.ui
        widgets/tagsearchproxymodel.cpp
        widgets/tagsearchproxymodel.h
        utils/tagsearchindex.cpp
        utils/translationcsv.cpp
        utils/tagutils.cpp
        utils/styleconstants.cpp
        utils/tableviewstylehelper.cpp
        resources.qrc)
    target_include_directories(tag_browser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/utils)
    target_link_libraries(tag_browser_test PRIVATE Qt6::Widgets Qt6::Concurrent)
    set_target_properties(tag_browser_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME tag_browser_test COMMAND tag_browser_test)
    set_tests_properties(tag_browser_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

option(BUILD_DOWNLOAD_RESTORE_TESTS "Build offscreen download cache restore tests" OFF)
if(BUILD_DOWNLOAD_RESTORE_TESTS)
    enable_testing()
    add_executable(download_restore_test
        tests/download_restore_test.cpp
        pages/downloadmanager.cpp
        pages/downloadmanager.h
        pages/downloadspage.cpp
        pages/downloadspage.h
        pages/downloadspage.ui
        utils/downloadcache.cpp
        utils/downloadstatus.cpp
        utils/fileutils.cpp
        utils/styleconstants.cpp
        utils/tableviewstylehelper.cpp
        resources.qrc)
    target_include_directories(download_restore_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/utils)
    target_link_libraries(download_restore_test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent)
    # Keep test page caches separate from the application's real config directory.
    set_target_properties(download_restore_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME download_restore_test COMMAND download_restore_test)
    set_tests_properties(download_restore_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
endif()

if(BUILD_MODEL_LIST_CACHE_TESTS)
    enable_testing()
    add_executable(model_list_cache_test tests/model_list_cache_test.cpp utils/modellistcache.cpp)
    target_include_directories(model_list_cache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/utils)
    target_link_libraries(model_list_cache_test PRIVATE Qt6::Core)
    add_test(NAME model_list_cache_test COMMAND model_list_cache_test)
endif()

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

