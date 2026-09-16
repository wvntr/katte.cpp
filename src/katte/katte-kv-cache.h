#pragma once

// Katte-LLama: Persistent KV Cache with SSD-backed storage
// Objetivo: Reduzir consumo de RAM em 70-90% mantendo qualidade imperceptível

#include "llama-kv-cache.h"
#include "llama-model.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <filesystem>

// Fingerprint SHA256 para identificação única de contexto
struct katte_context_fingerprint {
    std::string model_id;
    std::string model_revision;
    std::string tokenizer_hash;
    std::string chat_template_hash;
    std::string system_prompt_hash;
    std::string tools_hash;
    int32_t thinking_mode;
    std::string runtime_config_hash;
    std::vector<llama_token> tokens_sequence;
    
    // Gera hash SHA256 único para este contexto
    std::string compute_hash() const;
    
    // Verifica compatibilidade com outro fingerprint
    bool is_compatible(const katte_context_fingerprint& other) const;
};

// Metadados armazenados com cada KV cache persistente
struct katte_kv_metadata {
    std::string hash;
    uint64_t version;
    uint64_t size_bytes;
    uint32_t token_count;
    std::string model_id;
    std::string metadata_hash;
    int64_t created_at;
    int64_t last_used;
    uint64_t hit_count;
    uint64_t tokens_saved;
    
    // Serializa para JSON
    std::string to_json() const;
    
    // Desserializa de JSON
    static katte_kv_metadata from_json(const std::string& json);
};

// Tier de armazenamento para KV cache
enum class katte_storage_tier {
    TIER_L1_CACHE,      // CPU L1/L2 (MBs, ultra-rápido)
    TIER_RAM_HOT,       // RAM hot cache (2-4GB, rápido)
    TIER_RAM_MMAP,      // RAM mmap buffer (4-6GB, médio)
    TIER_SSD_PERSIST    // SSD persistente (TBs, lento mas durável)
};

// Entrada no cache persistente
struct katte_kv_entry {
    katte_context_fingerprint fingerprint;
    katte_kv_metadata metadata;
    katte_storage_tier tier;
    
    // Dados KV (pode estar em RAM ou mmap do SSD)
    void* kv_data;
    size_t kv_size;
    
    // Estado de validade
    bool is_valid;
    bool is_loading;
    bool is_dirty;
    
    // Lock para thread safety
    std::mutex entry_mutex;
};

// Gerenciador principal do KV cache persistente
class katte_persistent_kv_cache {
public:
    katte_persistent_kv_cache(
        const std::string& cache_dir,
        size_t max_ram_bytes = 4ULL * 1024 * 1024 * 1024,  // 4GB default
        size_t max_ssd_bytes = 100ULL * 1024 * 1024 * 1024  // 100GB default
    );
    
    ~katte_persistent_kv_cache();
    
    // Inicializa o sistema de cache
    bool initialize();
    
    // Limpa todo o cache
    void clear();
    
    // Busca entrada por fingerprint
    // Retorna nullptr se não existir ou for inválido
    katte_kv_entry* find(const katte_context_fingerprint& fp);
    
    // Armazena nova entrada no cache
    // Retorna true se sucesso, false se falhar
    bool store(
        const katte_context_fingerprint& fp,
        const void* kv_data,
        size_t kv_size,
        katte_storage_tier initial_tier = katte_storage_tier::TIER_RAM_HOT
    );
    
    // Remove entrada do cache
    bool remove(const katte_context_fingerprint& fp);
    
    // Atualiza metadados de uso (hit)
    void record_hit(const katte_context_fingerprint& fp);
    
    // Promove entrada para tier superior
    bool promote_tier(const katte_context_fingerprint& fp, katte_storage_tier new_tier);
    
    // Demove entrada para tier inferior (eviction)
    bool demote_tier(const katte_context_fingerprint& fp, katte_storage_tier new_tier);
    
    // Estatísticas do cache
    struct stats {
        size_t total_entries;
        size_t ram_usage_bytes;
        size_t ssd_usage_bytes;
        uint64_t total_hits;
        uint64_t total_misses;
        uint64_t total_stores;
        uint64_t evictions_count;
        double hit_rate;
    };
    
    stats get_stats() const;
    
    // Garbage collection (cold path)
    void run_gc();
    
    // Flush assíncrono de entradas dirty (cold path)
    void flush_dirty_entries();
    
    // Getter público para o diretório do cache
    const std::string& get_cache_directory() const;
    
private:
    std::string cache_directory;
    size_t max_ram_bytes;
    size_t max_ssd_bytes;
    
    // Mapas de busca rápida
    std::unordered_map<std::string, std::unique_ptr<katte_kv_entry>> entries_by_hash;
    
    // Lists por tier para eviction policy
    std::vector<std::string> l1_cache_list;
    std::vector<std::string> ram_hot_list;
    std::vector<std::string> ram_mmap_list;
    std::vector<std::string> ssd_persist_list;
    
    // Mutex global para operações de estrutura
    mutable std::mutex cache_mutex;
    
    // Contadores de estatísticas
    mutable uint64_t stat_hits;
    mutable uint64_t stat_misses;
    mutable uint64_t stat_stores;
    mutable uint64_t stat_evictions;
    
    // Métodos internos
    bool load_from_ssd(const katte_context_fingerprint& fp, katte_kv_entry* entry);
    bool save_to_ssd(const katte_context_fingerprint& fp, const katte_kv_entry* entry);
    bool validate_entry(const katte_kv_entry* entry) const;
    void evict_coldest_entry();
    std::string get_ssd_path(const std::string& hash, bool is_metadata) const;
};

// Conversation KV: gerencia estado por conversa com versionamento
class katte_conversation_kv {
public:
    katte_conversation_kv(
        katte_persistent_kv_cache& parent_cache,
        const std::string& conversation_id
    );
    
    ~katte_conversation_kv();
    
    // Cria novo snapshot da conversa
    bool create_snapshot(
        uint32_t revision,
        const katte_context_fingerprint& fp,
        const void* kv_data,
        size_t kv_size
    );
    
    // Restaura snapshot específico
    // Retorna false se revisão não existir ou for inválida
    bool restore_snapshot(
        uint32_t revision,
        katte_context_fingerprint& out_fp,
        void*& out_kv_data,
        size_t& out_kv_size
    );
    
    // Lista todas as revisões disponíveis
    std::vector<uint32_t> list_revisions() const;
    
    // Remove revisão específica
    bool remove_revision(uint32_t revision);
    
    // Cria branch a partir de revisão existente
    bool create_branch(
        const std::string& new_conv_id,
        uint32_t from_revision
    );
    
    // Obtém ID da conversa
    const std::string& get_conversation_id() const { return conversation_id; }
    
    // Acesso ao diretório do cache pai (necessário para conversation_kv)
    const std::string& get_cache_directory() const;
    
private:
    std::string conversation_id;
    katte_persistent_kv_cache& parent_cache;
    
    // Snapshots por revisão
    std::unordered_map<uint32_t, katte_context_fingerprint> revisions;
    
    mutable std::mutex conv_mutex;
    
    std::string get_snapshot_path(uint32_t revision) const;
};

// Context Compression: compressão lossless de histórico distante
class katte_context_compressor {
public:
    katte_context_compressor();
    ~katte_context_compressor();
    
    // Compressão de sequência de tokens
    // Retorna tamanho comprimido em bytes
    size_t compress_tokens(
        const llama_token* tokens,
        size_t token_count,
        uint8_t* output_buffer,
        size_t buffer_size
    ) const;
    
    // Descompressão de sequência de tokens
    // Retorna número de tokens descomprimidos
    size_t decompress_tokens(
        const uint8_t* compressed_data,
        size_t compressed_size,
        llama_token* output_tokens,
        size_t max_tokens
    ) const;
    
    // Calcula ratio de compressão esperado
    double estimate_compression_ratio(size_t token_count) const;
    
private:
    // Algoritmo interno de compressão (ex: Huffman + RLE adaptado para tokens)
    struct impl;
    std::unique_ptr<impl> pimpl;
};

// Utility functions
namespace katte {
    // Gera hash SHA256 de buffer
    std::string sha256_hex(const void* data, size_t size);
    
    // Gera hash SHA256 de string
    std::string sha256_hex(const std::string& str);
    
    // Atomic write: write + fsync + rename
    bool atomic_write_file(
        const std::string& target_path,
        const void* data,
        size_t size
    );
    
    // Lê arquivo inteiro para buffer
    bool read_file_whole(
        const std::string& path,
        std::vector<uint8_t>& out_data
    );
    
    // Verifica integridade de arquivo via SHA256
    bool verify_file_integrity(
        const std::string& path,
        const std::string& expected_hash
    );
    
    // Cria diretórios recursivamente
    bool ensure_directory_exists(const std::string& path);
}
