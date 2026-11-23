cmake_minimum_required(VERSION 3.24)
project(YiyiWinUIOcrWorker LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build the native OCR worker with MSVC x64.")
endif()

add_executable(yiyi_ocr_worker OcrWorker.cpp ../NativeImage.cpp)
target_include_directories(yiyi_ocr_worker PRIVATE ..)
target_compile_features(yiyi_ocr_worker PRIVATE cxx_std_20)
target_compile_definitions(yiyi_ocr_worker PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(yiyi_ocr_worker PRIVATE /utf-8 /W4)
target_link_libraries(yiyi_ocr_worker PRIVATE windowsapp)
set_property(TARGET yiyi_ocr_worker PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
