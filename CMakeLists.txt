cmake_minimum_required(VERSION 3.16)
project(RxTx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 실행 파일(.exe)은 bin 폴더에
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# 정적/공유 라이브러리(.lib, .dll, .a 등)는 lib 폴더에
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

# include 경로 추가
include_directories(${CMAKE_SOURCE_DIR}/inc)

# 하위 프로젝트 추가
add_subdirectory(src/libRxTx)
add_subdirectory(src/appRxTx)
add_subdirectory(src/DroneRacingServer)
add_subdirectory(src/DroneRacingClient)
add_subdirectory(src/DroneRacingClient_Recording)
add_subdirectory(src/DroneRacingClient_Replaying)
add_subdirectory(src/DroneRacingClient_SteerPoint)
