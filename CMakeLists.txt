cmake_minimum_required(VERSION 3.20)

#---------------PARAMS-START----------------
set(MyProjectName "forward_gpg_agent")

# set(PARAMS_HOST_COMPILER CLANG)

set(PARAMS_ADDITIONAL_LINK_LIBS ws2_32)
set(PARAMS_ADDITIONAL_INCLUDE_DIRECTORIES )
set(PARAMS_ADDITIONAL_LINK_DIRECTORIES )
set(PARAMS_UNICODE_USE ON)

#set(CMAKE_GENERATOR Ninja)
set(CMAKE_CXX_EXTENSIONS OFF) # Generate -std=c++17 instead of -std=gnu++17
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_VERBOSE_MAKEFILE OFF)
#----------------PARAMS-END-----------------

project(${MyProjectName} VERSION 1.0 LANGUAGES CXX)

if(PARAMS_UNICODE_USE)
    ADD_DEFINITIONS(-DUNICODE)
    ADD_DEFINITIONS(-D_UNICODE)
endif()

file(GLOB CPP_FILES "./" *.cpp)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${PARAMS_ADDITIONAL_INCLUDE_DIRECTORIES})
link_directories(${PARAMS_ADDITIONAL_LINK_DIRECTORIES})

#-----------------------targets-----------------------

list(APPEND MAINTARGET_LINK_LIBS ${PARAMS_ADDITIONAL_LINK_LIBS})

add_executable(${PROJECT_NAME})
target_sources(${PROJECT_NAME} PRIVATE ${CPP_FILES})
set_target_properties(${PROJECT_NAME} PROPERTIES
            LINKER_LANGUAGE CXX
            CXX_STANDARD 11
)
target_link_libraries(${PROJECT_NAME} PRIVATE ${MAINTARGET_LINK_LIBS})

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

if(EXISTS "${PROJECT_BINARY_DIR}\\compile_commands.json")
    cmake_path(NATIVE_PATH CMAKE_CURRENT_SOURCE_DIR NORMALIZE CMAKE_CURRENT_SOURCE_DIR_NATIVE)
    add_custom_target(
        create_ccdb_prebuild ALL
        COMMAND copy compile_commands.json "\"${CMAKE_CURRENT_SOURCE_DIR_NATIVE}\""
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        DEPENDS compile_commands.json
        COMMENT "Copy compile_commands.json for clangd..."
)
endif()
