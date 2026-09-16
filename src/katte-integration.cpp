// katte-integration.cpp - Implementação da integração Katte no llama-server
#include "katte-integration.h"
#include "common.h"
#include "llama.h"
#include "log.h"

#include <chrono>
#include <algorithm>
#include <sched.h>
#include <pthread.h>

namespace katte_utils {

uint32_t hash_tool_definitions(const json& tools) {
    if (tools.is_null() || !tools.is_array()) return 0;
    
    std::string tools_str = tools.dump();
    return XXH3_64bits(tools_str.data(), tools_str.size()) & 0xFFFFFFFF;
}

std::string extract_conversation_id(const json& request) {
    // Priorizar conversation_id explícito
    if (request.contains("conversation_id")) {
        return request.value("conversation_id", "");
    }
    
    // Fallback para user_id + timestamp hash
    if (request.contains("user")) {
        return request.value("user", "default");
    }
    
    // Gerar ID único baseado em timestamp
    auto now = std::chrono::steady_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return "conv_" + std::to_string(ts);
}

uint64_t compute_context_revision(
    const std::vector<llama_token>& tokens,
    const std::string& system_prompt) {
    
    // Hash combinado dos tokens + system prompt
    uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a offset basis
    
    for (const auto& token : tokens) {
        hash ^= static_cast<uint64_t>(token);
        hash *= 0x100000001b3ULL; // FNV-1a prime
    }
    
    for (char c : system_prompt) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 0x100000001b3ULL;
    }
    
    return hash;
}

float estimate_ttft_savings(int32_t cached_tokens, int32_t total_tokens) {
    if (total_tokens <= 0 || cached_tokens <= 0) return 0.0f;
    
    // TTFT é proporcional ao número de tokens no prefill
    // Cache hit evita reprocessamento de cached_tokens
    
    // Considerar overhead de carregamento do cache (~5ms para SSD NVMe)
    float load_overhead_tokens = 50; // Equivalente a ~5ms em CPU
    float effective_cached = std::max(0, cached_tokens - static_cast<int32_t>(load_overhead_tokens));
    
    return std::min(0.95f, static_cast<float>(effective_cached) / total_tokens);
}

} // namespace katte_utils

katte_integrator::katte_integrator(const katte_config& config)
    : config_(config) {
    
    // Inicializar estatísticas
    stats_.total_requests = 0;
    stats_.cache_hits = 0;
    stats_.cache_misses = 0;
    stats_.hit_rate = 0.0f;
    stats_.tokens_reused = 0;
    stats_.tokens_computed = 0;
    stats_.avg_ttft_reduction = 0.0f;
    stats_.ram_usage = 0;
    stats_.disk_usage = 0;
    stats_.avg_compression_ratio = 0.0f;
    stats_.speculative_accepted = 0;
    stats_.speculative_total = 0;
    stats_.speculation_accept_rate = 0.0f;
    
    // Detectar topologia AMD EPYC para thread affinity
    // AMD EPYC 9645 tem 12 CCDs com 8 cores cada
    // Para 4 vcores dedicados, idealmente distribuir em 1-2 CCDs
    cpu_affinity_ = {0, 1, 2, 3}; // Default: primeiros 4 cores
}

katte_integrator::~katte_integrator() {
    // Flush final e cleanup
    if (kv_cache_) {
        trigger_garbage_collection();
    }
}

bool katte_integrator::initialize(const llama_model* model, const std::string& system_prompt) {
    if (!model) {
        LOG_ERR("Model is null, cannot initialize Katte\n");
        return false;
    }
    
    model_ = model;
    system_prompt_ = system_prompt;
    
    // Inicializar KV cache persistente
    kv_cache_ = std::make_unique<katte::PersistentKVCache>(
        config_.cache_dir,
        config_.max_ram_mb,
        config_.max_disk_gb
    );
    
    // Otimizar para arquitetura AMD EPYC
    optimize_for_epyc(nullptr);
    
    initialized_.store(true);
    LOG_INF("Katte initialized: cache_dir=%s, max_ram=%zuMB, max_disk=%zuGB\n",
            config_.cache_dir.c_str(), config_.max_ram_mb, config_.max_disk_gb);
    
    return true;
}

int32_t katte_integrator::pre_prefill_optimization(
    const std::string& conversation_id,
    uint64_t revision,
    llama_context* ctx,
    const std::vector<llama_token>& input_tokens,
    int32_t& n_past) {
    
    if (!initialized_.load() || !ctx || input_tokens.empty()) {
        return 0;
    }
    
    stats_.total_requests++;
    
    // Computar fingerprint do contexto
    auto fingerprint = compute_context_fingerprint(
        model_, system_prompt_, input_tokens);
    
    // Tentar carregar cache
    int32_t n_tokens_loaded = 0;
    bool cache_hit = kv_cache_->load_kv_state(
        conversation_id, revision, ctx, fingerprint, n_tokens_loaded);
    
    if (cache_hit && n_tokens_loaded > 0) {
        stats_.cache_hits++;
        stats_.tokens_reused += n_tokens_loaded;
        
        // Atualizar n_past para pular tokens já processados
        n_past = n_tokens_loaded;
        
        // Calcular economia estimada de TTFT
        float savings = katte_utils::estimate_ttft_savings(
            n_tokens_loaded, input_tokens.size());
        
        // Média móvel simples
        stats_.avg_ttft_reduction = 
            0.8f * stats_.avg_ttft_reduction + 0.2f * savings;
        
        LOG_DBG("Katte cache HIT: conv=%s, tokens_reused=%d, ttft_savings=%.1f%%\n",
                conversation_id.c_str(), n_tokens_loaded, savings * 100.0f);
        
        return n_tokens_loaded;
    } else {
        stats_.cache_misses++;
        stats_.tokens_computed += input_tokens.size();
        
        // Atualizar hit rate
        if (stats_.total_requests > 0) {
            stats_.hit_rate = static_cast<float>(stats_.cache_hits) / 
                             stats_.total_requests;
        }
        
        n_past = 0;
        LOG_DBG("Katte cache MISS: conv=%s, computing %zu tokens\n",
                conversation_id.c_str(), input_tokens.size());
        
        return 0;
    }
}

void katte_integrator::post_prefill_optimization(
    const std::string& conversation_id,
    uint64_t revision,
    llama_context* ctx,
    const std::vector<llama_token>& processed_tokens) {
    
    if (!initialized_.load() || !ctx || processed_tokens.empty()) {
        return;
    }
    
    // Computar fingerprint atualizado
    auto fingerprint = compute_context_fingerprint(
        model_, system_prompt_, processed_tokens);
    
    // Salvar estado KV no cache
    bool saved = kv_cache_->save_kv_state(
        conversation_id, revision, ctx, fingerprint, processed_tokens.size());
    
    if (saved) {
        LOG_DBG("Katte cache SAVED: conv=%s, tokens=%zu\n",
                conversation_id.c_str(), processed_tokens.size());
        
        // Atualizar sessão
        auto it = sessions_.find(conversation_id);
        if (it != sessions_.end()) {
            it->second->revision = revision;
            it->second->token_count = processed_tokens.size();
            it->second->last_access = std::chrono::steady_clock::now();
            it->second->fingerprint = fingerprint;
        }
    }
}

void katte_integrator::during_generation_optimization(
    llama_context* ctx,
    int32_t current_token,
    bool is_eos) {
    
    if (!initialized_.load() || !ctx) {
        return;
    }
    
    // Otimizações durante geração:
    // 1. Speculative decoding (se habilitado)
    // 2. Monitoramento de padrões para prediction store
    // 3. Adaptive token dropping
    
    if (config_.speculative_decoding) {
        // Implementação de speculative decoding
        // Draft model gera tokens candidatos
        // Modelo principal verifica
        
        stats_.speculative_total++;
        // Se aceito: stats_.speculative_accepted++
    }
    
    // Se EOS, marcar sessão como inactive para possível cleanup
    if (is_eos) {
        // Trigger garbage collection assíncrono se necessário
    }
}

std::shared_ptr<katte_session> katte_integrator::get_or_create_session(
    const std::string& conversation_id,
    const std::string& user_id) {
    
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(conversation_id);
    if (it != sessions_.end()) {
        it->second->last_access = std::chrono::steady_clock::now();
        it->second->is_active = true;
        return it->second;
    }
    
    // Criar nova sessão
    auto session = std::make_shared<katte_session>();
    session->conversation_id = conversation_id;
    session->user_id = user_id;
    session->revision = 0;
    session->created_at = std::chrono::steady_clock::now();
    session->last_access = session->created_at;
    session->token_count = 0;
    session->is_active = true;
    
    sessions_[conversation_id] = session;
    
    LOG_DBG("Katte session created: conv=%s, user=%s\n",
            conversation_id.c_str(), user_id.c_str());
    
    return session;
}

void katte_integrator::update_session_revision(
    const std::string& conversation_id, 
    uint64_t new_revision) {
    
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(conversation_id);
    if (it != sessions_.end()) {
        it->second->revision = new_revision;
        it->second->last_access = std::chrono::steady_clock::now();
    }
}

void katte_integrator::mark_session_inactive(const std::string& conversation_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(conversation_id);
    if (it != sessions_.end()) {
        it->second->is_active = false;
        it->second->last_access = std::chrono::steady_clock::now();
    }
}

katte_integrator::katte_stats katte_integrator::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    katte_stats result = stats_;
    
    // Adicionar estatísticas do KV cache
    if (kv_cache_) {
        auto cache_stats = kv_cache_->get_stats();
        result.ram_usage = cache_stats.ram_usage;
        result.disk_usage = cache_stats.disk_usage;
        result.avg_compression_ratio = cache_stats.avg_compression_ratio;
    }
    
    return result;
}

std::string katte_integrator::get_stats_json() const {
    auto stats = get_stats();
    
    json j;
    j["enabled"] = config_.enabled;
    j["total_requests"] = stats.total_requests;
    j["cache_hits"] = stats.cache_hits;
    j["cache_misses"] = stats.cache_misses;
    j["hit_rate"] = stats.hit_rate;
    j["tokens_reused"] = stats.tokens_reused;
    j["tokens_computed"] = stats.tokens_computed;
    j["avg_ttft_reduction_percent"] = stats.avg_ttft_reduction * 100.0f;
    j["ram_usage_bytes"] = stats.ram_usage;
    j["disk_usage_bytes"] = stats.disk_usage;
    j["avg_compression_ratio"] = stats.avg_compression_ratio;
    j["speculative_accepted"] = stats.speculative_accepted;
    j["speculative_total"] = stats.speculative_total;
    j["speculation_accept_rate"] = stats.speculation_accept_rate;
    
    return j.dump(2);
}

void katte_integrator::update_speculation_policy(float accept_rate) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.speculation_accept_rate = accept_rate;
    
    // Política adaptativa: ajustar número de draft tokens
    if (accept_rate < config_.speculation_threshold && config_.draft_tokens > 4) {
        config_.draft_tokens -= 2;
        LOG_INF("Katte: reducing draft tokens to %d (accept_rate=%.2f)\n",
                config_.draft_tokens, accept_rate);
    } else if (accept_rate > 0.8f && config_.draft_tokens < 32) {
        config_.draft_tokens += 2;
        LOG_INF("Katte: increasing draft tokens to %d (accept_rate=%.2f)\n",
                config_.draft_tokens, accept_rate);
    }
}

void katte_integrator::trigger_garbage_collection() {
    if (kv_cache_) {
        kv_cache_->garbage_collect();
        LOG_INF("Katte: garbage collection triggered\n");
    }
}

void katte_integrator::warmup_conversations(
    const std::vector<std::string>& conversation_ids) {
    
    if (kv_cache_) {
        kv_cache_->warmup_cache(conversation_ids);
        LOG_INF("Katte: warming up %zu conversations\n", conversation_ids.size());
    }
}

katte::ContextFingerprint katte_integrator::compute_context_fingerprint(
    const llama_model* model,
    const std::string& system_prompt,
    const std::vector<llama_token>& /*tokens*/,
    uint32_t tool_hash) {
    
    katte::ContextFingerprint fp;
    memset(&fp, 0, sizeof(fp));
    
    // Hash do modelo (usar metadata GGUF)
    // Nota: implementação simplificada - idealmente usar hash real do arquivo
    
    // Configurações do modelo usando API moderna do llama.cpp
    fp.vocab_size = llama_model_vocab_size(model);
    fp.n_embd = llama_model_n_embd(model);
    fp.n_head = llama_model_n_head(model);
    fp.n_layer = llama_model_n_layer(model);
    
    // Hash do system prompt com XXH3_64bits
    if (!system_prompt.empty()) {
        uint64_t prompt_hash = XXH3_64bits(system_prompt.data(), system_prompt.size());
        memcpy(fp.system_prompt_hash, &prompt_hash, sizeof(prompt_hash));
    }
    
    // Tool hash
    fp.tool_hash = tool_hash;
    
    // Config hash (incluir tokenizer, chat template, etc.)
    // Implementação futura
    
    return fp;
}

void katte_integrator::optimize_for_epyc(llama_context* ctx) {
    // AMD EPYC 9645: 96 cores / 192 threads, 12 CCDs
    // Cada CCD tem 8 cores Zen5
    
    // Para 4 vcores dedicados:
    // Estratégia 1: Manter todos no mesmo CCD para menor latência L3
    // Estratégia 2: Distribuir em 2 CCDs para melhor throughput
    
    // Fixar thread affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    // Usar primeiros 4 cores (assumindo que são dedicados)
    for (int core : cpu_affinity_) {
        CPU_SET(core, &cpuset);
    }
    
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    
    LOG_INF("Katte: optimized for AMD EPYC with %zu cores affinity\n",
            cpu_affinity_.size());
    
    // Se ctx não for null, ajustar parâmetros do llama_context
    if (ctx) {
        // Ajustar batch size, ubatch, threads
        // Implementação específica do llama.cpp
    }
}

void katte_integrator::distribute_threads_across_ccx(int n_requests) {
    // Distribuir threads de requisições múltiplas através de CCX/CCDs
    // para maximizar paralelismo e minimizar contenção
    
    // AMD EPYC 9645: 12 CCDs com 8 cores cada
    // Ideal: 1 requisição por CCD para conversas longas
    
    if (n_requests <= 0) return;
    
    // Implementação de load balancing baseada em topologia
    // Futura melhoria: usar libhwloc para detecção automática
}
