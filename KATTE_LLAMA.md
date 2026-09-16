# Katte-LLama: Extreme Optimization Fork

> **Objetivo:** Transformar o llama.cpp em um motor de inferência ultrarápido e extremamente eficiente em RAM para ambientes CPU-only, mantendo qualidade imperceptivelmente próxima do original.

## Alvo de Hardware

- **CPU:** AMD EPYC™ 9645 (4 vcores dedicados)
- **RAM:** 8GB DDR5 ECC (restrição crítica)
- **Storage:** SSD NVMe (para KV cache persistente)
- **Cenário:** Servidor de produção com múltiplos usuários simultâneos

## Prioridades

1. **Requests simultâneos não degradam desempenho** - TTFT, output e input consistentes
2. **Resposta quase instantânea** - Reutilização agressiva de contexto processado
3. **Consumo de RAM mínimo** - Modelos grandes (MoE como GLM 4.7 Flash, Gemma 4 26B A4B) em 8GB
4. **Velocidade de geração ultrarrápida** - Otimizações surreais de CPU
5. **Perda de qualidade imperceptível ou nula** - Validação rigorosa de estados

---

## Arquitetura Proposta

```
┌─────────────────────────────────────────────────────────────┐
│                    KATTE-LLAMA SERVER                        │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │  Permanent   │  │ Conversation │  │  Prediction  │       │
│  │     KV       │  │      KV      │  │    Store     │       │
│  │   (SSD)      │  │   (SSD/RAM)  │  │   (future)   │       │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘       │
│         │                 │                 │                │
│         └─────────────────┼─────────────────┘                │
│                           ▼                                  │
│              ┌────────────────────────┐                      │
│              │   Context Compression  │                      │
│              │   + Fingerprinting     │                      │
│              └────────────┬───────────┘                      │
│                           │                                  │
│                           ▼                                  │
│              ┌────────────────────────┐                      │
│              │   Adaptive Speculation │                      │
│              │      Policy Engine     │                      │
│              └────────────┬───────────┘                      │
│                           │                                  │
│                           ▼                                  │
│              ┌────────────────────────┐                      │
│              │   llama.cpp optimized  │                      │
│              │   + CPU extensions     │                      │
│              └────────────┬───────────┘                      │
│                           │                                  │
└───────────────────────────┼──────────────────────────────────┘
                            ▼
                   ┌─────────────────┐
                   │   AMD EPYC CPU  │
                   │   + NVMe SSD    │
                   └─────────────────┘
```

---

## Módulos Principais

### 1. Persistent KV Cache (SSD-Backed)

**Problema:** KV cache em RAM consome GBs rapidamente em conversas longas.

**Solução:** Armazenar estados KV em SSD com mmap inteligente, mantendo apenas hot cache em RAM.

**Implementação:**
- Fingerprint SHA256 de contexto completo (model + template + tokens + config)
- Armazenamento atômico com fsync + rename
- Validação de integridade antes de restaurar
- Tiered storage: RAM (hot) → SSD (persistent/cold)

**Ganho esperado:** Redução de 70-90% no uso de RAM para conversas longas.

---

### 2. Conversation Identity & Revisioning

**Problema:** Restaurar estado incompatível corrompe a inferência.

**Solução:** Identidade única por conversa com versionamento explícito.

**Implementação:**
- `conversation_id` + `revision_number` por sessão
- Snapshots computacionais validados
- Branching seguro para regeneração
- Metadados: SHA256, token_count, model_hash, template_hash

---

### 3. Context Compression

**Problema:** Histórico textual cresce linearmente, pressionando RAM.

**Solução:** Separar informação histórica do contexto ativo.

**Implementação:**
- Compressão lossless de tokens antigos
- Representação compacta de histórico distante
- Active context apenas com janelas recentes + compressed summary
- Trigger-based decompression quando necessário

**Ganho esperado:** Redução de 50-70% no contexto ativo em RAM.

---

### 4. CPU-Specific Optimizations (AMD EPYC)

**Problema:** Código genérico não explora arquitetura específica.

**Solução:** Otimizações manuais para Zen 4/EPYC 9645.

**Implementação:**
- AVX-512 otimizado para inferência
- Prefetching agressivo de dados do SSD
- Thread affinity para 4 vcores dedicados
- Cache-aware memory layout (L1/L2/L3 optimization)
- Non-temporal stores para streaming de KV
- Huge pages para mmap de KV cache

**Ganho esperado:** 2-4x speedup em prefill e decode vs código genérico.

---

### 5. Multi-Slot Concurrency

**Problema:** Múltiplos usuários degradam performance mútua.

**Solução:** Slots isolados com prioridades dinâmicas.

**Implementação:**
- Slot pool com isolamento estrito
- Priority scheduling baseado em latência
- Preemption segura para requests críticos
- Per-slot KV cache independente
- Fair share garantido mesmo sob carga

**Ganho esperado:** Latência consistente mesmo com 10+ usuários simultâneos.

---

### 6. Adaptive Speculation Policy

**Problema:** Speculative decoding fixo pode piorar performance.

**Solução:** Política adaptativa baseada em métricas em tempo real.

**Implementação:**
- Monitoramento: acceptance_rate, latency, throughput
- Ajuste dinâmico de draft_tokens
- Fallback automático se speculation falhar
- Learning por conversation pattern

**Ganho esperado:** 1.3-1.8x tokens/s em cenários favoráveis.

---

## Métricas de Sucesso

| Métrica | Baseline (llama.cpp) | Target (Katte-LLama) |
|---------|---------------------|----------------------|
| RAM usage (26B MoE, 32K ctx) | ~12 GB | ≤ 8 GB |
| TTFT (reused context) | ~500ms | ≤ 50ms |
| Token gen speed (CPU only) | ~15 tok/s | ≥ 40 tok/s |
| Multi-user degradation | High | Minimal (<10%) |
| Cache hit rate | N/A | ≥ 85% |
| Storage overhead | N/A | ≤ 2GB per 100K tokens |

---

## Roadmap de Implementação

### Fase 1: Fundação (Semanas 1-2)
- [x] Análise da arquitetura atual
- [ ] Implementar fingerprinting de contexto
- [ ] Criar módulo de Persistent KV (SSD-backed)
- [ ] Adicionar validação de integridade SHA256
- [ ] Testes básicos de save/restore

### Fase 2: Otimizações de CPU (Semanas 3-4)
- [ ] AVX-512 optimizations para Zen 4
- [ ] Thread affinity e NUMA awareness
- [ ] Huge pages support
- [ ] Prefetching agressivo de SSD
- [ ] Benchmark vs baseline

### Fase 3: Compressão de Contexto (Semanas 5-6)
- [ ] Algoritmo de compressão lossless
- [ ] Integration com KV cache
- [ ] Trigger-based decompression
- [ ] Validação de qualidade

### Fase 4: Multi-Slot Concurrency (Semanas 7-8)
- [ ] Slot isolation implementation
- [ ] Priority scheduler
- [ ] Preemption mechanism
- [ ] Load testing com múltiplos usuários

### Fase 5: Speculation Adaptativa (Semanas 9-10)
- [ ] Metrics collection
- [ ] Policy engine
- [ ] Dynamic adjustment
- [ ] Fallback mechanisms

### Fase 6: Polimento e Produção (Semanas 11-12)
- [ ] Observability (metrics, logs)
- [ ] Error handling robusto
- [ ] Documentation completa
- [ ] Production deployment guide

---

## Princípios de Engenharia

1. **Medir antes de otimizar** - Benchmarks obrigatórios
2. **Validação rigorosa** - Estado incompatível = cache miss
3. **Hot path minimalista** - Crítico para TTFT
4. **Cold path assíncrono** - Persistence, GC, pruning fora do caminho crítico
5. **Qualidade primeiro** - Otimização não pode degradar output
6. **Regra econômica:** `optimization_cost < compute_saved`

---

## Notas Técnicas

### Fingerprint Hash Composition
```
HASH = SHA256(
    model_id + model_revision +
    tokenizer_hash + chat_template_hash +
    system_prompt_hash + tools_hash +
    thinking_mode + runtime_config_hash +
    tokens_sequence
)
```

### Storage Layout (SSD)
```
/kv-cache/
├── permanent/
│   ├── {hash}.kv           # KV data
│   └── {hash}.meta         # Metadata (JSON)
├── conversations/
│   ├── {conv_id}/
│   │   ├── rev_{n}.kv
│   │   └── rev_{n}.meta
│   └── ...
└── prediction/             # Future
    └── ...
```

### Memory Tiers
```
Tier 0: CPU L1/L2 cache   (~MBs, ultra-fast)
Tier 1: RAM hot cache     (~2-4 GB, fast)
Tier 2: RAM mmap buffer   (~4-6 GB, medium)
Tier 3: SSD persistent    (~TBs, slow but durable)
```

---

## Referências

- Documento original Katte (conceitos de serving e reutilização)
- llama.cpp source code (src/llama-kv-cache.cpp, tools/server/)
- AMD EPYC 9645 architecture documentation
- Research papers on KV cache compression and speculative decoding

---

**Status:** Em desenvolvimento ativo  
**Licença:** MIT (mesma do llama.cpp)  
**Contribuições:** Bem-vindas via PRs com benchmarks
