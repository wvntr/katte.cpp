// Katte-LLama: Persistent KV Cache Implementation
// Implementação dos conceitos de SSD-backed KV cache com fingerprinting rigoroso

#include "katte-kv-cache.h"
#include "llama-impl.h"

#include <openssl/sha.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <sys/types.h>

#ifdef _WIN32
    #include <io.h>
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <fcntl.h>
#endif

// ============================================================================
// Utility Functions (namespace katte)
// ============================================================================

std::string katte::sha256_hex(const void* data, size_t size) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, data, size);
    SHA256_Final(hash, &sha256);
    
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

std::string katte::sha256_hex(const std::string& str) {
    return sha256_hex(str.c_str(), str.size());
}

bool katte::atomic_write_file(
    const std::string& target_path,
    const void* data,
    size_t size
) {
    // Cria diretório se necessário
    std::filesystem::path p(target_path);
    if (!ensure_directory_exists(p.parent_path().string())) {
        return false;
    }
    
    // Escreve em arquivo temporário
    std::string temp_path = target_path + ".tmp." + std::to_string(getpid());
    
    std::ofstream ofs(temp_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return false;
    }
    
    ofs.write(static_cast<const char*>(data), size);
    if (!ofs.good()) {
        ofs.close();
        std::remove(temp_path.c_str());
        return false;
    }
    
    // Flush e fsync
    ofs.flush();
#ifdef _WIN32
    HANDLE h = CreateFileA(temp_path.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
#else
    ofs.close();
    int fd = open(temp_path.c_str(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
#endif
    
    ofs.close();
    
    // Rename atômico
    if (std::rename(temp_path.c_str(), target_path.c_str()) != 0) {
        std::remove(temp_path.c_str());
        return false;
    }
    
    return true;
}

bool katte::read_file_whole(
    const std::string& path,
    std::vector<uint8_t>& out_data
) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return false;
    }
    
    auto size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    
    out_data.resize(size);
    ifs.read(reinterpret_cast<char*>(out_data.data()), size);
    
    return ifs.good();
}

bool katte::verify_file_integrity(
    const std::string& path,
    const std::string& expected_hash
) {
    std::vector<uint8_t> data;
    if (!read_file_whole(path, data)) {
        return false;
    }
    
    std::string actual_hash = sha256_hex(data.data(), data.size());
    return actual_hash == expected_hash;
}

bool katte::ensure_directory_exists(const std::string& path) {
    try {
        return std::filesystem::create_directories(path);
    } catch (...) {
        return std::filesystem::exists(path);
    }
}

// ============================================================================
// katte_context_fingerprint Implementation
// ============================================================================

std::string katte_context_fingerprint::compute_hash() const {
    // Combina todos os campos em um único hash
    std::stringstream ss;
    
    ss << model_id << "|";
    ss << model_revision << "|";
    ss << tokenizer_hash << "|";
    ss << chat_template_hash << "|";
    ss << system_prompt_hash << "|";
    ss << tools_hash << "|";
    ss << thinking_mode << "|";
    ss << runtime_config_hash << "|";
    
    // Hash da sequência de tokens
    if (!tokens_sequence.empty()) {
        std::string token_hash = katte::sha256_hex(
            tokens_sequence.data(),
            tokens_sequence.size() * sizeof(llama_token)
        );
        ss << token_hash;
    }
    
    return katte::sha256_hex(ss.str());
}

bool katte_context_fingerprint::is_compatible(const katte_context_fingerprint& other) const {
    // Compatibilidade requer igualdade exata em todos os campos críticos
    return model_id == other.model_id &&
           model_revision == other.model_revision &&
           tokenizer_hash == other.tokenizer_hash &&
           chat_template_hash == other.chat_template_hash &&
           system_prompt_hash == other.system_prompt_hash &&
           tools_hash == other.tools_hash &&
           thinking_mode == other.thinking_mode &&
           runtime_config_hash == other.runtime_config_hash;
}

// ============================================================================
// katte_kv_metadata Implementation
// ============================================================================

std::string katte_kv_metadata::to_json() const {
    // JSON manual simples (evita dependência de nlohmann/json aqui)
    std::stringstream ss;
    ss << "{";
    ss << "\"hash\":\"" << hash << "\",";
    ss << "\"version\":" << version << ",";
    ss << "\"size_bytes\":" << size_bytes << ",";
    ss << "\"token_count\":" << token_count << ",";
    ss << "\"model_id\":\"" << model_id << "\",";
    ss << "\"metadata_hash\":\"" << metadata_hash << "\",";
    ss << "\"created_at\":" << created_at << ",";
    ss << "\"last_used\":" << last_used << ",";
    ss << "\"hit_count\":" << hit_count << ",";
    ss << "\"tokens_saved\":" << tokens_saved;
    ss << "}";
    return ss.str();
}

katte_kv_metadata katte_kv_metadata::from_json(const std::string& json) {
    katte_kv_metadata meta;
    // Parsing manual simplificado - em produção usaria nlohmann/json
    // Esta é uma implementação placeholder
    meta.version = 1;
    meta.created_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    meta.last_used = meta.created_at;
    return meta;
}

// ============================================================================
// katte_persistent_kv_cache Implementation
// ============================================================================

katte_persistent_kv_cache::katte_persistent_kv_cache(
    const std::string& cache_dir,
    size_t max_ram_bytes,
    size_t max_ssd_bytes
) : cache_directory(cache_dir),
    max_ram_bytes(max_ram_bytes),
    max_ssd_bytes(max_ssd_bytes),
    stat_hits(0),
    stat_misses(0),
    stat_stores(0),
    stat_evictions(0)
{
}

katte_persistent_kv_cache::~katte_persistent_kv_cache() {
    clear();
}

bool katte_persistent_kv_cache::initialize() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    // Cria estrutura de diretórios
    if (!katte::ensure_directory_exists(cache_directory)) {
        LLAMA_LOG_ERROR("%s: failed to create cache directory: %s\n", 
                       __func__, cache_directory.c_str());
        return false;
    }
    
    if (!katte::ensure_directory_exists(cache_directory + "/permanent")) {
        return false;
    }
    
    if (!katte::ensure_directory_exists(cache_directory + "/conversations")) {
        return false;
    }
    
    LLAMA_LOG_INFO("%s: initialized cache at %s (RAM: %zu GB, SSD: %zu GB)\n",
                  __func__, cache_directory.c_str(),
                  max_ram_bytes / (1024*1024*1024),
                  max_ssd_bytes / (1024*1024*1024));
    
    return true;
}

void katte_persistent_kv_cache::clear() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    entries_by_hash.clear();
    l1_cache_list.clear();
    ram_hot_list.clear();
    ram_mmap_list.clear();
    ssd_persist_list.clear();
}

katte_kv_entry* katte_persistent_kv_cache::find(const katte_context_fingerprint& fp) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    std::string hash = fp.compute_hash();
    auto it = entries_by_hash.find(hash);
    
    if (it == entries_by_hash.end()) {
        stat_misses++;
        
        // Tenta carregar do SSD
        // Nota: implementação completa carregaria do SSD aqui
        return nullptr;
    }
    
    katte_kv_entry* entry = it->second.get();
    
    if (!entry->is_valid) {
        stat_misses++;
        return nullptr;
    }
    
    // Atualiza metadados de uso
    entry->metadata.last_used = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    entry->metadata.hit_count++;
    stat_hits++;
    
    return entry;
}

bool katte_persistent_kv_cache::store(
    const katte_context_fingerprint& fp,
    const void* kv_data,
    size_t kv_size,
    katte_storage_tier initial_tier
) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    std::string hash = fp.compute_hash();
    
    // Verifica se já existe
    if (entries_by_hash.find(hash) != entries_by_hash.end()) {
        LLAMA_LOG_WARN("%s: entry already exists: %s\n", __func__, hash.c_str());
        return true;  // Já existe, considerado sucesso
    }
    
    // Cria nova entrada
    auto entry = std::make_unique<katte_kv_entry>();
    entry->fingerprint = fp;
    entry->metadata.hash = hash;
    entry->metadata.size_bytes = kv_size;
    entry->metadata.token_count = fp.tokens_sequence.size();
    entry->metadata.model_id = fp.model_id;
    entry->tier = initial_tier;
    entry->kv_size = kv_size;
    entry->is_valid = true;
    entry->is_loading = false;
    entry->is_dirty = true;
    
    // Aloca memória baseada no tier
    switch (initial_tier) {
        case katte_storage_tier::TIER_L1_CACHE:
        case katte_storage_tier::TIER_RAM_HOT:
            entry->kv_data = malloc(kv_size);
            if (!entry->kv_data) {
                LLAMA_LOG_ERROR("%s: failed to allocate %zu bytes for KV cache\n",
                               __func__, kv_size);
                return false;
            }
            memcpy(entry->kv_data, kv_data, kv_size);
            break;
            
        case katte_storage_tier::TIER_RAM_MMAP:
        case katte_storage_tier::TIER_SSD_PERSIST:
            // Para tiers frios, salva diretamente no SSD
            entry->kv_data = malloc(kv_size);
            if (!entry->kv_data) {
                return false;
            }
            memcpy(entry->kv_data, kv_data, kv_size);
            
            // Salva no SSD (cold path, mas feito síncrono aqui por simplicidade)
            if (!save_to_ssd(fp, entry.get())) {
                free(entry->kv_data);
                entry->kv_data = nullptr;
                return false;
            }
            entry->is_dirty = false;
            break;
    }
    
    // Adiciona aos mapas
    entries_by_hash[hash] = std::move(entry);
    
    // Adiciona à lista do tier apropriado
    switch (initial_tier) {
        case katte_storage_tier::TIER_L1_CACHE:
            l1_cache_list.push_back(hash);
            break;
        case katte_storage_tier::TIER_RAM_HOT:
            ram_hot_list.push_back(hash);
            break;
        case katte_storage_tier::TIER_RAM_MMAP:
            ram_mmap_list.push_back(hash);
            break;
        case katte_storage_tier::TIER_SSD_PERSIST:
            ssd_persist_list.push_back(hash);
            break;
    }
    
    stat_stores++;
    
    LLAMA_LOG_DEBUG("%s: stored entry %s (%zu bytes, tier=%d)\n",
                   __func__, hash.c_str(), kv_size, (int)initial_tier);
    
    return true;
}

bool katte_persistent_kv_cache::remove(const katte_context_fingerprint& fp) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    std::string hash = fp.compute_hash();
    auto it = entries_by_hash.find(hash);
    
    if (it == entries_by_hash.end()) {
        return false;
    }
    
    katte_kv_entry* entry = it->second.get();
    
    // Remove da lista do tier
    auto remove_from_list = [&](std::vector<std::string>& list) {
        list.erase(std::remove(list.begin(), list.end(), hash), list.end());
    };
    
    remove_from_list(l1_cache_list);
    remove_from_list(ram_hot_list);
    remove_from_list(ram_mmap_list);
    remove_from_list(ssd_persist_list);
    
    // Libera memória
    if (entry->kv_data) {
        free(entry->kv_data);
    }
    
    // Remove arquivo do SSD se existir
    std::string kv_path = get_ssd_path(hash, false);
    std::string meta_path = get_ssd_path(hash, true);
    std::remove(kv_path.c_str());
    std::remove(meta_path.c_str());
    
    // Remove do mapa
    entries_by_hash.erase(it);
    
    return true;
}

void katte_persistent_kv_cache::record_hit(const katte_context_fingerprint& fp) {
    // Já feito no find(), mas pode ser chamado explicitamente
    find(fp);
}

bool katte_persistent_kv_cache::promote_tier(
    const katte_context_fingerprint& fp,
    katte_storage_tier new_tier
) {
    // TODO: Implementar promoção de tier
    return false;
}

bool katte_persistent_kv_cache::demote_tier(
    const katte_context_fingerprint& fp,
    katte_storage_tier new_tier
) {
    // TODO: Implementar demotion de tier
    return false;
}

katte_persistent_kv_cache::stats katte_persistent_kv_cache::get_stats() const {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    stats s;
    s.total_entries = entries_by_hash.size();
    s.total_hits = stat_hits;
    s.total_misses = stat_misses;
    s.total_stores = stat_stores;
    s.evictions_count = stat_evictions;
    
    // Calcula uso de RAM
    s.ram_usage_bytes = 0;
    for (const auto& pair : entries_by_hash) {
        if (pair.second->tier == katte_storage_tier::TIER_RAM_HOT ||
            pair.second->tier == katte_storage_tier::TIER_RAM_MMAP ||
            pair.second->tier == katte_storage_tier::TIER_L1_CACHE) {
            s.ram_usage_bytes += pair.second->kv_size;
        }
    }
    
    // Uso de SSD (aproximado)
    s.ssd_usage_bytes = ssd_persist_list.size() * 1024 * 1024;  // Estimativa
    
    s.hit_rate = (stat_hits + stat_misses > 0) ?
                 (double)stat_hits / (stat_hits + stat_misses) : 0.0;
    
    return s;
}

void katte_persistent_kv_cache::run_gc() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    // Garbage collection: remove entradas não usadas há muito tempo
    // Implementação simplificada - em produção seria mais sofisticada
    
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    int64_t max_age_seconds = 3600;  // 1 hora
    
    std::vector<std::string> to_remove;
    
    for (const auto& pair : entries_by_hash) {
        const auto& entry = pair.second;
        if (entry->metadata.hit_count == 0 &&
            (now - entry->metadata.last_used) > max_age_seconds) {
            to_remove.push_back(pair.first);
        }
    }
    
    // Limita GC a 10% das entradas por vez
    size_t max_remove = std::max((size_t)1, entries_by_hash.size() / 10);
    size_t removed = 0;
    
    for (const auto& hash : to_remove) {
        if (removed >= max_remove) break;
        
        auto it = entries_by_hash.find(hash);
        if (it != entries_by_hash.end()) {
            // Libera memória
            if (it->second->kv_data) {
                free(it->second->kv_data);
            }
            
            // Remove arquivos do SSD
            std::string kv_path = get_ssd_path(hash, false);
            std::string meta_path = get_ssd_path(hash, true);
            std::remove(kv_path.c_str());
            std::remove(meta_path.c_str());
            
            entries_by_hash.erase(it);
            removed++;
            stat_evictions++;
        }
    }
    
    if (removed > 0) {
        LLAMA_LOG_INFO("%s: evicted %zu entries\n", __func__, removed);
    }
}

void katte_persistent_kv_cache::flush_dirty_entries() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    for (auto& pair : entries_by_hash) {
        katte_kv_entry* entry = pair.second.get();
        
        if (entry->is_dirty && entry->is_valid) {
            if (save_to_ssd(entry->fingerprint, entry)) {
                entry->is_dirty = false;
            } else {
                LLAMA_LOG_ERROR("%s: failed to flush entry %s\n",
                               __func__, pair.first.c_str());
            }
        }
    }
}

bool katte_persistent_kv_cache::load_from_ssd(
    const katte_context_fingerprint& fp,
    katte_kv_entry* entry
) {
    // TODO: Implementar carregamento do SSD
    return false;
}

bool katte_persistent_kv_cache::save_to_ssd(
    const katte_context_fingerprint& fp,
    const katte_kv_entry* entry
) {
    std::string hash = fp.compute_hash();
    
    // Salva dados KV
    std::string kv_path = get_ssd_path(hash, false);
    if (!katte::atomic_write_file(kv_path, entry->kv_data, entry->kv_size)) {
        LLAMA_LOG_ERROR("%s: failed to write KV data to %s\n", __func__, kv_path.c_str());
        return false;
    }
    
    // Salva metadados
    std::string meta_path = get_ssd_path(hash, true);
    std::string meta_json = entry->metadata.to_json();
    if (!katte::atomic_write_file(meta_path, meta_json.c_str(), meta_json.size())) {
        LLAMA_LOG_ERROR("%s: failed to write metadata to %s\n", __func__, meta_path.c_str());
        return false;
    }
    
    LLAMA_LOG_DEBUG("%s: saved entry %s to SSD\n", __func__, hash.c_str());
    return true;
}

bool katte_persistent_kv_cache::validate_entry(const katte_kv_entry* entry) const {
    if (!entry || !entry->is_valid || !entry->kv_data) {
        return false;
    }
    
    // Validação adicional poderia verificar hash dos dados
    return true;
}

void katte_persistent_kv_cache::evict_coldest_entry() {
    // Encontra entrada com menor hit_count e mais antiga
    std::string coldest_hash;
    uint64_t min_score = UINT64_MAX;
    
    for (const auto& pair : entries_by_hash) {
        const auto& entry = pair.second;
        
        // Score baseado em hits e tempo desde último uso
        int64_t age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count() - entry->metadata.last_used;
        
        uint64_t score = entry->metadata.hit_count + (age / 60);  // Penaliza idade
        
        if (score < min_score) {
            min_score = score;
            coldest_hash = pair.first;
        }
    }
    
    if (!coldest_hash.empty()) {
        auto it = entries_by_hash.find(coldest_hash);
        if (it != entries_by_hash.end()) {
            // Move para SSD se estiver em RAM
            if (it->second->tier == katte_storage_tier::TIER_RAM_HOT) {
                save_to_ssd(it->second->fingerprint, it->second.get());
                
                // Libera memória RAM
                free(it->second->kv_data);
                it->second->kv_data = nullptr;
                it->second->tier = katte_storage_tier::TIER_SSD_PERSIST;
                
                // Atualiza listas
                ram_hot_list.erase(
                    std::remove(ram_hot_list.begin(), ram_hot_list.end(), coldest_hash),
                    ram_hot_list.end()
                );
                ssd_persist_list.push_back(coldest_hash);
            }
            
            stat_evictions++;
        }
    }
}

std::string katte_persistent_kv_cache::get_ssd_path(
    const std::string& hash,
    bool is_metadata
) const {
    return cache_directory + "/permanent/" + hash + (is_metadata ? ".meta" : ".kv");
}

// ============================================================================
// katte_conversation_kv Implementation
// ============================================================================

katte_conversation_kv::katte_conversation_kv(
    katte_persistent_kv_cache& parent_cache,
    const std::string& conv_id
) : conversation_id(conv_id),
    parent_cache(parent_cache)
{
}

katte_conversation_kv::~katte_conversation_kv() {
}

bool katte_conversation_kv::create_snapshot(
    uint32_t revision,
    const katte_context_fingerprint& fp,
    const void* kv_data,
    size_t kv_size
) {
    std::lock_guard<std::mutex> lock(conv_mutex);
    
    // Armazena no cache pai
    if (!parent_cache.store(fp, kv_data, kv_size)) {
        return false;
    }
    
    revisions[revision] = fp;
    return true;
}

bool katte_conversation_kv::restore_snapshot(
    uint32_t revision,
    katte_context_fingerprint& out_fp,
    void*& out_kv_data,
    size_t& out_kv_size
) {
    std::lock_guard<std::mutex> lock(conv_mutex);
    
    auto it = revisions.find(revision);
    if (it == revisions.end()) {
        return false;
    }
    
    out_fp = it->second;
    
    // Busca no cache pai
    katte_kv_entry* entry = parent_cache.find(out_fp);
    if (!entry || !entry->is_valid) {
        return false;
    }
    
    out_kv_data = entry->kv_data;
    out_kv_size = entry->kv_size;
    
    return true;
}

std::vector<uint32_t> katte_conversation_kv::list_revisions() const {
    std::lock_guard<std::mutex> lock(conv_mutex);
    
    std::vector<uint32_t> revs;
    for (const auto& pair : revisions) {
        revs.push_back(pair.first);
    }
    
    std::sort(revs.begin(), revs.end());
    return revs;
}

bool katte_conversation_kv::remove_revision(uint32_t revision) {
    std::lock_guard<std::mutex> lock(conv_mutex);
    
    auto it = revisions.find(revision);
    if (it == revisions.end()) {
        return false;
    }
    
    parent_cache.remove(it->second);
    revisions.erase(it);
    
    return true;
}

bool katte_conversation_kv::create_branch(
    const std::string& new_conv_id,
    uint32_t from_revision
) {
    // TODO: Implementar branching de conversas
    return false;
}

std::string katte_conversation_kv::get_snapshot_path(uint32_t revision) const {
    // Acessa o diretório através de método público ou getter
    return parent_cache.get_cache_directory() + "/conversations/" +
           conversation_id + "/rev_" + std::to_string(revision);
}

// Getter público para o diretório do cache
const std::string& katte_persistent_kv_cache::get_cache_directory() const {
    return cache_directory;
}

// ============================================================================
// katte_context_compressor Implementation
// ============================================================================

struct katte_context_compressor::impl {
    // Placeholder para implementação real de compressão
    // Em produção: Huffman coding adaptado para distribuição de tokens do modelo
};

katte_context_compressor::katte_context_compressor() : pimpl(std::make_unique<impl>()) {
}

katte_context_compressor::~katte_context_compressor() {
}

size_t katte_context_compressor::compress_tokens(
    const llama_token* tokens,
    size_t token_count,
    uint8_t* output_buffer,
    size_t buffer_size
) const {
    // Implementação placeholder - compressão RLE simples
    if (token_count == 0 || !tokens || !output_buffer) {
        return 0;
    }
    
    size_t out_pos = 0;
    size_t in_pos = 0;
    
    while (in_pos < token_count && out_pos < buffer_size) {
        llama_token current = tokens[in_pos];
        size_t run_length = 1;
        
        // Conta repetições
        while (in_pos + run_length < token_count &&
               tokens[in_pos + run_length] == current &&
               run_length < 255) {
            run_length++;
        }
        
        // Escreve par (token, count)
        if (out_pos + 2 > buffer_size) {
            break;
        }
        
        output_buffer[out_pos++] = static_cast<uint8_t>(current & 0xFF);
        output_buffer[out_pos++] = static_cast<uint8_t>(run_length);
        
        in_pos += run_length;
    }
    
    return out_pos;
}

size_t katte_context_compressor::decompress_tokens(
    const uint8_t* compressed_data,
    size_t compressed_size,
    llama_token* output_tokens,
    size_t max_tokens
) const {
    if (compressed_size == 0 || !compressed_data || !output_tokens) {
        return 0;
    }
    
    size_t out_pos = 0;
    size_t in_pos = 0;
    
    while (in_pos + 1 < compressed_size && out_pos < max_tokens) {
        llama_token token = compressed_data[in_pos++];
        size_t run_length = compressed_data[in_pos++];
        
        for (size_t i = 0; i < run_length && out_pos < max_tokens; ++i) {
            output_tokens[out_pos++] = token;
        }
    }
    
    return out_pos;
}

double katte_context_compressor::estimate_compression_ratio(size_t token_count) const {
    // Estimativa baseada em padrões típicos de LLM
    // Tokens de linguagem natural têm boa compressibilidade
    if (token_count < 100) {
        return 0.7;  // 70% do tamanho original
    } else if (token_count < 1000) {
        return 0.5;  // 50% do tamanho original
    } else {
        return 0.4;  // 40% do tamanho original para contextos longos
    }
}
