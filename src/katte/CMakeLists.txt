# Katte-LLama: Persistent KV Cache Module
# Adiciona suporte a KV cache persistente em SSD com fingerprinting rigoroso

set(KATTE_SOURCES
    katte-kv-cache.cpp
)

set(KATTE_HEADERS
    katte-kv-cache.h
)

# Cria biblioteca de objetos (não linka com llama para evitar ciclo)
add_library(katte OBJECT ${KATTE_SOURCES} ${KATTE_HEADERS})

target_include_directories(katte PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/ggml/include
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(katte PUBLIC cxx_std_17)

# Garante -fPIC para shared library
set_target_properties(katte PROPERTIES POSITION_INDEPENDENT_CODE ON)

# OpenSSL para SHA256
find_package(OpenSSL REQUIRED)
target_link_libraries(katte PRIVATE OpenSSL::Crypto)
