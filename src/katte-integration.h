// katte-integration.h - Integração do Katte KV Cache no llama-server
#pragma once

#include "katte-kv-cache.h"
#include "server-task.h"
#include "llama.h"

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>

// Configuração do Katte para o servidor
struct katte_config {
    bool enabled = true;
    std::string cache_dir = "./katte-cache";
    size_t max_ram_mb = 2048;      // RAM máxima para hot cache
    size_t max_disk_gb = 100;      // Disco máximo para cold cache
    bool compression_enabled = true;
    bool mmap_enabled = true;
    float compression_threshold = 0.7f;  // Ratio mínimo para compressão
    
    // Políticas de otimização
    bool speculative_decoding = false;
    int draft_tokens = 16;
    float speculation_threshold = 0.6f;
    
    // Context compression
    bool context_compression = false;
    int compression_window = 4096;  // Tokens antes de comprimir
};

// Gerenciador de sessões por usuário/conversa
struct katte_session {
    std::string conversation_id;
    uint64_t revision;
    std::string user_id;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_access;
    int32_t token_count;
    bool is_active;
    
    // Fingerprint do contexto atual
    katte::ContextFingerprint fingerprint;
};

// Integrador principal do Katte no llama-server
class katte_integrator {
public:
    katte_integrator(const katte_config& config);
    ~katte_integrator();
    
    // Inicializar sistema de cache
    bool initialize(const llama_model* model, const std::string& system_prompt);
    
    // Antes do prefill: tentar restaurar cache
    // Retorna número de tokens restaurados (0 se miss)
    int32_t pre_prefill_optimization(
        const std::string& conversation_id,
        uint64_t revision,
        llama_context* ctx,
        const std::vector<llama_token>& input_tokens,
        int32_t& n_past  // tokens já processados via cache
    );
    
    // Após o prefill: salvar estado KV
    void post_prefill_optimization(
        const std::string& conversation_id,
        uint64_t revision,
        llama_context* ctx,
        const std::vector<llama_token>& processed_tokens
    );
    
    // Durante geração: otimizações em tempo real
    void during_generation_optimization(
        llama_context* ctx,
        int32_t current_token,
        bool is_eos
    );
    
    // Gerenciar sessão de conversa
    std::shared_ptr<katte_session> get_or_create_session(
        const std::string& conversation_id,
        const std::string& user_id
    );
    
    void update_session_revision(const std::string& conversation_id, uint64_t new_revision);
    void mark_session_inactive(const std::string& conversation_id);
    
    // Estatísticas em tempo real
    struct katte_stats {
        uint64_t total_requests;
        uint64_t cache_hits;
        uint64_t cache_misses;
        float hit_rate;
        uint64_t tokens_reused;
        uint64_t tokens_computed;
        float avg_ttft_reduction;  // Redução média no TTFT
        size_t ram_usage;
        size_t disk_usage;
        float avg_compression_ratio;
        uint64_t speculative_accepted;
        uint64_t speculative_total;
        float speculation_accept_rate;
    };
    
    katte_stats get_stats() const;
    std::string get_stats_json() const;
    
    // Controle de políticas adaptativas
    void update_speculation_policy(float accept_rate);
    void trigger_garbage_collection();
    void warmup_conversations(const std::vector<std::string>& conversation_ids);
    
    // Obter configuração
    const katte_config& get_config() const { return config_; }
    
private:
    // Computar fingerprint do contexto
    katte::ContextFingerprint compute_context_fingerprint(
        const llama_model* model,
        const std::string& system_prompt,
        const std::vector<llama_token>& tokens,
        uint32_t tool_hash = 0
    );
    
    // Otimizações específicas para CPU AMD EPYC
    void optimize_for_epyc(llama_context* ctx);
    
    // Balanceamento de carga para múltiplas requisições
    void distribute_threads_across_ccx(int n_requests);
    
    katte_config config_;
    std::unique_ptr<katte::PersistentKVCache> kv_cache_;
    
    // Sessões ativas
    std::unordered_map<std::string, std::shared_ptr<katte_session>> sessions_;
    std::mutex sessions_mutex_;
    
    // Estatísticas
    mutable std::mutex stats_mutex_;
    katte_stats stats_;
    
    // Modelo e configurações
    const llama_model* model_ = nullptr;
    std::string system_prompt_;
    std::atomic<bool> initialized_{false};
    
    // Thread affinity para AMD EPYC (4 vcores dedicados)
    std::vector<int> cpu_affinity_;
};

// Funções utilitárias para integração
namespace katte_utils {
    // Hash rápido para tool definitions
    uint32_t hash_tool_definitions(const json& tools);
    
    // Extrair conversation_id de request OAI
    std::string extract_conversation_id(const json& request);
    
    // Calcular revisão do contexto
    uint64_t compute_context_revision(
        const std::vector<llama_token>& tokens,
        const std::string& system_prompt
    );
    
    // Estimar economia de tempo com cache hit
    float estimate_ttft_savings(int32_t cached_tokens, int32_t total_tokens);
}
