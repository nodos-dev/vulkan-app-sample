cmake_minimum_required(VERSION 3.24.2)

project(NosVulkanAppSample)

set(CMAKE_CXX_STANDARD 23)

set(EXTERNAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/External)

add_subdirectory(${EXTERNAL_DIR}/nosVulkan)

add_subdirectory(${EXTERNAL_DIR}/stb)

add_subdirectory(${EXTERNAL_DIR}/glfw)

file(GLOB_RECURSE SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h
)

if (NOT DEFINED NODOS_SDK_DIR)
    # Use nosman to get SDK PATH
    if (NOT DEFINED NOSMAN_WORKSPACE_DIR)
        message(FATAL_ERROR "NOSMAN_WORKSPACE_DIR is not defined")
    endif()
    set(NOSMAN_PATH ${NOSMAN_WORKSPACE_DIR}/nosman${CMAKE_EXECUTABLE_SUFFIX})
    execute_process(COMMAND ${NOSMAN_PATH} --workspace "${NOSMAN_WORKSPACE_DIR}" sdk-info "1.2.0" OUTPUT_VARIABLE NOSMAN_SDK_INFO)
    string(JSON NODOS_SDK_DIR GET ${NOSMAN_SDK_INFO} "path")
    string(STRIP ${NODOS_SDK_DIR} NODOS_SDK_DIR)
    set(NODOS_SDK_DIR ${NODOS_SDK_DIR} CACHE PATH "Path to Nodos SDK")
endif()
list(APPEND CMAKE_MODULE_PATH ${NODOS_SDK_DIR}/cmake)
add_library(nosAppSDK INTERFACE)
target_include_directories(nosAppSDK INTERFACE ${NODOS_SDK_DIR}/include)

add_executable(${PROJECT_NAME} ${SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE NODOS_APP_SDK_DLL="${NODOS_SDK_DIR}/bin/nosAppSDK.dll")
target_link_libraries(${PROJECT_NAME} PRIVATE nosVulkan stb glfw nosAppSDK)
