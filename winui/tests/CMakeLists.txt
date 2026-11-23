cmake_minimum_required(VERSION 3.24)
project(YiyiWinUITests LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    message(FATAL_ERROR "Use a Visual Studio generator with the Windows SDK.")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(sdk_cppwinrt "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
if(NOT EXISTS "${sdk_cppwinrt}/winrt/Windows.Foundation.h")
    message(FATAL_ERROR "C++/WinRT headers are missing from the Windows SDK.")
endif()

add_executable(text-client-smoke text_client_smoke.cpp ../TextClient.cpp)
add_executable(storage-smoke storage_smoke.cpp ../WorkspaceSettings.cpp ../HistoryStore.cpp)
add_executable(dictionary-smoke dictionary_smoke.cpp ../Dictionary.cpp)
add_executable(native-image-smoke native_image_smoke.cpp ../NativeImage.cpp)
add_executable(document-smoke document_smoke.cpp ../DocumentTask.cpp ../DocumentFiles.cpp ../TextClient.cpp)
add_executable(office-smoke office_smoke.cpp ../OfficeDocumentNative.cpp ../DocumentFiles.cpp)
add_executable(office-task-smoke office_task_smoke.cpp ../OfficeTask.cpp ../OfficeDocumentNative.cpp
    ../DocumentTask.cpp ../DocumentFiles.cpp ../TextClient.cpp)
add_executable(office-preview-renderer office_preview_renderer.cpp)
set_target_properties(office-preview-renderer PROPERTIES OUTPUT_NAME soffice)
target_compile_features(office-preview-renderer PRIVATE cxx_std_20)
set(miniz_dir ../../third_party/miniz)
add_library(office-miniz STATIC
    ${miniz_dir}/miniz.c ${miniz_dir}/miniz_tdef.c ${miniz_dir}/miniz_tinfl.c ${miniz_dir}/miniz_zip.c)
target_include_directories(office-miniz PUBLIC ${miniz_dir})
target_compile_definitions(office-miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
foreach(test_target IN ITEMS text-client-smoke storage-smoke dictionary-smoke native-image-smoke document-smoke office-smoke office-task-smoke)
    target_include_directories(${test_target} PRIVATE .. "${sdk_cppwinrt}")
    target_compile_features(${test_target} PRIVATE cxx_std_20)
    target_compile_options(${test_target} PRIVATE /utf-8 /W4)
    target_link_libraries(${test_target} PRIVATE windowsapp runtimeobject)
endforeach()
target_link_libraries(storage-smoke PRIVATE crypt32 winsqlite3)
target_link_libraries(document-smoke PRIVATE ole32)
target_link_libraries(office-smoke PRIVATE office-miniz ole32 bcrypt dwrite)
target_link_libraries(office-task-smoke PRIVATE office-miniz ole32 bcrypt dwrite)

enable_testing()
add_test(NAME text-client-smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/text_client_cases.py" "$<TARGET_FILE:text-client-smoke>")
add_test(NAME storage-smoke COMMAND storage-smoke "${CMAKE_CURRENT_SOURCE_DIR}/legacy-settings.ini")
add_test(NAME dictionary-smoke COMMAND dictionary-smoke
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/dictionary/hello-malformed-note.txt")
add_test(NAME native-image-smoke COMMAND native-image-smoke)
add_test(NAME document-smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/document_cases.py" "$<TARGET_FILE:document-smoke>")
add_test(NAME office-smoke COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/office_cases.py"
    "$<TARGET_FILE:office-smoke>" "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/office")
add_test(NAME office-task-smoke COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/office_task_cases.py"
    "$<TARGET_FILE:office-task-smoke>" "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/office" "$<TARGET_FILE:office-preview-renderer>")
set_tests_properties(text-client-smoke storage-smoke dictionary-smoke native-image-smoke document-smoke office-smoke office-task-smoke PROPERTIES TIMEOUT 40)
