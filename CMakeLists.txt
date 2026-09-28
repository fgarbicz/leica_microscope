cmake_minimum_required(VERSION 3.24)
project(DMImaging VERSION 1.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Objective-C++ is needed for the macOS AVFoundation camera backend.
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /wd4127 /wd4251)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # whole-program optimisation for release builds
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Oi /GL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    # the imaging kernels are the whole point of this application: optimise them
    # in every configuration, and keep debug info in release builds for crash reports
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O1")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O3")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -g1")
endif()

option(BUILD_APP "Build the Qt application" ON)
enable_testing()

# ------------------------------------------------------------------ libusb
# The native Leica camera driver speaks USB directly. Windows uses WinUSB from
# the SDK; macOS and Linux use libusb-1.0.
if(NOT WIN32)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
    endif()
    if(NOT LIBUSB_FOUND)
        find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0)
        find_library(LIBUSB_LIBRARY NAMES usb-1.0 usb)
        if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
            message(FATAL_ERROR
                "libusb-1.0 was not found. Install it first:\n"
                "  macOS          brew install libusb\n"
                "  Debian/Ubuntu  sudo apt install libusb-1.0-0-dev\n"
                "  Fedora         sudo dnf install libusb1-devel")
        endif()
        add_library(usb_imported INTERFACE)
        target_include_directories(usb_imported INTERFACE ${LIBUSB_INCLUDE_DIR})
        target_link_libraries(usb_imported INTERFACE ${LIBUSB_LIBRARY})
        set(LIBUSB_TARGET usb_imported)
    else()
        set(LIBUSB_TARGET PkgConfig::LIBUSB)
    endif()
endif()

# ---------------------------------------------------------------- core library
# Hardware access and image processing, no Qt dependency (usable from tools/tests).
set(LMCORE_SOURCES
    src/core/Frame.cpp
    src/imaging/Debayer.cpp
    src/imaging/ColorPipeline.cpp
    src/imaging/ShadingCorrection.cpp
    src/imaging/Analysis.cpp
    src/imaging/Registration.cpp
    src/imaging/FocusStacker.cpp
    src/imaging/MosaicBuilder.cpp
    src/imaging/PixelShift.cpp
    src/imaging/Hdr.cpp
    src/imaging/StainAnalysis.cpp
    src/imaging/NucleusDetection.cpp
    src/camera/Backends.cpp
    src/camera/SimulatedCamera.cpp
    src/camera/leica/Dmc6200Protocol.cpp
    src/camera/leica/Dmc6200Camera.cpp
)
# One USB backend and one UVC camera backend per platform.
if(WIN32)
    list(APPEND LMCORE_SOURCES
        src/camera/usb/UsbDeviceWin.cpp
        src/camera/usb/UsbCPower.cpp      # UCSI USB-C power cycling: Windows only
        src/camera/MFCamera.cpp)
elseif(APPLE)
    list(APPEND LMCORE_SOURCES
        src/camera/usb/UsbDeviceLibusb.cpp
        src/camera/AvfCamera.mm)
else()
    list(APPEND LMCORE_SOURCES
        src/camera/usb/UsbDeviceLibusb.cpp
        src/camera/V4l2Camera.cpp)
endif()

add_library(lmcore STATIC ${LMCORE_SOURCES})
target_include_directories(lmcore PUBLIC src)

if(WIN32)
    target_link_libraries(lmcore PUBLIC winusb cfgmgr32 setupapi mfplat mf mfreadwrite mfuuid strmiids ole32)
elseif(APPLE)
    target_link_libraries(lmcore PUBLIC ${LIBUSB_TARGET}
        "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
        "-framework CoreFoundation" "-framework Foundation")
    set_source_files_properties(src/camera/AvfCamera.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(lmcore PUBLIC ${LIBUSB_TARGET} Threads::Threads)
endif()

# ---------------------------------------------------------------- application
if(BUILD_APP)
set(QT_COMPONENTS Core Gui Widgets Concurrent Svg PrintSupport)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND QT_COMPONENTS DBus)   # "show in file manager" via org.freedesktop.FileManager1
endif()
find_package(Qt6 6.4 REQUIRED COMPONENTS ${QT_COMPONENTS})

set(APP_SOURCES
    src/app/main.cpp
    src/app/AcquisitionEngine.cpp
    src/app/AppSettings.cpp
    src/app/Calibration.cpp
    src/app/VideoRecorder.cpp
    src/io/AviWriter.cpp
    src/io/ImageIO.cpp
    src/io/CalibrationRepair.cpp
    src/io/LifFile.cpp
    src/io/Metadata.cpp
    src/ui/MainWindow.cpp
    src/ui/ImageView.cpp
    src/ui/Overlays.cpp
    src/ui/HistogramWidget.cpp
    src/ui/CollapsibleSection.cpp
    src/ui/SliderSpin.cpp
    src/ui/CameraPanel.cpp
    src/ui/ColorPanel.cpp
    src/ui/CapturePanel.cpp
    src/ui/MicroscopePanel.cpp
    src/ui/ToolsPanel.cpp
    src/ui/GalleryWidget.cpp
    src/ui/BrowsePage.cpp
    src/ui/ProcessPage.cpp
    src/ui/Annotations.cpp
    src/ui/Icons.cpp
    src/ui/CalibrationRepairDialog.cpp
    src/ui/LifDialog.cpp
    src/ui/PanelGroup.cpp
    src/ui/PlatformUi.cpp
    src/ui/Theme.cpp
    src/ui/WheelGuard.cpp
    src/ui/SettingsDialog.cpp
    src/ui/CaptureDialog.cpp
    src/ui/CompareWindow.cpp
    src/ui/BatchExportDialog.cpp
    src/ui/BatchIhcDialog.cpp
    resources/resources.qrc
)
if(WIN32)
    list(APPEND APP_SOURCES resources/app.rc)   # icon + version information
endif()

add_executable(DMImaging ${APP_SOURCES})
if(WIN32)
    set_target_properties(DMImaging PROPERTIES WIN32_EXECUTABLE ON)
endif()
target_link_libraries(DMImaging PRIVATE lmcore Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Svg Qt6::PrintSupport)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(DMImaging PRIVATE Qt6::DBus)
endif()

# ---- macOS application bundle
if(APPLE)
    # An .icns built from the shared PNG, so Dock and Finder show the app icon.
    set(MACOS_ICON ${CMAKE_BINARY_DIR}/DMImaging.icns)
    find_program(SIPS_EXECUTABLE sips)
    find_program(ICONUTIL_EXECUTABLE iconutil)
    if(SIPS_EXECUTABLE AND ICONUTIL_EXECUTABLE)
        set(ICONSET ${CMAKE_BINARY_DIR}/DMImaging.iconset)
        add_custom_command(OUTPUT ${MACOS_ICON}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${ICONSET}
            COMMAND ${SIPS_EXECUTABLE} -z 16 16     ${CMAKE_SOURCE_DIR}/resources/icons/app.png --out ${ICONSET}/icon_16x16.png
            COMMAND ${SIPS_EXECUTABLE} -z 32 32     ${CMAKE_SOURCE_DIR}/resources/icons/app.png --out ${ICONSET}/icon_16x16@2x.png
            COMMAND ${SIPS_EXECUTABLE} -z 32 32     ${CMAKE_SOURCE_DIR}/resources/icons/app.png --out ${ICONSET}/icon_32x32.png
            COMMAND ${SIPS_EXECUTABLE} -z 64 64     ${CMAKE_SOURCE_DIR}/resources/icons/app.png --out ${ICONSET}/icon_32x32@2x.png
            COMMAND ${SIPS_EXECUTABLE} -z 128 128   ${CMAKE_SOURCE_DIR}/resources/icons/app.png --out ${ICONSET}/icon_128x128.png
            COMMAND ${SIPS_EXECUTABLE} -z 256 256   ${CMAKE_SOURCE_DIR}/resources/icons/app.png --out ${ICONSET}/icon_128x128@2x.png
            COMMAND ${SIPS_EXECUTABLE} -z 256 256   ${CMAKE_SOURCE_DIR}/resources/icons/app.png --out ${ICONSET}/icon_256x256.png
            COMMAND ${SIPS_EXECUTABLE} -z 512 512   ${CMAKE_SOURCE_DIR}/resources/icons/app.png --out ${ICONSET}/icon_256x256@2x.png
            COMMAND ${SIPS_EXECUTABLE} -z 512 512   ${CMAKE_SOURCE_DIR}/resources/icons/app.png --out ${ICONSET}/icon_512x512.png
            COMMAND ${ICONUTIL_EXECUTABLE} -c icns ${ICONSET} -o ${MACOS_ICON}
            DEPENDS ${CMAKE_SOURCE_DIR}/resources/icons/app.png
            COMMENT "Building DMImaging.icns")
        target_sources(DMImaging PRIVATE ${MACOS_ICON})
        set_source_files_properties(${MACOS_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set(MACOSX_BUNDLE_ICON_FILE DMImaging.icns)
    endif()
    set(MACOSX_BUNDLE_BUNDLE_NAME "DM Imaging")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.dmimaging.app")
    set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    set_target_properties(DMImaging PROPERTIES
        MACOSX_BUNDLE ON
        OUTPUT_NAME "DM Imaging"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/resources/macos/Info.plist.in)
    # the user guide ships inside the bundle (there is no driver to install on macOS),
    # with the GPL of DM Imaging and the licenses of Qt and libusb bundled with it
    set(DMI_BUNDLE_DOCS ${CMAKE_SOURCE_DIR}/docs/USER_GUIDE.md ${CMAKE_SOURCE_DIR}/LICENSE
                        ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md)
    target_sources(DMImaging PRIVATE ${DMI_BUNDLE_DOCS})
    set_source_files_properties(${DMI_BUNDLE_DOCS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    file(GLOB DMI_BUNDLE_LICENSES ${CMAKE_SOURCE_DIR}/licenses/*.txt)
    target_sources(DMImaging PRIVATE ${DMI_BUNDLE_LICENSES})
    set_source_files_properties(${DMI_BUNDLE_LICENSES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses)
endif()

# commit of this build (About box, image metadata), refreshed on every configure
find_package(Git QUIET)
set(DMI_GIT_HASH "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE DMI_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
target_compile_definitions(DMImaging PRIVATE
    DMI_GIT_HASH="${DMI_GIT_HASH}"
    DMI_VERSION="${PROJECT_VERSION}"
    DMI_VER_MAJOR=${PROJECT_VERSION_MAJOR} DMI_VER_MINOR=${PROJECT_VERSION_MINOR} DMI_VER_PATCH=${PROJECT_VERSION_PATCH})

# engine integration test (simulated camera)
add_executable(enginetest tests/enginetest.cpp src/app/AcquisitionEngine.cpp)
target_include_directories(enginetest PRIVATE src)
target_link_libraries(enginetest PRIVATE lmcore Qt6::Core Qt6::Gui Qt6::Concurrent)
add_test(NAME enginetest COMMAND enginetest)

# user interface behaviour (the wheel guard, the icon set, image view overlays)
add_executable(uitest tests/uitest.cpp src/ui/WheelGuard.cpp src/ui/Theme.cpp src/ui/Icons.cpp
               src/ui/ImageView.cpp src/ui/Annotations.cpp src/ui/Overlays.cpp src/app/AppSettings.cpp
               src/io/ImageIO.cpp src/io/LifFile.cpp src/io/Metadata.cpp src/ui/CompareWindow.cpp
               src/ui/GalleryWidget.cpp src/ui/PlatformUi.cpp)
target_include_directories(uitest PRIVATE src)
target_link_libraries(uitest PRIVATE lmcore Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(uitest PRIVATE Qt6::DBus)  # PlatformUi's "show in file manager"
endif()
add_test(NAME uitest COMMAND uitest)

add_executable(iotest tests/iotest.cpp src/io/ImageIO.cpp src/io/CalibrationRepair.cpp src/io/LifFile.cpp
               src/io/Metadata.cpp)
target_include_directories(iotest PRIVATE src)
target_link_libraries(iotest PRIVATE lmcore Qt6::Core Qt6::Gui)
add_test(NAME iotest COMMAND iotest)

# command-line IHC analysis
add_executable(ihctool tools/ihctool/ihctool.cpp src/io/ImageIO.cpp src/io/LifFile.cpp src/io/Metadata.cpp)
target_include_directories(ihctool PRIVATE src)
target_link_libraries(ihctool PRIVATE lmcore Qt6::Core Qt6::Gui)
endif()

# ---------------------------------------------------------------- tools & tests
add_executable(dmctest tools/dmctest/dmctest.cpp)
target_link_libraries(dmctest PRIVATE lmcore)

add_executable(lmtests tests/tests.cpp)
target_link_libraries(lmtests PRIVATE lmcore)
add_test(NAME lmtests COMMAND lmtests)

# ---------------------------------------------------------------- deployment
if(BUILD_APP)
if(WIN32)
    install(TARGETS DMImaging RUNTIME DESTINATION .)
    install(DIRECTORY driver/ DESTINATION driver FILES_MATCHING PATTERN "*.inf" PATTERN "*.ps1")
elseif(APPLE)
    install(TARGETS DMImaging BUNDLE DESTINATION .)
else()
    # Linux: FHS layout plus the desktop entry, icon and udev rule
    include(GNUInstallDirs)
    install(TARGETS DMImaging RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES resources/linux/dmimaging.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES resources/icons/app.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
            RENAME dmimaging.png)
    install(FILES driver/99-leica-dmc6200.rules DESTINATION ${CMAKE_INSTALL_DATADIR}/dmimaging/driver)
    install(PROGRAMS driver/install_udev_rule.sh DESTINATION ${CMAKE_INSTALL_DATADIR}/dmimaging/driver)
    install(FILES docs/USER_GUIDE.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(DIRECTORY licenses/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses FILES_MATCHING PATTERN "*.txt")
endif()
endif()
