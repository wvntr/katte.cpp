// katte-kv-cache.h - Persistent KV Cache with Compression
#pragma once

#include "ggml.h"
#include "llama.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <sys/mman.h>

namespace katte {

// Fingerprint rigoroso para validação de contexto
struct ContextFingerprint {
    uint8_t model_hash[32];      // SHA256 do modelo
    uint8_t tokenizer_hash[32];  // SHA256 do tokenizer
    uint8_t system_prompt_hash[32];
    uint32_t vocab_size;
    uint32_t n_embd;
    uint32_t n_head;
    uint32_t n_layer;
    uint32_t tool_hash;
    uint32_t config_hash;
    
    bool operator==(const ContextFingerprint& other) const {
        return memcmp(model_hash, other.model_hash, 32) == 0 &&
               memcmp(tokenizer_hash, other.tokenizer_hash, 32) == 0 &&
               memcmp(system_prompt_hash, other.system_prompt_hash, 32) == 0 &&
               vocab_size == other.vocab_size &&
               n_embd == other.n_embd &&
               n_head == other.n_head &&
               n_layer == other.n_layer &&
               tool_hash == other.tool_hash &&
               config_hash == other.config_hash;
    }
};

// Metadados do cache persistente
struct KVMetadata {
    uint64_t version = 1;
    uint64_t token_count;
    uint64_t timestamp;
    uint64_t hits;
    uint64_t disk_size;
    ContextFingerprint fingerprint;
    uint32_t checksum;
    char conversation_id[64];
    uint64_t conversation_revision;
};

// Cache entry comprimido
struct CompressedKVEntry {
    std::vector<uint8_t> compressed_data;
    KVMetadata metadata;
    uint32_t original_size;
    float compression_ratio;
    
    // Fast path: dados descomprimidos em memória se recente
    mutable std::shared_ptr<ggml_context> cached_kv_ctx;
    mutable std::chrono::steady_clock::time_point last_access;
};

// Gerenciador de KV Cache Persistente
class PersistentKVCache {
public:
    PersistentKVCache(const std::string& cache_dir, 
                     size_t max_ram_mb = 2048,
                     size_t max_disk_gb = 100);
    ~PersistentKVCache();
    
    // Salvar estado KV no disco com compressão
    bool save_kv_state(const std::string& conversation_id,
                      uint64_t revision,
                      const llama_context* ctx,
                      const ContextFingerprint& fp,
                      int32_t n_tokens);
    
    // Carregar estado KV do disco (zero-copy via mmap)
    bool load_kv_state(const std::string& conversation_id,
                      uint64_t revision,
                      llama_context* ctx,
                      const ContextFingerprint& fp,
                      int32_t& n_tokens_loaded);
    
    // Verificar se existe cache válido
    bool has_valid_cache(const std::string& conversation_id,
                        uint64_t revision,
                        const ContextFingerprint& fp);
    
    // Estatísticas
    struct Stats {
        uint64_t total_hits;
        uint64_t total_misses;
        uint64_t disk_reads;
        uint64_t disk_writes;
        uint64_t bytes_saved;
        uint64_t tokens_reused;
        float avg_compression_ratio;
        size_t ram_usage;
        size_t disk_usage;
    };
    
    Stats get_stats() const { return stats_; }
    
    // Limpeza inteligente (cold path)
    void garbage_collect();
    void warmup_cache(const std::vector<std::string>& hot_conversations);
    
private:
    // Compressão rápida com LZ4
    std::vector<uint8_t> compress_kv(const void* data, size_t size, float& ratio);
    void* decompress_kv(const uint8_t* data, size_t compressed_size, size_t& original_size);
    
    // Memory-mapped files para zero-copy
    void* mmap_kv_file(const std::string& path, size_t& size);
    void munmap_kv_file(void* addr, size_t size);
    
    // Fingerprinting
    ContextFingerprint compute_fingerprint(const llama_model* model,
                                          const std::string& system_prompt,
                                          uint32_t tool_hash);
    
    // Hot/Cold separation
    void move_to_cold(const std::string& key);
    void move_to_hot(const std::string& key);
    
    std::string cache_dir_;
    size_t max_ram_bytes_;
    size_t max_disk_bytes_;
    
    // Hot cache em RAM (LRU)
    struct LRUCache {
        std::unordered_map<std::string, CompressedKVEntry> entries;
        std::list<std::string> access_order;
        size_t current_size = 0;
    };
    
    LRUCache hot_cache_;
    mutable std::mutex hot_mutex_;
    
    // Índices persistentes
    std::unordered_map<std::string, std::string> conversation_index_; // conv_id -> file_path
    std::mutex index_mutex_;
    
    Stats stats_;
    std::atomic<bool> gc_running_{false};
};

} // namespace katte
