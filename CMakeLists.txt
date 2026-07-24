# Visual Studio "폴더 열기(Open Folder)" / CMake 빌드 지원.
# 외부 패키지 매니저 불필요 - third_party 헤더 온리 라이브러리만 사용.
cmake_minimum_required(VERSION 3.15)
project(monitoring_backend CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server main.cpp)

if (MSVC)
    # 한글 문자열 리터럴을 UTF-8로 처리 (필수). httplib.h가 ws2_32는 자동 링크.
    target_compile_options(server PRIVATE /utf-8)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(server PRIVATE Threads::Threads)
endif()

# 실행 파일 옆에 Index.html을 복사해 두어, 빌드 폴더에서 실행해도 프론트엔드가 서빙되도록 함.
add_custom_command(TARGET server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Index.html" "$<TARGET_FILE_DIR:server>/Index.html")
