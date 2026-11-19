cmake_minimum_required(VERSION 3.24)
project(Yiyi VERSION 0.11.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Quick QuickControls2 Network Sql Svg Xml Concurrent)
qt_standard_project_setup(REQUIRES 6.8)

add_library(yiyi_miniz STATIC third_party/miniz/miniz.c third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c third_party/miniz/miniz_zip.c)
target_include_directories(yiyi_miniz PUBLIC third_party/miniz)
target_compile_definitions(yiyi_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

add_library(yiyi_office STATIC src/officedocument.cpp src/officedocument.h)
target_include_directories(yiyi_office PUBLIC src)
target_link_libraries(yiyi_office PUBLIC Qt6::Core Qt6::Gui Qt6::Xml PRIVATE yiyi_miniz)

add_library(yiyi_pdf_layout STATIC src/pdflayout.cpp src/pdflayout.h)
target_include_directories(yiyi_pdf_layout PUBLIC src)
target_link_libraries(yiyi_pdf_layout PUBLIC Qt6::Core Qt6::Gui)

add_library(yiyi_pdf_tasks STATIC src/pdftaskstore.cpp src/pdftaskstore.h src/pdfcache.cpp src/pdfcache.h)
target_include_directories(yiyi_pdf_tasks PUBLIC src)
target_link_libraries(yiyi_pdf_tasks PUBLIC Qt6::Core Qt6::Sql yiyi_pdf_layout)

add_library(yiyi_core STATIC
    src/aiclient.cpp src/aiclient.h
    src/appcontroller.cpp src/appcontroller.h
    src/storage.cpp src/storage.h
    src/secretstore.cpp src/secretstore.h
    src/documentcontroller.cpp src/documentcontroller.h
    src/fileimportcontroller.cpp src/fileimportcontroller.h
    src/officecontroller.cpp src/officecontroller.h
    src/visioncontroller.cpp src/visioncontroller.h
    src/pdfcontroller.cpp src/pdfcontroller.h
    src/pdfpaging.cpp
    src/pdfworkflow.cpp
    src/pdfrecognition.cpp src/visionprompt.h
    src/pdfstructure.cpp
)
target_link_libraries(yiyi_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::Concurrent yiyi_pdf_layout yiyi_pdf_tasks yiyi_office)
target_include_directories(yiyi_core PUBLIC src)
if(WIN32)
    target_link_libraries(yiyi_core PRIVATE crypt32)
endif()

add_library(yiyi_desktop STATIC
    src/desktopintegration.cpp src/desktopintegration.h
    src/selectioncapture.cpp src/selectioncapture.h
    src/singleinstance.cpp src/singleinstance.h
    src/ocrservice.cpp src/ocrservice.h
    src/screenshotcontroller.cpp src/screenshotcontroller.h
    src/ocrpreprocess.cpp src/ocrpreprocess.h
)
target_include_directories(yiyi_desktop PUBLIC src)
target_link_libraries(yiyi_desktop PUBLIC Qt6::Widgets Qt6::Network)
if(WIN32)
    target_link_libraries(yiyi_desktop PRIVATE user32 ole32 oleaut32 uuid)
endif()

qt_add_executable(yiyi_ocr_worker tools/ocr_worker.cpp src/ocrpreprocess.cpp)
target_include_directories(yiyi_ocr_worker PRIVATE src)
target_link_libraries(yiyi_ocr_worker PRIVATE Qt6::Core Qt6::Gui)

set(YIYI_PDF_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/pdf/include" CACHE PATH "PDFium public headers (scripts/setup-pdf.ps1)")
if(NOT EXISTS "${YIYI_PDF_INCLUDE_DIR}/fpdf_edit.h")
    message(FATAL_ERROR "PDF headers missing. Run scripts/setup-pdf.ps1 -OutputDirectory ${CMAKE_CURRENT_BINARY_DIR}/pdf first.")
endif()
if(NOT EXISTS "${YIYI_PDF_INCLUDE_DIR}/qpdf/qpdf-c.h")
    message(FATAL_ERROR "PDF merge headers missing. Run scripts/setup-pdf-merge.ps1 -OutputDirectory ${CMAKE_CURRENT_BINARY_DIR}/pdf first.")
endif()
add_library(yiyi_pdf_engine STATIC src/pdfengine.cpp src/pdfengine.h src/pdfmerge.cpp src/pdfmerge.h src/pdfscanlayout.cpp src/pdfscanlayout.h src/pdfbackground.cpp src/pdfbackground.h)
target_include_directories(yiyi_pdf_engine PUBLIC src PRIVATE "${YIYI_PDF_INCLUDE_DIR}")
target_link_libraries(yiyi_pdf_engine PUBLIC Qt6::Core Qt6::Gui yiyi_pdf_layout yiyi_pdf_tasks)
qt_add_executable(yiyi_pdf_worker tools/pdf_worker.cpp)
target_link_libraries(yiyi_pdf_worker PRIVATE yiyi_pdf_engine)

qt_add_executable(yiyi src/main.cpp resources.qrc)
add_dependencies(yiyi yiyi_ocr_worker yiyi_pdf_worker)
target_link_libraries(yiyi PRIVATE yiyi_core yiyi_desktop Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
set_target_properties(yiyi PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME Yiyi)
if(WIN32)
    target_sources(yiyi PRIVATE assets/windows.rc)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(yiyi_tests tests/tst_core.cpp)
    target_link_libraries(yiyi_tests PRIVATE yiyi_core Qt6::Test)
    add_test(NAME yiyi_core_tests COMMAND yiyi_tests -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/yiyi-core-results.xml,xml")
    qt_add_executable(yiyi_pdf_tests tests/tst_pdf.cpp)
    target_link_libraries(yiyi_pdf_tests PRIVATE yiyi_core yiyi_pdf_engine Qt6::Test)
    add_dependencies(yiyi_pdf_tests yiyi_pdf_worker)
    add_test(NAME yiyi_pdf_tests COMMAND yiyi_pdf_tests -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/yiyi-pdf-results.xml,xml")
    set_tests_properties(yiyi_pdf_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    qt_add_executable(yiyi_pdf_large_tests tests/tst_pdf_large.cpp)
    target_include_directories(yiyi_pdf_large_tests PRIVATE "${YIYI_PDF_INCLUDE_DIR}")
    target_link_libraries(yiyi_pdf_large_tests PRIVATE yiyi_core yiyi_pdf_engine Qt6::Test)
    add_dependencies(yiyi_pdf_large_tests yiyi_pdf_worker)
    add_test(NAME yiyi_pdf_large_tests COMMAND yiyi_pdf_large_tests -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/yiyi-pdf-large-results.xml,xml")
    set_tests_properties(yiyi_pdf_large_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    qt_add_executable(yiyi_pdf_workflow_tests tests/tst_pdf_workflow.cpp)
    target_link_libraries(yiyi_pdf_workflow_tests PRIVATE yiyi_core yiyi_pdf_engine Qt6::Test)
    add_dependencies(yiyi_pdf_workflow_tests yiyi_pdf_worker)
    add_test(NAME yiyi_pdf_workflow_tests COMMAND yiyi_pdf_workflow_tests -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/yiyi-pdf-workflow-results.xml,xml")
    set_tests_properties(yiyi_pdf_workflow_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    qt_add_executable(yiyi_file_import_tests tests/tst_fileimport.cpp)
    target_link_libraries(yiyi_file_import_tests PRIVATE yiyi_core Qt6::Test)
    add_dependencies(yiyi_file_import_tests yiyi_pdf_worker)
    add_test(NAME yiyi_file_import_tests COMMAND yiyi_file_import_tests -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/yiyi-file-import-results.xml,xml")
    set_tests_properties(yiyi_file_import_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    qt_add_executable(yiyi_office_tests tests/tst_office.cpp)
    target_link_libraries(yiyi_office_tests PRIVATE yiyi_core yiyi_miniz Qt6::Test Qt6::Widgets)
    target_compile_definitions(yiyi_office_tests PRIVATE YIYI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME yiyi_office_tests COMMAND yiyi_office_tests -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/yiyi-office-results.xml,xml")
    set_tests_properties(yiyi_office_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    qt_add_executable(yiyi_content_tests tests/tst_content.cpp resources.qrc)
    target_link_libraries(yiyi_content_tests PRIVATE yiyi_core yiyi_desktop Qt6::Test)
    add_test(NAME yiyi_content_tests COMMAND yiyi_content_tests -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/yiyi-content-results.xml,xml")
    set_tests_properties(yiyi_content_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=C:/Windows/Fonts;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    qt_add_executable(yiyi_ui_tests tests/tst_ui.cpp resources.qrc)
    target_compile_definitions(yiyi_ui_tests PRIVATE YIYI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(yiyi_ui_tests yiyi_pdf_worker)
    target_link_libraries(yiyi_ui_tests PRIVATE yiyi_core yiyi_desktop Qt6::Test Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
    add_test(NAME yiyi_ui_tests COMMAND yiyi_ui_tests -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/yiyi-ui-results.xml,xml")
    # build/qt.conf can redirect plugin discovery after desktop deployment.
    # Keep the test-only offscreen platform discoverable in the selected Qt kit.
    set_tests_properties(yiyi_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QPA_FONTDIR=C:/Windows/Fonts;QT_QPA_PLATFORM_PLUGIN_PATH=${Qt6_DIR}/../../../plugins/platforms")
    if(WIN32)
        qt_add_executable(yiyi_desktop_tests tests/tst_desktop.cpp resources.qrc)
        target_link_libraries(yiyi_desktop_tests PRIVATE yiyi_core yiyi_desktop Qt6::Test Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
        add_test(NAME yiyi_desktop_tests COMMAND yiyi_desktop_tests -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/yiyi-desktop-results.xml,xml")
        set_tests_properties(yiyi_desktop_tests PROPERTIES RUN_SERIAL TRUE ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_QUICK_BACKEND=software")
    endif()
endif()

install(TARGETS yiyi yiyi_ocr_worker yiyi_pdf_worker RUNTIME DESTINATION .)
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ocr" DESTINATION . OPTIONAL)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/pdf/pdfium.dll" "${CMAKE_CURRENT_BINARY_DIR}/pdf/YiyiPdfSans-Regular.otf" "${CMAKE_CURRENT_BINARY_DIR}/pdf/YiyiPdfSerif-Regular.otf"
    "${CMAKE_CURRENT_BINARY_DIR}/pdf/checksums.json" "${CMAKE_CURRENT_BINARY_DIR}/pdf/pdf-dependencies.json" DESTINATION pdf)
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pdf/licenses" DESTINATION pdf)
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pdf/qpdf" DESTINATION pdf)

# Manual, opt-in check against a local model; never part of offline CTest runs
# or the portable application. Uses a temporary profile and no saved API keys.
add_executable(yiyi_office_check EXCLUDE_FROM_ALL tools/check_office.cpp)
target_link_libraries(yiyi_office_check PRIVATE yiyi_office)
add_executable(yiyi_model_check EXCLUDE_FROM_ALL tools/check_local_model.cpp)
target_link_libraries(yiyi_model_check PRIVATE yiyi_core)
add_executable(yiyi_vision_check EXCLUDE_FROM_ALL tools/check_vision_ocr.cpp resources.qrc)
target_link_libraries(yiyi_vision_check PRIVATE yiyi_core yiyi_desktop)
add_executable(yiyi_pdf_scan_check EXCLUDE_FROM_ALL tools/check_pdf_scan.cpp)
target_link_libraries(yiyi_pdf_scan_check PRIVATE yiyi_core)
add_dependencies(yiyi_pdf_scan_check yiyi_pdf_worker)
add_executable(yiyi_pdf_book_check EXCLUDE_FROM_ALL tools/check_pdf_book.cpp)
target_link_libraries(yiyi_pdf_book_check PRIVATE yiyi_core)
add_dependencies(yiyi_pdf_book_check yiyi_pdf_worker)
add_executable(yiyi_pdf_large_check EXCLUDE_FROM_ALL tools/check_pdf_large.cpp)
target_link_libraries(yiyi_pdf_large_check PRIVATE yiyi_core)
add_dependencies(yiyi_pdf_large_check yiyi_pdf_worker)
