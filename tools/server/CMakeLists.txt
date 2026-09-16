include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# Katte KV Cache - Persistent context caching system
set(KATTE_SOURCES
    ${PROJECT_SOURCE_DIR}/src/katte-kv-cache.cpp
    ${PROJECT_SOURCE_DIR}/src/katte-kv-cache.h
    ${PROJECT_SOURCE_DIR}/src/katte-integration.cpp
    ${PROJECT_SOURCE_DIR}/src/katte-integration.h
)

# server-context containing the core server logic, used by llama-server and CLI

set(TARGET server-context)

add_library(${TARGET} STATIC
    server-chat.cpp
    server-chat.h
    server-task.cpp
    server-task.h
    server-queue.cpp
    server-queue.h
    server-common.cpp
    server-common.h
    server-context.cpp
    server-context.h
    server-stream.cpp
    server-stream.h
    server-tools.cpp
    server-tools.h
    server-mcp.cpp
    server-mcp.h
    server-schema.cpp
    server-schema.h
    ${KATTE_SOURCES}
)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(${TARGET} PRIVATE ../mtmd)
target_include_directories(${TARGET} PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(${TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${TARGET} PUBLIC llama-common mtmd ${CMAKE_THREAD_LIBS_INIT})

# Link LZ4 e XXHash para compressão e hashing do Katte
find_package(PkgConfig QUIET)
if(PKGCONFIG_FOUND)
    pkg_check_modules(LZ4 liblz4)
endif()
if(LZ4_FOUND)
    target_include_directories(${TARGET} PRIVATE ${LZ4_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PUBLIC ${LZ4_LIBRARIES})
else()
    target_link_libraries(${TARGET} PUBLIC lz4)
endif()

target_link_libraries(${TARGET} PUBLIC xxhash)

# llama-server-impl: server logic, reusable by app

set(TARGET llama-server-impl)

add_library(${TARGET}
    server.cpp
    server-http.cpp
    server-http.h
    server-models.cpp
    server-models.h
)
set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET} PRIVATE ../mtmd ${PROJECT_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC server-context llama-ui cpp-httplib ${CMAKE_THREAD_LIBS_INIT})

add_dependencies(${TARGET} llama-ui-assets)

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} LIBRARY)
endif()

# llama-server executable

set(TARGET llama-server)

add_executable(${TARGET} main.cpp)
install(TARGETS ${TARGET} RUNTIME)

target_link_libraries(${TARGET} PRIVATE llama-server-impl)
target_compile_features(${TARGET} PRIVATE cxx_std_17)
