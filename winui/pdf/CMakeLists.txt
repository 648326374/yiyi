cmake_minimum_required(VERSION 3.24)
project(YiyiPdfWorker LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    message(FATAL_ERROR "Build the PDF worker with a Visual Studio generator and the Windows SDK.")
endif()

set(YIYI_PDF_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../../build/pdf" CACHE PATH "Prepared PDF runtime")
if(NOT EXISTS "${YIYI_PDF_RUNTIME}/include/fpdfview.h" OR NOT EXISTS "${YIYI_PDF_RUNTIME}/pdfium.dll")
    message(FATAL_ERROR "Run scripts/setup-pdf.ps1 to prepare the PDFium headers and runtime.")
endif()
set(sdk_cppwinrt "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/pdf-dependencies.json" pdf_manifest)
string(JSON qpdf_version GET "${pdf_manifest}" qpdf version)
set(qpdf_asset "qpdf-${qpdf_version}-mingw64")
set(YIYI_QPDF_TOOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../artifacts/pdf-deps/qpdf-${qpdf_version}/${qpdf_asset}/bin"
    CACHE PATH "Directory containing the pinned qpdf command and runtime")
if(NOT EXISTS "${YIYI_QPDF_TOOL_DIR}/qpdf30.dll" OR NOT EXISTS "${YIYI_PDF_RUNTIME}/include/qpdf/qpdf-c.h")
    message(FATAL_ERROR "Run scripts/setup-pdf-merge.ps1 to prepare qpdf and its headers.")
endif()

add_library(yiyi_pdf_task_store STATIC pdftaskstore.cpp)
target_compile_features(yiyi_pdf_task_store PUBLIC cxx_std_20)
target_compile_options(yiyi_pdf_task_store PRIVATE /utf-8 /W4)
target_compile_definitions(yiyi_pdf_task_store PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(yiyi_pdf_task_store SYSTEM PUBLIC "${sdk_cppwinrt}")
target_link_libraries(yiyi_pdf_task_store PUBLIC winsqlite3 windowsapp runtimeobject)
set_property(TARGET yiyi_pdf_task_store PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(yiyi_pdf_worker pdfworker.cpp pdfdocument.cpp pdfworkerfiles.cpp pdfanalysis.cpp
    pdfscanlayout.cpp pdfmerge.cpp pdfoverlay.cpp ../PdfTextChecks.cpp)
target_compile_features(yiyi_pdf_worker PRIVATE cxx_std_20)
target_compile_options(yiyi_pdf_worker PRIVATE /utf-8 /W4)
target_compile_definitions(yiyi_pdf_worker PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(yiyi_pdf_worker SYSTEM PRIVATE "${YIYI_PDF_RUNTIME}/include" "${sdk_cppwinrt}")
target_link_libraries(yiyi_pdf_worker PRIVATE yiyi_pdf_task_store bcrypt windowscodecs ole32 dwrite normaliz)
set_property(TARGET yiyi_pdf_worker PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_custom_command(TARGET yiyi_pdf_worker POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:yiyi_pdf_worker>/pdf/qpdf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${YIYI_PDF_RUNTIME}/pdfium.dll" "${YIYI_PDF_RUNTIME}/YiyiPdfSans-Regular.otf"
        "${YIYI_PDF_RUNTIME}/YiyiPdfSerif-Regular.otf" "$<TARGET_FILE_DIR:yiyi_pdf_worker>/pdf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${YIYI_QPDF_TOOL_DIR}/qpdf30.dll" "$<TARGET_FILE_DIR:yiyi_pdf_worker>/pdf/qpdf/qpdf.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${YIYI_QPDF_TOOL_DIR}/libgcc_s_seh-1.dll" "${YIYI_QPDF_TOOL_DIR}/libstdc++-6.dll"
        "${YIYI_QPDF_TOOL_DIR}/libwinpthread-1.dll" "$<TARGET_FILE_DIR:yiyi_pdf_worker>/pdf/qpdf")

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(pdf-task-smoke ../tests/pdf_task_smoke.cpp)
    target_compile_options(pdf-task-smoke PRIVATE /utf-8 /W4)
    target_link_libraries(pdf-task-smoke PRIVATE yiyi_pdf_task_store)
    set_property(TARGET pdf-task-smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME pdf-worker-smoke COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/pdf_worker_smoke.py"
        "$<TARGET_FILE:yiyi_pdf_worker>" "${CMAKE_CURRENT_BINARY_DIR}" "$<TARGET_FILE:pdf-task-smoke>")
    set_tests_properties(pdf-worker-smoke PROPERTIES TIMEOUT 50)

    if(EXISTS "${YIYI_QPDF_TOOL_DIR}/qpdf.exe")
        add_executable(pdf-workflow-smoke ../tests/pdf_workflow_smoke.cpp ../PdfTask.cpp ../DocumentTask.cpp
            ../DocumentFiles.cpp ../TextClient.cpp ../PdfTextChecks.cpp ../PdfWorkerClient.cpp pdfworkerfiles.cpp)
        target_compile_features(pdf-workflow-smoke PRIVATE cxx_std_20)
        target_compile_options(pdf-workflow-smoke PRIVATE /utf-8 /W4)
        target_compile_definitions(pdf-workflow-smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_include_directories(pdf-workflow-smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
        target_link_libraries(pdf-workflow-smoke PRIVATE yiyi_pdf_task_store normaliz ole32)
        set_property(TARGET pdf-workflow-smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        add_dependencies(pdf-workflow-smoke yiyi_pdf_worker)
        add_test(NAME pdf-workflow-smoke COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/../tests/pdf_workflow_smoke.py" "$<TARGET_FILE:pdf-workflow-smoke>"
            "${CMAKE_CURRENT_BINARY_DIR}" "${YIYI_QPDF_TOOL_DIR}/qpdf.exe")
        set_tests_properties(pdf-workflow-smoke PROPERTIES TIMEOUT 50)
    endif()

    add_executable(pdf-overlay-smoke ../tests/pdf_overlay_smoke.cpp pdfoverlay.cpp)
    target_compile_features(pdf-overlay-smoke PRIVATE cxx_std_20)
    target_compile_options(pdf-overlay-smoke PRIVATE /utf-8 /W4)
    target_compile_definitions(pdf-overlay-smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(pdf-overlay-smoke SYSTEM PRIVATE "${YIYI_PDF_RUNTIME}/include")
    target_link_libraries(pdf-overlay-smoke PRIVATE dwrite)
    set_property(TARGET pdf-overlay-smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME pdf-overlay-smoke COMMAND pdf-overlay-smoke "${YIYI_PDF_RUNTIME}" "${CMAKE_CURRENT_BINARY_DIR}/overlay-smoke.pdf")

    add_executable(pdf-analysis-smoke ../tests/pdf_analysis_smoke.cpp pdfanalysis.cpp)
    target_compile_features(pdf-analysis-smoke PRIVATE cxx_std_20)
    target_compile_options(pdf-analysis-smoke PRIVATE /utf-8 /W4)
    target_compile_definitions(pdf-analysis-smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(pdf-analysis-smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(pdf-analysis-smoke SYSTEM PRIVATE "${sdk_cppwinrt}")
    target_link_libraries(pdf-analysis-smoke PRIVATE bcrypt windowsapp runtimeobject)
    set_property(TARGET pdf-analysis-smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME pdf-analysis-smoke COMMAND pdf-analysis-smoke)

    add_executable(pdf-scan-smoke ../tests/pdf_scan_smoke.cpp pdfanalysis.cpp pdfscanlayout.cpp)
    target_compile_features(pdf-scan-smoke PRIVATE cxx_std_20)
    target_compile_options(pdf-scan-smoke PRIVATE /utf-8 /W4)
    target_compile_definitions(pdf-scan-smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(pdf-scan-smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(pdf-scan-smoke SYSTEM PRIVATE "${sdk_cppwinrt}")
    target_link_libraries(pdf-scan-smoke PRIVATE bcrypt windowsapp runtimeobject gdi32)
    set_property(TARGET pdf-scan-smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME pdf-scan-smoke COMMAND pdf-scan-smoke)

    if(EXISTS "${YIYI_QPDF_TOOL_DIR}/qpdf.exe" AND EXISTS "${YIYI_PDF_RUNTIME}/include/qpdf/qpdf-c.h")
        add_executable(pdf-merge-smoke ../tests/pdf_merge_smoke.cpp pdfmerge.cpp)
        target_compile_features(pdf-merge-smoke PRIVATE cxx_std_20)
        target_compile_options(pdf-merge-smoke PRIVATE /utf-8 /W4)
        target_compile_definitions(pdf-merge-smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_include_directories(pdf-merge-smoke SYSTEM PRIVATE "${YIYI_PDF_RUNTIME}/include")
        set_property(TARGET pdf-merge-smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        set(merge_test_runtime "$<TARGET_FILE_DIR:pdf-merge-smoke>/pdf-merge-runtime")
        add_custom_command(TARGET pdf-merge-smoke POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${merge_test_runtime}/qpdf"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${YIYI_QPDF_TOOL_DIR}/qpdf30.dll" "${merge_test_runtime}/qpdf/qpdf.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${YIYI_QPDF_TOOL_DIR}/libgcc_s_seh-1.dll"
                "${YIYI_QPDF_TOOL_DIR}/libstdc++-6.dll"
                "${YIYI_QPDF_TOOL_DIR}/libwinpthread-1.dll" "${merge_test_runtime}/qpdf")
        add_test(NAME pdf-merge-smoke COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tests/pdf_merge_cases.py" "$<TARGET_FILE:pdf-merge-smoke>"
            "${merge_test_runtime}" "${YIYI_QPDF_TOOL_DIR}/qpdf.exe" "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(pdf-merge-smoke PROPERTIES TIMEOUT 50)
    endif()
endif()
