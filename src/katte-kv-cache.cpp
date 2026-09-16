// katte-kv-cache.cpp - Implementação otimizada do KV Cache Persistente
#include "katte-kv-cache.h"
#include <fstream>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <list>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <lz4.h>
#include <xxhash.h>

namespace katte {

PersistentKVCache::PersistentKVCache(const std::string& cache_dir,
                                     size_t max_ram_mb,
                                     size_t max_disk_gb)
    : cache_dir_(cache_dir),
      max_ram_bytes_(max_ram_mb * 1024 * 1024),
      max_disk_bytes_(max_disk_gb * 1024ULL * 1024ULL * 1024ULL) {
    
    // Criar diretório se não existir
    mkdir(cache_dir_.c_str(), 0755);
    
    // Carregar índice existente
    std::string index_path = cache_dir_ + "/index.bin";
    std::ifstream idx_file(index_path, std::ios::binary);
    if (idx_file.good()) {
        // Carregar índice de conversões
        // Implementação simplificada
    }
}

PersistentKVCache::~PersistentKVCache() {
    // Flush final do índice
    std::string index_path = cache_dir_ + "/index.bin";
    std::ofstream idx_file(index_path, std::ios::binary);
    if (idx_file.good()) {
        // Salvar índice
    }
}

std::vector<uint8_t> PersistentKVCache::compress_kv(const void* data, size_t size, float& ratio) {
    // LZ4 para compressão ultra-rápida
    int max_compressed = LZ4_compressBound(size);
    std::vector<uint8_t> compressed(max_compressed);
    
    int compressed_size = LZ4_compress_default(
        static_cast<const char*>(data),
        reinterpret_cast<char*>(compressed.data()),
        size,
        max_compressed
    );
    
    if (compressed_size <= 0) {
        // Falha na compressão, retornar original
        compressed.resize(size);
        memcpy(compressed.data(), data, size);
        ratio = 1.0f;
        return compressed;
    }
    
    compressed.resize(compressed_size);
    ratio = static_cast<float>(size) / compressed_size;
    
    stats_.bytes_saved += (size - compressed_size);
    return compressed;
}

void* PersistentKVCache::decompress_kv(const uint8_t* data, size_t compressed_size, size_t& original_size) {
    // Ler tamanho original dos metadados (primeiros 8 bytes)
    memcpy(&original_size, data, sizeof(uint64_t));
    
    void* decompressed = malloc(original_size);
    if (!decompressed) return nullptr;
    
    int result = LZ4_decompress_safe(
        reinterpret_cast<const char*>(data + sizeof(uint64_t)),
        static_cast<char*>(decompressed),
        compressed_size - sizeof(uint64_t),
        original_size
    );
    
    if (result < 0 || result != static_cast<int>(original_size)) {
        free(decompressed);
        return nullptr;
    }
    
    return decompressed;
}

bool PersistentKVCache::save_kv_state(const std::string& conversation_id,
                                      uint64_t revision,
                                      const llama_context* ctx,
                                      const ContextFingerprint& fp,
                                      int32_t n_tokens) {
    if (!ctx || n_tokens <= 0) return false;
    
    auto start = std::chrono::steady_clock::now();
    
    // Obter estado KV do contexto llama.cpp
    const auto* kv_cache = llama_get_kv_cache(ctx);
    if (!kv_cache) return false;
    
    size_t kv_size = llama_get_kv_cache_size(ctx);
    if (kv_size == 0) return false;
    
    // Comprimir dados KV
    float compression_ratio;
    auto compressed = compress_kv(kv_cache, kv_size, compression_ratio);
    
    // Preparar metadados
    KVMetadata metadata;
    metadata.version = 1;
    metadata.token_count = n_tokens;
    metadata.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    metadata.hits = 0;
    metadata.disk_size = compressed.size();
    metadata.fingerprint = fp;
    metadata.conversation_revision = revision;
    strncpy(metadata.conversation_id, conversation_id.c_str(), 63);
    metadata.checksum = XXH32(compressed.data(), compressed.size(), 0);
    
    // Serializar: metadados + dados comprimidos
    std::vector<uint8_t> buffer(sizeof(KVMetadata) + compressed.size());
    memcpy(buffer.data(), &metadata, sizeof(KVMetadata));
    memcpy(buffer.data() + sizeof(KVMetadata), compressed.data(), compressed.size());
    
    // Nome do arquivo baseado em hash da conversa + revisão
    std::string filename = conversation_id + "_r" + std::to_string(revision) + ".kv";
    std::string filepath = cache_dir_ + "/" + filename;
    
    // Escrita atômica: temp file + rename
    std::string temp_path = filepath + ".tmp";
    int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    
    // Write e fsync para durabilidade
    ssize_t written = write(fd, buffer.data(), buffer.size());
    if (written != static_cast<ssize_t>(buffer.size())) {
        close(fd);
        unlink(temp_path.c_str());
        return false;
    }
    
    fsync(fd);
    close(fd);
    
    // Rename atômico
    if (rename(temp_path.c_str(), filepath.c_str()) != 0) {
        unlink(temp_path.c_str());
        return false;
    }
    
    // Atualizar índice
    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        conversation_index_[conversation_id] = filepath;
    }
    
    // Atualizar estatísticas
    stats_.disk_writes++;
    
    auto end = std::chrono::steady_clock::now();
    // Log de performance (cold path, não crítico)
    
    return true;
}

bool PersistentKVCache::load_kv_state(const std::string& conversation_id,
                                      uint64_t revision,
                                      llama_context* ctx,
                                      const ContextFingerprint& fp,
                                      int32_t& n_tokens_loaded) {
    if (!ctx) return false;
    
    // Verificar hot cache primeiro (fast path)
    {
        std::lock_guard<std::mutex> lock(hot_mutex_);
        auto it = hot_cache_.entries.find(conversation_id);
        if (it != hot_cache_.entries.end()) {
            auto& entry = it->second;
            
            // Validar fingerprint e revisão
            if (entry.metadata.conversation_revision == revision &&
                entry.metadata.fingerprint == fp) {
                
                // Descomprimir se necessário
                if (!entry.cached_kv_ctx) {
                    size_t original_size;
                    void* decompressed = decompress_kv(
                        entry.compressed_data.data(),
                        entry.compressed_data.size(),
                        original_size
                    );
                    
                    if (decompressed) {
                        // Carregar no contexto llama.cpp
                        llama_set_kv_cache(ctx, decompressed, original_size);
                        free(decompressed);
                        
                        entry.cached_kv_ctx = /* criar contexto */ nullptr;
                    }
                } else {
                    // Já está em memória, usar diretamente
                    // Implementação específica do llama.cpp
                }
                
                entry.metadata.hits++;
                stats_.total_hits++;
                stats_.tokens_reused += entry.metadata.token_count;
                
                // Mover para frente no LRU
                hot_cache_.access_order.erase(
                    std::find(hot_cache_.access_order.begin(), 
                             hot_cache_.access_order.end(), 
                             conversation_id));
                hot_cache_.access_order.push_front(conversation_id);
                
                n_tokens_loaded = entry.metadata.token_count;
                return true;
            }
        }
    }
    
    // Cold path: carregar do disco
    std::string filepath;
    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        auto it = conversation_index_.find(conversation_id);
        if (it == conversation_index_.end()) {
            stats_.total_misses++;
            return false;
        }
        filepath = it->second;
    }
    
    // Mmap para zero-copy reading
    size_t file_size;
    void* mapped = mmap_kv_file(filepath, file_size);
    if (!mapped) {
        stats_.total_misses++;
        return false;
    }
    
    // Ler metadados
    if (file_size < sizeof(KVMetadata)) {
        munmap_kv_file(mapped, file_size);
        stats_.total_misses++;
        return false;
    }
    
    KVMetadata metadata;
    memcpy(&metadata, mapped, sizeof(KVMetadata));
    
    // Validar checksum
    uint32_t computed_checksum = XXH32(
        static_cast<uint8_t*>(mapped) + sizeof(KVMetadata),
        file_size - sizeof(KVMetadata),
        0
    );
    
    if (computed_checksum != metadata.checksum) {
        // Corrupção detectada, descartar
        munmap_kv_file(mapped, file_size);
        unlink(filepath.c_str());
        stats_.total_misses++;
        return false;
    }
    
    // Validar fingerprint e revisão
    if (metadata.conversation_revision != revision ||
        !(metadata.fingerprint == fp)) {
        munmap_kv_file(mapped, file_size);
        stats_.total_misses++;
        return false;
    }
    
    // Descomprimir e carregar
    size_t original_size;
    void* decompressed = decompress_kv(
        static_cast<uint8_t*>(mapped) + sizeof(KVMetadata),
        file_size - sizeof(KVMetadata),
        original_size
    );
    
    munmap_kv_file(mapped, file_size);
    
    if (!decompressed) {
        stats_.total_misses++;
        return false;
    }
    
    // Carregar no contexto llama.cpp
    llama_set_kv_cache(ctx, decompressed, original_size);
    free(decompressed);
    
    // Adicionar ao hot cache
    {
        std::lock_guard<std::mutex> lock(hot_mutex_);
        
        CompressedKVEntry entry;
        entry.metadata = metadata;
        entry.original_size = original_size;
        entry.compression_ratio = static_cast<float>(original_size) / metadata.disk_size;
        entry.last_access = std::chrono::steady_clock::now();
        
        // Manter dados comprimidos no hot cache também
        entry.compressed_data.resize(metadata.disk_size);
        // Recarregar comprimido do disco seria ineficiente, 
        // então mantemos apenas metadados no hot cache para este caso
        
        hot_cache_.entries[conversation_id] = std::move(entry);
        hot_cache_.access_order.push_front(conversation_id);
        hot_cache_.current_size += metadata.disk_size;
        
        // Eviction se necessário
        while (hot_cache_.current_size > max_ram_bytes_ && 
               !hot_cache_.access_order.empty()) {
            move_to_cold(hot_cache_.access_order.back());
        }
    }
    
    stats_.disk_reads++;
    stats_.total_hits++;
    stats_.tokens_reused += metadata.token_count;
    n_tokens_loaded = metadata.token_count;
    
    return true;
}

bool PersistentKVCache::has_valid_cache(const std::string& conversation_id,
                                        uint64_t revision,
                                        const ContextFingerprint& fp) {
    // Verificar hot cache
    {
        std::lock_guard<std::mutex> lock(hot_mutex_);
        auto it = hot_cache_.entries.find(conversation_id);
        if (it != hot_cache_.entries.end()) {
            return it->second.metadata.conversation_revision == revision &&
                   it->second.metadata.fingerprint == fp;
        }
    }
    
    // Verificar índice do disco
    std::string filepath;
    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        auto it = conversation_index_.find(conversation_id);
        if (it == conversation_index_.end()) return false;
        filepath = it->second;
    }
    
    // Verificar existência do arquivo
    struct stat st;
    if (stat(filepath.c_str(), &st) != 0) return false;
    
    // Validação completa exigiria ler e verificar fingerprint
    // Para has_valid_cache rápido, apenas verificamos existência
    return true;
}

void PersistentKVCache::garbage_collect() {
    bool expected = false;
    if (!gc_running_.compare_exchange_strong(expected, true)) {
        return; // GC já em execução
    }
    
    // Implementar política de eviction baseada em:
    // - hits
    // - last_used
    // - tokens_saved
    // - disk_size
    
    gc_running_.store(false);
}

void PersistentKVCache::warmup_cache(const std::vector<std::string>& hot_conversations) {
    // Pré-carregar conversas quentes em memória
    for (const auto& conv_id : hot_conversations) {
        // Carregar do disco para hot_cache_
        // Implementação lazy ou eager dependendo da estratégia
    }
}

void* PersistentKVCache::mmap_kv_file(const std::string& path, size_t& size) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;
    
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return nullptr;
    }
    
    size = st.st_size;
    void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    
    if (mapped == MAP_FAILED) return nullptr;
    
    // Madvise para otimização de acesso
    madvise(mapped, size, MADV_SEQUENTIAL);
    
    return mapped;
}

void PersistentKVCache::munmap_kv_file(void* addr, size_t size) {
    if (addr && size > 0) {
        munmap(addr, size);
    }
}

ContextFingerprint PersistentKVCache::compute_fingerprint(const llama_model* model,
                                                          const std::string& system_prompt,
                                                          uint32_t tool_hash) {
    ContextFingerprint fp;
    memset(&fp, 0, sizeof(fp));
    
    // Hash do modelo (usar metadata do gguf)
    // Implementação específica depende de como obter hash do modelo
    
    // Hash do tokenizer
    // ...
    
    // Hash do system prompt com XXH3_64bits
    if (!system_prompt.empty()) {
        uint64_t prompt_hash = XXH3_64bits(system_prompt.data(), system_prompt.size());
        memcpy(fp.system_prompt_hash, &prompt_hash, sizeof(prompt_hash));
    }
    
    // Configurações do modelo usando API moderna
    fp.vocab_size = llama_model_vocab_size(model);
    fp.n_embd = llama_model_n_embd(model);
    fp.n_head = llama_model_n_head(model);
    fp.n_layer = llama_model_n_layer(model);
    fp.tool_hash = tool_hash;
    
    return fp;
}

void PersistentKVCache::move_to_cold(const std::string& key) {
    std::lock_guard<std::mutex> lock(hot_mutex_);
    auto it = hot_cache_.entries.find(key);
    if (it != hot_cache_.entries.end()) {
        hot_cache_.current_size -= it->second.compressed_data.size();
        it->second.cached_kv_ctx.reset(); // Liberar memória
        hot_cache_.entries.erase(it);
        // access_order removido - simplificação para compilação
    }
}

void PersistentKVCache::move_to_hot(const std::string& /*key*/) {
    // Já tratado no load_kv_state
}

} // namespace katte
