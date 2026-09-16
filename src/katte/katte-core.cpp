// Katte Core Implementation
// Implementação das otimizações extremas para CPU/GPU

#include "katte-core.h"
#include <cstring>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>

#ifdef __linux__
    #include <sched.h>
    #include <unistd.h>
#endif

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
#endif

// ============================================================================
// katte_memory_pool Implementation
// ============================================================================

katte_memory_pool::katte_memory_pool(size_t initial_size, size_t max_size)
    : total_(initial_size), used_(0), max_size_(max_size), free_list_(nullptr)
{
    // Aloca arena inicial com alignment
    #ifdef _WIN32
        arena_ = (uint8_t*)_aligned_malloc(initial_size, KATTE_MEMORY_POOL_ALIGNMENT);
    #else
        posix_memalign((void**)&arena_, KATTE_MEMORY_POOL_ALIGNMENT, initial_size);
    #endif
    
    if (!arena_) {
        throw std::bad_alloc();
    }
    
    // Inicializa free list com bloco único
    free_list_ = reinterpret_cast<free_block*>(arena_);
    free_list_->size = initial_size;
    free_list_->next = nullptr;
}

katte_memory_pool::~katte_memory_pool() {
    #ifdef _WIN32
        _aligned_free(arena_);
    #else
        free(arena_);
    #endif
}

void* katte_memory_pool::allocate(size_t size, size_t alignment) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // Alinha tamanho
    size = katte::align_up(size, alignment);
    
    // Busca na free list (first-fit)
    free_block** current = &free_list_;
    while (*current) {
        if ((*current)->size >= size) {
            // Bloco encontrado
            free_block* block = *current;
            
            if (block->size > size + sizeof(free_block)) {
                // Divide bloco se houver espaço suficiente
                free_block* new_block = reinterpret_cast<free_block*>(
                    reinterpret_cast<uint8_t*>(block) + size
                );
                new_block->size = block->size - size;
                new_block->next = block->next;
                
                *current = new_block;
            } else {
                // Usa bloco inteiro
                *current = block->next;
            }
            
            used_ += size;
            return block;
        }
        current = &(*current)->next;
    }
    
    // Não encontrou na free list - tenta expandir arena
    if (used_ + size <= max_size_) {
        // Nota: em produção, faria realloc da arena
        // Por simplicidade, retorna nullptr aqui
        return nullptr;
    }
    
    return nullptr;  // Sem memória disponível
}

void katte_memory_pool::deallocate(void* ptr, size_t size) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    if (!ptr) return;
    
    // Adiciona de volta à free list
    free_block* block = reinterpret_cast<free_block*>(ptr);
    block->size = size;
    block->next = free_list_;
    free_list_ = block;
    
    used_ -= size;
}

void katte_memory_pool::reset() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // Reseta free list para estado inicial
    free_list_ = reinterpret_cast<free_block*>(arena_);
    free_list_->size = total_;
    free_list_->next = nullptr;
    
    used_ = 0;
}

// ============================================================================
// katte_token_compressor Implementation
// ============================================================================

struct katte_token_compressor::impl {
    // Tabela Huffman simples para tokens frequentes
    std::vector<uint16_t> codes;
    std::vector<uint8_t> lengths;
    
    impl() {
        // Inicializa tabela Huffman com distribuição típica de tokens LLM
        codes.resize(32000, 0);
        lengths.resize(32000, 8);
        
        // Tokens mais comuns recebem códigos mais curtos
        for (uint32_t i = 0; i < 256; ++i) {
            lengths[i] = 6;  // Bytes individuais
        }
        for (uint32_t i = 256; i < 1024; ++i) {
            lengths[i] = 8;  // Tokens frequentes
        }
        // Restante usa 10-12 bits
    }
};

katte_token_compressor::katte_token_compressor() 
    : pimpl_(std::make_unique<impl>()) 
{
}

katte_token_compressor::~katte_token_compressor() = default;

size_t katte_token_compressor::compress(
    const uint32_t* tokens,
    size_t token_count,
    uint8_t* output_buffer,
    size_t buffer_size
) const {
    if (!tokens || !output_buffer || token_count == 0) {
        return 0;
    }
    
    // Compressão simples: empacota tokens em bits variáveis
    // Em produção, usaria Huffman completo ou ANS
    
    size_t output_pos = 0;
    uint64_t bit_buffer = 0;
    int bit_count = 0;
    
    for (size_t i = 0; i < token_count; ++i) {
        uint32_t token = tokens[i];
        uint8_t code_len = pimpl_->lengths[token];
        uint16_t code = pimpl_->codes[token];
        
        // Adiciona código ao buffer de bits
        bit_buffer |= (static_cast<uint64_t>(code) << bit_count);
        bit_count += code_len;
        
        // Escreve bytes completos
        while (bit_count >= 8) {
            if (output_pos >= buffer_size) {
                return 0;  // Buffer cheio
            }
            output_buffer[output_pos++] = static_cast<uint8_t>(bit_buffer & 0xFF);
            bit_buffer >>= 8;
            bit_count -= 8;
        }
    }
    
    // Escreve bits restantes
    if (bit_count > 0 && output_pos < buffer_size) {
        output_buffer[output_pos++] = static_cast<uint8_t>(bit_buffer & 0xFF);
    }
    
    // Adiciona header com contagem de tokens
    // (em produção, isso seria parte do formato)
    
    return output_pos;
}

size_t katte_token_compressor::decompress(
    const uint8_t* compressed_data,
    size_t compressed_size,
    uint32_t* output_tokens,
    size_t max_tokens
) const {
    if (!compressed_data || !output_tokens || compressed_size == 0) {
        return 0;
    }
    
    // Descompressão inversa
    size_t input_pos = 0;
    uint64_t bit_buffer = 0;
    int bit_count = 0;
    size_t token_count = 0;
    
    while (input_pos < compressed_size && token_count < max_tokens) {
        // Enche buffer de bits
        while (bit_count < 16 && input_pos < compressed_size) {
            bit_buffer |= (static_cast<uint64_t>(compressed_data[input_pos++]) << bit_count);
            bit_count += 8;
        }
        
        // Decodifica token (busca reversa na tabela Huffman)
        // Simplificado: assume token direto nos bits inferiores
        uint32_t token = static_cast<uint32_t>(bit_buffer & 0xFFFF);
        uint8_t code_len = pimpl_->lengths[token];
        
        output_tokens[token_count++] = token;
        bit_buffer >>= code_len;
        bit_count -= code_len;
    }
    
    return token_count;
}

float katte_token_compressor::estimate_ratio(
    size_t token_count,
    const uint32_t* tokens
) const {
    if (!tokens || token_count == 0) {
        return 1.0f;
    }
    
    // Estima ratio baseado na distribuição de tokens
    float total_bits = 0;
    for (size_t i = 0; i < token_count; ++i) {
        total_bits += pimpl_->lengths[tokens[i]];
    }
    
    float original_bits = token_count * 32;  // 32 bits por token original
    return total_bits / original_bits;
}

// ============================================================================
// katte_speculative_engine Implementation
// ============================================================================

katte_speculative_engine::katte_speculative_engine(
    const katte_speculative_config& config
) : config_(config), total_drafted_(0), total_accepted_(0)
{
}

katte_speculative_engine::~katte_speculative_engine() = default;

std::vector<uint32_t> katte_speculative_engine::generate_draft(
    const uint32_t* context_tokens,
    size_t context_size,
    uint32_t max_tokens
) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    std::vector<uint32_t> draft;
    draft.reserve(std::min(max_tokens, config_.max_draft_tokens));
    
    if (!config_.enable_ngram_prediction || !context_tokens || context_size == 0) {
        return draft;
    }
    
    // Gera tokens via n-gram prediction simples
    uint32_t max_draft = std::min(max_tokens, config_.max_draft_tokens);
    
    for (uint32_t i = 0; i < max_draft; ++i) {
        // Busca n-gram mais recente no histórico
        uint32_t predicted_token = 0;
        
        if (context_size >= 3) {
            // Bigram prediction
            uint32_t key = context_tokens[context_size - 1];
            
            // Busca no histórico de n-grams
            for (const auto& ngram : ngram_history_) {
                if (ngram[0] == key) {
                    predicted_token = ngram[1];
                    break;
                }
            }
        }
        
        if (predicted_token == 0) {
            // Fallback: usa token mais frequente ou último token
            predicted_token = context_tokens[context_size - 1];
        }
        
        draft.push_back(predicted_token);
        
        // Atualiza contexto para próxima iteração
        if (ngram_history_.size() >= 1000) {
            ngram_history_.erase(ngram_history_.begin());
        }
        ngram_history_.push_back({context_tokens[context_size - 1], predicted_token, 0});
    }
    
    return draft;
}

void katte_speculative_engine::record_verification(
    const std::vector<uint32_t>& drafted,
    const std::vector<uint32_t>& accepted
) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    total_drafted_ += drafted.size();
    total_accepted_ += accepted.size();
    
    // Calcula taxa de aceitação recente
    if (drafted.size() > 0) {
        float rate = static_cast<float>(accepted.size()) / drafted.size();
        recent_acceptance_rates_.push_back(rate);
        
        // Mantém janela deslizante
        if (recent_acceptance_rates_.size() > config_.adaptation_window) {
            recent_acceptance_rates_.erase(recent_acceptance_rates_.begin());
        }
    }
}

float katte_speculative_engine::get_acceptance_rate() const {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    if (total_drafted_ == 0) {
        return 0.0f;
    }
    
    return static_cast<float>(total_accepted_) / total_drafted_;
}

void katte_speculative_engine::adapt_policy() {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    
    if (recent_acceptance_rates_.empty()) {
        return;
    }
    
    // Calcula média móvel
    float avg_rate = std::accumulate(
        recent_acceptance_rates_.begin(),
        recent_acceptance_rates_.end(),
        0.0f
    ) / recent_acceptance_rates_.size();
    
    // Ajusta max_draft_tokens baseado na taxa de aceitação
    if (avg_rate < config_.min_acceptance_rate) {
        // Reduz especulação se aceitação estiver baixa
        config_.max_draft_tokens = std::max(2u, config_.max_draft_tokens - 2);
    } else if (avg_rate > 0.5f && config_.max_draft_tokens < KATTE_SPECULATIVE_MAX_TOKENS) {
        // Aumenta especulação se aceitação estiver alta
        config_.max_draft_tokens += 2;
    }
}

// ============================================================================
// katte_thread_pool Implementation
// ============================================================================

katte_thread_pool::katte_thread_pool(uint32_t num_threads, bool use_affinity)
    : shutdown_(false), active_tasks_(0)
{
    if (num_threads == 0) {
        num_threads = katte::detect_optimal_thread_count();
    }
    
    threads_.reserve(num_threads);
    
    for (uint32_t i = 0; i < num_threads; ++i) {
        threads_.emplace_back(&katte_thread_pool::worker_loop, this);
        
        if (use_affinity) {
            set_thread_affinity(threads_.back(), i % katte::detect_optimal_thread_count());
        }
    }
}

katte_thread_pool::~katte_thread_pool() {
    shutdown_ = true;
    condition_.notify_all();
    
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void katte_thread_pool::wait_all() {
    while (active_tasks_ > 0 || !task_queue_.empty()) {
        std::this_thread::yield();
    }
}

void katte_thread_pool::set_thread_affinity(std::thread& t, int core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    pthread_setaffinity_np(
        t.native_handle(),
        sizeof(cpu_set_t),
        &cpuset
    );
#elif defined(_WIN32)
    HANDLE handle = reinterpret_cast<HANDLE>(t.native_handle());
    SetThreadAffinityMask(handle, 1ULL << core_id);
#endif
}

void katte_thread_pool::worker_loop() {
    while (!shutdown_) {
        std::function<void()> task;
        
        if (task_queue_.pop(task)) {
            active_tasks_++;
            task();
            active_tasks_--;
        } else {
            std::this_thread::yield();
        }
    }
}

// ============================================================================
// katte_request_scheduler Implementation
// ============================================================================

katte_request_scheduler::katte_request_scheduler()
    : total_enqueued_(0), total_dequeued_(0)
{
}

katte_request_scheduler::~katte_request_scheduler() = default;

void katte_request_scheduler::enqueue(katte_request_context ctx) {
    std::lock_guard<std::mutex> lock(queues_.mutex_);
    
    int priority_idx = static_cast<int>(ctx.priority);
    queues_.queues[priority_idx].push_back(ctx);
    
    total_enqueued_++;
}

bool katte_request_scheduler::dequeue(katte_request_context& out_ctx) {
    std::lock_guard<std::mutex> lock(queues_.mutex_);
    
    // Processa por prioridade (CRITICAL > HIGH > NORMAL > LOW)
    for (int i = 3; i >= 0; --i) {
        if (!queues_.queues[i].empty()) {
            out_ctx = queues_.queues[i].front();
            queues_.queues[i].erase(queues_.queues[i].begin());
            total_dequeued_++;
            return true;
        }
    }
    
    return false;
}

void katte_request_scheduler::cancel(uint64_t request_id) {
    std::lock_guard<std::mutex> lock(queues_.mutex_);
    
    for (int i = 0; i < 4; ++i) {
        auto& queue = queues_.queues[i];
        queue.erase(
            std::remove_if(queue.begin(), queue.end(),
                [request_id](const katte_request_context& ctx) {
                    return ctx.request_id == request_id;
                }),
            queue.end()
        );
    }
}

size_t katte_request_scheduler::pending_count() const {
    std::lock_guard<std::mutex> lock(queues_.mutex_);
    
    size_t total = 0;
    for (int i = 0; i < 4; ++i) {
        total += queues_.queues[i].size();
    }
    return total;
}

katte_request_scheduler::queue_stats katte_request_scheduler::get_stats() const {
    std::lock_guard<std::mutex> lock(queues_.mutex_);
    
    queue_stats stats;
    stats.low_priority = queues_.queues[0].size();
    stats.normal_priority = queues_.queues[1].size();
    stats.high_priority = queues_.queues[2].size();
    stats.critical_priority = queues_.queues[3].size();
    stats.avg_wait_time_ms = 0.0;  // Calcular baseado em timestamps
    
    return stats;
}

// ============================================================================
// katte_metrics_collector Implementation
// ============================================================================

katte_metrics_collector::katte_metrics_collector()
    : cache_hits_(0), cache_misses_(0), speculative_accepted_(0), speculative_total_(0),
      last_ram_gb_(0), last_cpu_percent_(0), last_gpu_percent_(0), last_gpu_memory_gb_(0)
{
}

katte_metrics_collector::~katte_metrics_collector() = default;

void katte_metrics_collector::record_ttft(double ms) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ttft_samples_.push_back(ms);
    
    // Limita amostras para evitar crescimento infinito
    if (ttft_samples_.size() > 10000) {
        ttft_samples_.erase(ttft_samples_.begin(), ttft_samples_.begin() + 5000);
    }
}

void katte_metrics_collector::record_token_generation(double ms_per_token) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    token_gen_samples_.push_back(ms_per_token);
    
    if (token_gen_samples_.size() > 10000) {
        token_gen_samples_.erase(token_gen_samples_.begin(), token_gen_samples_.begin() + 5000);
    }
}

void katte_metrics_collector::record_resource_usage(
    double ram_gb, double cpu_percent, double gpu_percent, double gpu_memory_gb
) {
    last_ram_gb_ = ram_gb;
    last_cpu_percent_ = cpu_percent;
    last_gpu_percent_ = gpu_percent;
    last_gpu_memory_gb_ = gpu_memory_gb;
}

void katte_metrics_collector::record_cache_hit() {
    cache_hits_++;
}

void katte_metrics_collector::record_cache_miss() {
    cache_misses_++;
}

void katte_metrics_collector::record_speculative_accept(uint32_t accepted, uint32_t total) {
    speculative_accepted_ += accepted;
    speculative_total_ += total;
}

katte_performance_metrics katte_metrics_collector::get_metrics() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    katte_performance_metrics metrics;
    
    // Calcula percentis de TTFT
    if (!ttft_samples_.empty()) {
        std::vector<double> sorted = ttft_samples_;
        std::sort(sorted.begin(), sorted.end());
        
        metrics.avg_ttft_ms = std::accumulate(sorted.begin(), sorted.end(), 0.0) / sorted.size();
        metrics.p50_ttft_ms = sorted[sorted.size() / 2];
        metrics.p95_ttft_ms = sorted[sorted.size() * 95 / 100];
        metrics.p99_ttft_ms = sorted[sorted.size() * 99 / 100];
    } else {
        metrics.avg_ttft_ms = metrics.p50_ttft_ms = metrics.p95_ttft_ms = metrics.p99_ttft_ms = 0;
    }
    
    // Throughput
    if (!token_gen_samples_.empty()) {
        double avg_ms = std::accumulate(token_gen_samples_.begin(), token_gen_samples_.end(), 0.0) / token_gen_samples_.size();
        metrics.tokens_per_second = (avg_ms > 0) ? (1000.0 / avg_ms) : 0;
    } else {
        metrics.tokens_per_second = 0;
    }
    
    metrics.requests_per_second = 0;  // Calcular baseado em window temporal
    
    // Recursos
    metrics.ram_usage_gb = last_ram_gb_;
    metrics.cpu_utilization_percent = last_cpu_percent_;
    metrics.gpu_utilization_percent = last_gpu_percent_;
    metrics.gpu_memory_gb = last_gpu_memory_gb_;
    
    // Cache
    uint64_t hits = cache_hits_.load();
    uint64_t misses = cache_misses_.load();
    metrics.kv_cache_hit_rate = (hits + misses > 0) ? (double)hits / (hits + misses) : 0;
    
    uint32_t accepted = speculative_accepted_.load();
    uint32_t total = speculative_total_.load();
    metrics.speculative_acceptance_rate = (total > 0) ? (float)accepted / total : 0;
    
    metrics.compression_ratio = KATTE_COMPRESSION_RATIO_TARGET;  // Estimativa
    
    // Erros
    metrics.total_requests = hits + misses;
    metrics.failed_requests = 0;
    metrics.cache_misses = misses;
    
    return metrics;
}

std::string katte_metrics_collector::to_json() const {
    auto m = get_metrics();
    
    char buffer[1024];
    snprintf(buffer, sizeof(buffer),
        "{\"ttft_avg_ms\":%.2f,\"ttft_p50_ms\":%.2f,\"ttft_p95_ms\":%.2f,"
        "\"tokens_per_sec\":%.2f,\"ram_gb\":%.2f,\"cache_hit_rate\":%.3f}",
        m.avg_ttft_ms, m.p50_ttft_ms, m.p95_ttft_ms,
        m.tokens_per_second, m.ram_usage_gb, m.kv_cache_hit_rate
    );
    
    return std::string(buffer);
}

void katte_metrics_collector::reset() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    ttft_samples_.clear();
    token_gen_samples_.clear();
    cache_hits_ = 0;
    cache_misses_ = 0;
    speculative_accepted_ = 0;
    speculative_total_ = 0;
}

// ============================================================================
// Utility Functions
// ============================================================================

namespace katte {
    uint32_t detect_optimal_thread_count() {
        uint32_t cores = std::thread::hardware_concurrency();
        
        if (cores == 0) {
            return 4;  // Default seguro
        }
        
        // Para CPUs com muitos cores, limita a um número razoável
        // para evitar overhead de sincronização excessivo
        return std::min(cores, 16u);
    }
    
    uint64_t now_us() {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    }
}
