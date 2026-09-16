# Katte-LLama: Persistent KV Cache and Extreme Optimizations Module
# Adiciona suporte a KV cache persistente em SSD com fingerprinting rigoroso
# e otimizações extremas para CPU/GPU

set(KATTE_SOURCES
    katte-kv-cache.cpp
    katte-core.cpp
)

set(KATTE_HEADERS
    katte-kv-cache.h
    katte-core.h
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

# Otimizações agressivas para performance extrema
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(katte PRIVATE
        -O3
        -march=native
        -mtune=native
        -ffast-math
        -funroll-loops
        -ftree-vectorize
    )
endif()
