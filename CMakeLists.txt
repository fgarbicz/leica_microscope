cmake_minimum_required(VERSION 3.24)
project(DMImaging VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /MP /wd4127 /wd4251)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # optimise the imaging code even in debug builds
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Oi /GL /arch:AVX2")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
endif()

option(BUILD_APP "Build the Qt application" ON)

# ---------------------------------------------------------------- core library
# Hardware access and image processing, no Qt dependency (usable from tools/tests).
add_library(lmcore STATIC
    src/core/Frame.cpp
    src/imaging/Debayer.cpp
    src/imaging/ColorPipeline.cpp
    src/imaging/ShadingCorrection.cpp
    src/imaging/Analysis.cpp
    src/imaging/Registration.cpp
    src/imaging/FocusStacker.cpp
    src/imaging/MosaicBuilder.cpp
    src/imaging/PixelShift.cpp
    src/camera/Backends.cpp
    src/camera/SimulatedCamera.cpp
    src/camera/MFCamera.cpp
    src/camera/usb/WinUsbDevice.cpp
    src/camera/leica/Dmc6200Protocol.cpp
    src/camera/leica/Dmc6200Camera.cpp
)
target_include_directories(lmcore PUBLIC src)
target_link_libraries(lmcore PUBLIC winusb cfgmgr32 setupapi mfplat mf mfreadwrite mfuuid strmiids ole32)

# ---------------------------------------------------------------- application
if(BUILD_APP)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent Svg PrintSupport)
add_executable(DMImaging WIN32
    src/app/main.cpp
    src/app/AcquisitionEngine.cpp
    src/app/AppSettings.cpp
    src/app/Calibration.cpp
    src/io/ImageIO.cpp
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
    src/ui/Theme.cpp
    src/ui/SettingsDialog.cpp
    src/ui/CaptureDialog.cpp
    resources/resources.qrc
    resources/app.rc
)
target_link_libraries(DMImaging PRIVATE lmcore Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Svg Qt6::PrintSupport)
install(TARGETS DMImaging RUNTIME DESTINATION .)

# engine integration test (simulated camera)
add_executable(enginetest tests/enginetest.cpp src/app/AcquisitionEngine.cpp)
target_include_directories(enginetest PRIVATE src)
target_link_libraries(enginetest PRIVATE lmcore Qt6::Core Qt6::Gui Qt6::Concurrent)
add_test(NAME enginetest COMMAND enginetest)
endif()

# ---------------------------------------------------------------- tools & tests
add_executable(dmctest tools/dmctest/dmctest.cpp)
target_link_libraries(dmctest PRIVATE lmcore)

enable_testing()
add_executable(lmtests tests/tests.cpp)
target_link_libraries(lmtests PRIVATE lmcore)
add_test(NAME lmtests COMMAND lmtests)

# ---------------------------------------------------------------- deployment
install(DIRECTORY driver/ DESTINATION driver FILES_MATCHING PATTERN "*.inf" PATTERN "*.ps1")
