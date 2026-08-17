cmake_minimum_required(VERSION 3.25)

if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum supported macOS version")
endif()

project(NoxShell VERSION 0.2.47 LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(NOXSHELL_MAC_SOURCES
        src/ui/MacTitleBarControls.h
        src/ui/MacTitleBarControls.mm
    )
    set(NOXSHELL_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/NoxShell.icns")
    set_source_files_properties("${NOXSHELL_APP_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    set_source_files_properties(src/ui/MacTitleBarControls.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

if(WIN32)
    set(NOXSHELL_WINDOWS_SOURCES assets/NoxShell.rc)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Sql Svg Widgets)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBSSH2 QUIET IMPORTED_TARGET libssh2)
endif()

if(TARGET PkgConfig::LIBSSH2)
    set(NOXSHELL_LIBSSH2_TARGET PkgConfig::LIBSSH2)
else()
    find_package(libssh2 CONFIG REQUIRED)
    set(NOXSHELL_LIBSSH2_TARGET libssh2::libssh2)
endif()

qt_standard_project_setup()

set(NOXSHELL_CORE_SOURCES
    src/core/AppLogger.cpp
    src/core/AppLogger.h
    src/core/CredentialStore.cpp
    src/core/CredentialStore.h
    src/core/FileTransferTask.h
    src/core/Libssh2Worker.cpp
    src/core/Libssh2Worker.h
    src/core/LinuxMetrics.cpp
    src/core/LinuxMetrics.h
    src/core/MetricHistory.cpp
    src/core/MetricHistory.h
    src/core/MonitoringData.h
    src/core/RemoteFileEntry.h
    src/core/ServerProfile.h
    src/core/ServerRepository.cpp
    src/core/ServerRepository.h
    src/core/SshSession.cpp
    src/core/SshSession.h
)

qt_add_executable(NoxShell
    src/main.cpp
    resources.qrc
    ${NOXSHELL_APP_ICON}
    ${NOXSHELL_WINDOWS_SOURCES}
    ${NOXSHELL_CORE_SOURCES}
    src/ui/AppTheme.cpp
    src/ui/AppTheme.h
    src/ui/FilePanel.cpp
    src/ui/FilePanel.h
    src/ui/HostSidebar.cpp
    src/ui/HostSidebar.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    ${NOXSHELL_MAC_SOURCES}
    src/ui/MetricCard.cpp
    src/ui/MetricCard.h
    src/ui/MonitoringThresholdDialog.cpp
    src/ui/MonitoringThresholdDialog.h
    src/ui/RemoteFileEditor.cpp
    src/ui/RemoteFileEditor.h
    src/ui/ServerDialog.cpp
    src/ui/ServerDialog.h
    src/ui/TerminalPanel.cpp
    src/ui/TerminalPanel.h
    src/ui/TerminalWorkspace.cpp
    src/ui/TerminalWorkspace.h
    src/ui/TerminalView.cpp
    src/ui/TerminalView.h
    src/ui/TrendChart.cpp
    src/ui/TrendChart.h
    src/ui/TransferQueuePanel.cpp
    src/ui/TransferQueuePanel.h
    src/ui/VtTerminalModel.cpp
    src/ui/VtTerminalModel.h
)

target_link_libraries(NoxShell PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::Svg Qt6::Widgets ${NOXSHELL_LIBSSH2_TARGET})
target_compile_definitions(NoxShell PRIVATE NOXSHELL_APP_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(NoxShell PRIVATE Advapi32 Ws2_32)
endif()
if(APPLE)
    target_link_libraries(NoxShell PRIVATE "-framework Cocoa")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(NoxShellSmokeTest
        tests/SmokeTest.cpp
        resources.qrc
        ${NOXSHELL_CORE_SOURCES}
        src/ui/AppTheme.cpp
        src/ui/AppTheme.h
        src/ui/FilePanel.cpp
        src/ui/FilePanel.h
        src/ui/HostSidebar.cpp
        src/ui/HostSidebar.h
        src/ui/MainWindow.cpp
        src/ui/MainWindow.h
        ${NOXSHELL_MAC_SOURCES}
        src/ui/MetricCard.cpp
        src/ui/MetricCard.h
        src/ui/MonitoringThresholdDialog.cpp
        src/ui/MonitoringThresholdDialog.h
        src/ui/RemoteFileEditor.cpp
        src/ui/RemoteFileEditor.h
        src/ui/ServerDialog.cpp
        src/ui/ServerDialog.h
        src/ui/TerminalPanel.cpp
        src/ui/TerminalPanel.h
        src/ui/TerminalWorkspace.cpp
        src/ui/TerminalWorkspace.h
        src/ui/TerminalView.cpp
        src/ui/TerminalView.h
        src/ui/TrendChart.cpp
        src/ui/TrendChart.h
        src/ui/TransferQueuePanel.cpp
        src/ui/TransferQueuePanel.h
        src/ui/VtTerminalModel.cpp
        src/ui/VtTerminalModel.h
    )
    target_link_libraries(NoxShellSmokeTest PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::Widgets Qt6::Test ${NOXSHELL_LIBSSH2_TARGET})
    target_compile_definitions(NoxShellSmokeTest PRIVATE NOXSHELL_APP_VERSION="${PROJECT_VERSION}")
    if(WIN32)
        target_link_libraries(NoxShellSmokeTest PRIVATE Advapi32 Ws2_32)
        set_target_properties(NoxShellSmokeTest PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
    if(APPLE)
        target_link_libraries(NoxShellSmokeTest PRIVATE "-framework Cocoa")
    endif()
    add_test(NAME ui_smoke COMMAND NoxShellSmokeTest -v1)
    if(WIN32)
        set_tests_properties(ui_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    else()
        set_tests_properties(ui_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal")
    endif()
endif()

if(APPLE)
    set_target_properties(NoxShell PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "玄壳"
        MACOSX_BUNDLE_ICON_FILE "NoxShell.icns"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.noxshell.ops"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright © 2026 NoxShell"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/NoxShellInfo.plist.in"
    )
endif()

install(TARGETS NoxShell
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(NOT APPLE)
    qt_generate_deploy_app_script(
        TARGET NoxShell
        OUTPUT_SCRIPT NOXSHELL_DEPLOY_SCRIPT
        NO_TRANSLATIONS
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT "${NOXSHELL_DEPLOY_SCRIPT}")
endif()

set(CPACK_PACKAGE_NAME "NoxShell")
set(CPACK_PACKAGE_VENDOR "NoxShell")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "玄壳 SSH 远程服务器管理桌面应用")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "NoxShell")
set(CPACK_MONOLITHIC_INSTALL ON)
if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
elseif(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_PACKAGE_FILE_NAME "NoxShell-v${PROJECT_VERSION}-Windows-x64")
    set(CPACK_PACKAGE_EXECUTABLES "NoxShell" "玄壳")
    set(CPACK_CREATE_DESKTOP_LINKS "NoxShell")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY "bin")
    set(CPACK_NSIS_DISPLAY_NAME "玄壳 (NoxShell)")
    set(CPACK_NSIS_PACKAGE_NAME "玄壳 (NoxShell)")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\NoxShell.exe")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/NoxShell.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/NoxShell.ico")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "bin\\\\NoxShell.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
    set(CPACK_NSIS_IGNORE_LICENSE_PAGE ON)
    set(CPACK_NSIS_COMPRESSOR "/SOLID lzma")
    set(CPACK_NSIS_BRANDING_TEXT "玄壳 · NoxShell")
    set(CPACK_NSIS_UNINSTALL_NAME "卸载玄壳")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
