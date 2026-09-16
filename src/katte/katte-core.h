#pragma once

// Katte Core: Otimizações extremas para CPU-only e CPU+GPU
// Foco: Mínimo uso de RAM, máximo throughput, TTFT ultrarápido

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <vector>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>

#ifdef GGML_USE_CUDA
#include <cuda_runtime.h>
typedef cudaStream_t katte_cuda_stream_t;
#else
// Fallback para CPU-only: usa ponteiro void como placeholder
typedef void* katte_cuda_stream_t;
#endif

// ============================================================================
// Configurações de Otimização Compile-Time
// ============================================================================

#define KATTE_CACHE_LINE_SIZE 64
#define KATTE_PREFETCH_DISTANCE 8
#define KATTE_MAX_CONCURRENT_REQUESTS 64
#define KATTE_HOT_CONTEXT_TOKENS 512
#define KATTE_COMPRESSION_RATIO_TARGET 0.65f
#define KATTE_SPECULATIVE_MAX_TOKENS 16
#define KATTE_SPECULATIVE_MIN_ACCEPTANCE 0.15f
#define KATTE_GPU_BATCH_SIZE_GTX 256
#define KATTE_MEMORY_POOL_ALIGNMENT 64

// ============================================================================
// Tipos Otimizados para Cache Alignment
// ============================================================================

template<typename T>
struct alignas(KATTE_CACHE_LINE_SIZE) katte_aligned_vector {
    std::vector<T> data;
    
    katte_aligned_vector() = default;
    explicit katte_aligned_vector(size_t n) : data(n) {
        // Garante alignment para SIMD
        if constexpr (alignof(T) < KATTE_CACHE_LINE_SIZE) {
            // Padding manual se necessário
        }
    }
    
    T* data_ptr() { return data.data(); }
    const T* data_ptr() const { return data.data(); }
    size_t size() const { return data.size(); }
    void resize(size_t n) { data.resize(n); }
};

// ============================================================================
// Memory Pool Ultra-Rápido com Arena Allocation
// ============================================================================

class katte_memory_pool {
public:
    explicit katte_memory_pool(
        size_t initial_size = 256ULL * 1024 * 1024,  // 256MB inicial
        size_t max_size = 4ULL * 1024 * 1024 * 1024   // 4GB máximo
    );
    
    ~katte_memory_pool();
    
    // Alocação ultra-rápida (O(1))
    void* allocate(size_t size, size_t alignment = KATTE_MEMORY_POOL_ALIGNMENT);
    
    // Dealocação (apenas reset em batch)
    void deallocate(void* ptr, size_t size);
    
    // Reset completo do pool
    void reset();
    
    // Estatísticas
    size_t used_bytes() const { return used_; }
    size_t total_bytes() const { return total_; }
    double utilization() const { return (double)used_ / total_; }
    
private:
    uint8_t* arena_;
    size_t total_;
    size_t used_;
    size_t max_size_;
    std::mutex pool_mutex_;
    
    // Free list para reutilização
    struct free_block {
        size_t size;
        free_block* next;
    };
    free_block* free_list_;
};

// ============================================================================
// Lock-Free Queue para Hot Path
// ============================================================================

template<typename T, size_t Capacity>
class katte_lockfree_queue {
public:
    katte_lockfree_queue() : head_(0), tail_(0) {}
    
    bool push(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) % Capacity;
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Queue cheia
        }
        
        buffer_[tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    bool pop(T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;  // Queue vazia
        }
        
        item = buffer_[head];
        head_.store((head + 1) % Capacity, std::memory_order_release);
        return true;
    }
    
    bool empty() const {
        return head_.load(std::memory_order_acquire) == 
               tail_.load(std::memory_order_acquire);
    }
    
    size_t size() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (tail >= head) ? (tail - head) : (Capacity - head + tail);
    }
    
private:
    std::array<T, Capacity> buffer_;
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;
    char padding_[KATTE_CACHE_LINE_SIZE];  // Evita false sharing
};

// ============================================================================
// Context Compressor Lossless para Tokens
// ============================================================================

class katte_token_compressor {
public:
    katte_token_compressor();
    ~katte_token_compressor();
    
    // Comprime sequência de tokens
    // Retorna tamanho em bytes do buffer comprimido
    size_t compress(
        const uint32_t* tokens,
        size_t token_count,
        uint8_t* output_buffer,
        size_t buffer_size
    ) const;
    
    // Descomprime sequência de tokens
    // Retorna número de tokens descomprimidos
    size_t decompress(
        const uint8_t* compressed_data,
        size_t compressed_size,
        uint32_t* output_tokens,
        size_t max_tokens
    ) const;
    
    // Estima ratio de compressão para dado contexto
    float estimate_ratio(size_t token_count, const uint32_t* tokens) const;
    
private:
    struct impl;
    std::unique_ptr<impl> pimpl_;
    
    // Huffman coding adaptado para distribuição de tokens LLM
    mutable std::vector<uint16_t> huffman_codes_;
    mutable std::vector<uint8_t> code_lengths_;
};

// ============================================================================
// Speculative Decoding com Policy Adaptativa
// ============================================================================

struct katte_speculative_config {
    uint32_t max_draft_tokens = KATTE_SPECULATIVE_MAX_TOKENS;
    float min_acceptance_rate = KATTE_SPECULATIVE_MIN_ACCEPTANCE;
    uint32_t adaptation_window = 64;  // Tokens para ajustar policy
    bool enable_ngram_prediction = true;
    bool enable_small_draft_model = false;
    std::string draft_model_path;
};

class katte_speculative_engine {
public:
    explicit katte_speculative_engine(const katte_speculative_config& config);
    ~katte_speculative_engine();
    
    // Gera tokens especulativos
    std::vector<uint32_t> generate_draft(
        const uint32_t* context_tokens,
        size_t context_size,
        uint32_t max_tokens
    );
    
    // Registra resultado da verificação (aceite/rejeição)
    void record_verification(
        const std::vector<uint32_t>& drafted,
        const std::vector<uint32_t>& accepted
    );
    
    // Obtém taxa de aceitação atual
    float get_acceptance_rate() const;
    
    // Ajusta policy baseado em desempenho recente
    void adapt_policy();
    
    // Configuração atual
    const katte_speculative_config& config() const { return config_; }
    
private:
    katte_speculative_config config_;
    mutable std::mutex engine_mutex_;
    
    // Estatísticas para adaptação
    std::vector<float> recent_acceptance_rates_;
    uint32_t total_drafted_;
    uint32_t total_accepted_;
    
    // N-gram predictor simples
    std::vector<std::array<uint32_t, 3>> ngram_history_;
};

// ============================================================================
// Thread Pool com Affinity para CPU
// ============================================================================

class katte_thread_pool {
public:
    explicit katte_thread_pool(
        uint32_t num_threads = 0,  // 0 = auto-detect
        bool use_affinity = true
    );
    
    ~katte_thread_pool();
    
    // Submete tarefa para execução
    template<typename F>
    void submit(F&& func) {
        task_queue_.push(std::forward<F>(func));
        condition_.notify_one();
    }
    
    // Aguarda conclusão de todas as tarefas
    void wait_all();
    
    // Número de threads
    uint32_t size() const { return threads_.size(); }
    
    // Define affinity de thread para núcleo específico
    static void set_thread_affinity(std::thread& t, int core_id);
    
private:
    std::vector<std::thread> threads_;
    katte_lockfree_queue<std::function<void()>, 1024> task_queue_;
    std::condition_variable condition_;
    std::atomic<bool> shutdown_;
    std::atomic<uint32_t> active_tasks_;
    
    void worker_loop();
};

// ============================================================================
// GPU Memory Manager Otimizado para GTX
// ============================================================================

class katte_gpu_manager {
public:
    katte_gpu_manager();
    ~katte_gpu_manager();
    
    // Inicializa GPU (CUDA para GTX)
    bool initialize(int device_id = 0);
    
    // Aloca memória na GPU com otimizações para GTX
    void* gpu_allocate(size_t size);
    
    // Libera memória da GPU
    void gpu_free(void* ptr);
    
    // Copy host->GPU com async e pinned memory
    void h2d_async(void* dst, const void* src, size_t size, katte_cuda_stream_t stream);
    
    // Copy GPU->host com async
    void d2h_async(void* dst, const void* src, size_t size, katte_cuda_stream_t stream);
    
    // Sincroniza stream
    void synchronize(katte_cuda_stream_t stream);
    
    // Cria stream CUDA otimizado
    katte_cuda_stream_t create_stream();
    
    // Verifica se GPU está disponível
    bool is_available() const { return available_; }
    
    // Estatísticas
    size_t gpu_memory_used() const { return gpu_used_; }
    size_t gpu_memory_total() const { return gpu_total_; }
    
private:
    bool available_;
    int device_id_;
    size_t gpu_total_;
    size_t gpu_used_;
    std::vector<void*> allocations_;
};

// ============================================================================
// Request Scheduler com Prioridade
// ============================================================================

enum class katte_request_priority {
    LOW = 0,
    NORMAL = 1,
    HIGH = 2,
    CRITICAL = 3
};

struct katte_request_context {
    uint64_t request_id;
    katte_request_priority priority;
    uint64_t arrival_time;
    uint32_t prompt_tokens;
    uint32_t max_tokens;
    bool streaming;
    std::string conversation_id;
    uint32_t conversation_revision;
    
    // Timestamps para métricas
    uint64_t ttft_start;
    uint64_t first_token_time;
    uint64_t completion_time;
};

class katte_request_scheduler {
public:
    katte_request_scheduler();
    ~katte_request_scheduler();
    
    // Adiciona requisição à fila
    void enqueue(katte_request_context ctx);
    
    // Obtém próxima requisição (baseado em prioridade + FIFO)
    bool dequeue(katte_request_context& out_ctx);
    
    // Remove requisição por ID
    void cancel(uint64_t request_id);
    
    // Número de requisições pendentes
    size_t pending_count() const;
    
    // Estatísticas por prioridade
    struct queue_stats {
        size_t low_priority;
        size_t normal_priority;
        size_t high_priority;
        size_t critical_priority;
        double avg_wait_time_ms;
    };
    
    queue_stats get_stats() const;
    
private:
    struct priority_queue {
        std::vector<katte_request_context> queues[4];  // Uma por prioridade
        mutable std::mutex mutex_;
    };
    
    priority_queue queues_;
    std::atomic<uint64_t> total_enqueued_;
    std::atomic<uint64_t> total_dequeued_;
};

// ============================================================================
// Performance Metrics Collector
// ============================================================================

struct katte_performance_metrics {
    // Latência
    double avg_ttft_ms;
    double p50_ttft_ms;
    double p95_ttft_ms;
    double p99_ttft_ms;
    
    // Throughput
    double tokens_per_second;
    double requests_per_second;
    
    // Uso de recursos
    double ram_usage_gb;
    double cpu_utilization_percent;
    double gpu_utilization_percent;
    double gpu_memory_gb;
    
    // Cache
    double kv_cache_hit_rate;
    double speculative_acceptance_rate;
    double compression_ratio;
    
    // Erros
    uint64_t total_requests;
    uint64_t failed_requests;
    uint64_t cache_misses;
};

class katte_metrics_collector {
public:
    katte_metrics_collector();
    ~katte_metrics_collector();
    
    // Registra evento de latência
    void record_ttft(double ms);
    void record_token_generation(double ms_per_token);
    
    // Registra uso de recursos
    void record_resource_usage(
        double ram_gb,
        double cpu_percent,
        double gpu_percent,
        double gpu_memory_gb
    );
    
    // Registra eventos de cache
    void record_cache_hit();
    void record_cache_miss();
    void record_speculative_accept(uint32_t accepted, uint32_t total);
    
    // Obtém métricas consolidadas
    katte_performance_metrics get_metrics() const;
    
    // Exporta para JSON
    std::string to_json() const;
    
    // Reset estatísticas
    void reset();
    
private:
    mutable std::mutex metrics_mutex_;
    
    // Histogramas para percentis
    std::vector<double> ttft_samples_;
    std::vector<double> token_gen_samples_;
    
    // Contadores
    std::atomic<uint64_t> cache_hits_;
    std::atomic<uint64_t> cache_misses_;
    std::atomic<uint32_t> speculative_accepted_;
    std::atomic<uint32_t> speculative_total_;
    
    // Últimas medições de recursos
    double last_ram_gb_;
    double last_cpu_percent_;
    double last_gpu_percent_;
    double last_gpu_memory_gb_;
};

// ============================================================================
// Utility Functions
// ============================================================================

namespace katte {
    // Detecta número ótimo de threads para CPU atual
    uint32_t detect_optimal_thread_count();
    
    // Obtém timestamp em microssegundos
    uint64_t now_us();
    
    // Prefetch de dados para cache L1/L2
    template<typename T>
    inline void prefetch(const T* ptr, int offset = 0) {
#ifdef __GNUC__
        __builtin_prefetch(ptr + offset, 0, 3);  // Read hint, high temporal locality
#else
        (void)ptr; (void)offset;
#endif
    }
    
    // Alinha valor para múltiplo de alignment
    template<typename T>
    inline constexpr T align_up(T value, T alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }
    
    // Barreira de memória para operações atômicas
    inline void memory_barrier() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
}
