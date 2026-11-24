cmake_minimum_required(VERSION 3.24)
project(YiyiWordPreview LANGUAGES CXX)

add_executable(yiyi_word_preview word_preview.cpp)
target_compile_features(yiyi_word_preview PRIVATE cxx_std_20)
target_compile_options(yiyi_word_preview PRIVATE /utf-8 /W4)
target_link_libraries(yiyi_word_preview PRIVATE ole32 oleaut32 oleacc uuid comsuppw)
