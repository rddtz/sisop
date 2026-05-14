/*
 * memsim.c - Simulador de Gerência de Memória
 * INF01142 - Sistemas Operacionais - 2026/1
 *
 * =========================================================================
 * IMPLEMENTADO (não modificar):
 *   - Estruturas de dados (Memory, Process, PageTable, Frame)
 *   - Alocação contígua completa (alloc_contiguous, free_contiguous)
 *   - Driver de simulação (simulate)
 *   - Métricas (metrics_report)
 *   - main() com carga de trabalho predefinida
 *
 * A IMPLEMENTAR (apenas as funções marcadas com TODO):
 *   - alloc_paged()
 *   - free_paged()
 *   - lru_evict()
 *   - translate()
 * =========================================================================
 *
 * Compilação:
 *   Ver Makefile.
 *
 * Uso:
 *   ./memsim contiguous    executa com alocação contígua
 *   ./memsim paged         executa com alocação paginada + LRU
 *   ./memsim validate      executa teste de validação da função translate
 */

#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* =========================================================================
 * Parâmetros do simulador
 * ========================================================================= */

#define MEM_SIZE      64    /* tamanho total da memória em unidades          */
#define PAGE_SIZE      4    /* tamanho de uma página/quadro em unidades      */
#define N_FRAMES      (MEM_SIZE / PAGE_SIZE)   /* número de quadros físicos  */
#define MAX_PROCS     16    /* máximo de processos simultâneos               */
#define MAX_PAGES     16    /* máximo de páginas por processo                */

/* =========================================================================
 * Estruturas de dados
 * ========================================================================= */

/* Entrada da tabela de páginas de um processo */
typedef struct {
    int valid;          /* 1 = página está na memória, 0 = não está         */
    int frame;          /* índice do quadro físico (se valid == 1)          */
    int last_used;      /* instante do último acesso - usado pelo LRU       */
} PageEntry;

/* Tabela de páginas de um processo */
typedef struct {
    PageEntry entries[MAX_PAGES];
    int       n_pages;  /* número de páginas do processo                    */
} PageTable;

/* Processo */
typedef struct {
    int   pid;
    char  name[32];
    int   size;         /* tamanho em unidades de memória                   */
    int   duration;     /* número de acessos que o processo realiza         */
    int   active;       /* 1 = está na simulação                            */
    /* alocação contígua */
    int   base;         /* endereço base (-1 se não alocado)                */
    /* alocação paginada */
    PageTable pt;
} Process;

/* Quadro físico de memória */
typedef struct {
    int   free;         /* 1 = quadro livre                                 */
    int   owner_pid;    /* PID do processo dono (-1 se livre)               */
    int   owner_page;   /* índice da página no processo dono                */
} Frame;

/* Memória física */
typedef struct {
    Frame frames[N_FRAMES];
    int   clock;        /* contador global de tempo lógico                  */
} Memory;

/* Métricas coletadas durante a simulação */
typedef struct {
    int   total_allocs;       /* tentativas de alocação                     */
    int   failed_allocs;      /* alocações que falharam (sem espaço)        */
    int   page_faults;        /* falhas de página (apenas paginação)        */
    int   evictions;          /* páginas expulsas pelo LRU                  */
    long  internal_frag;      /* fragmentação interna acumulada (unidades)  */
    long  external_frag_samples; /* amostras de fragmentação externa        */
    int   n_samples;          /* número de amostras                         */
} Metrics;

/* =========================================================================
 * Utilitários
 * ========================================================================= */

static void mem_init(Memory *m)
{
    for (int i = 0; i < N_FRAMES; i++) {
        m->frames[i].free       = 1;
        m->frames[i].owner_pid  = -1;
        m->frames[i].owner_page = -1;
    }
    m->clock = 0;
}

/* Retorna o número de unidades livres contíguas mais longa */
static int largest_contiguous_free(const Memory *m)
{
    int max_run = 0, run = 0;
    for (int i = 0; i < N_FRAMES; i++) {
        if (m->frames[i].free) {
            run += PAGE_SIZE;
            if (run > max_run) max_run = run;
        } else {
            run = 0;
        }
    }
    return max_run;
}

/* Retorna o total de unidades livres */
static int total_free(const Memory *m)
{
    int free = 0;
    for (int i = 0; i < N_FRAMES; i++)
        if (m->frames[i].free) free += PAGE_SIZE;
    return free;
}

static void print_memory(const Memory *m, const char *label)
{
    printf("  [%s] ", label);
    for (int i = 0; i < N_FRAMES; i++) {
        if (m->frames[i].free)
            printf(".");
        else
            printf("%d", m->frames[i].owner_pid % 10);
    }
    printf("  livre=%d\n", total_free(m));
}

/* =========================================================================
 * Alocação CONTÍGUA - implementada (não modificar)
 * ========================================================================= */

/*
 * alloc_contiguous - aloca 'size' unidades de memória de forma contígua.
 * Usa política first-fit. Retorna o endereço base ou -1 se falhar.
 */
static int alloc_contiguous(Memory *m, Process *p, Metrics *met)
{
    int needed = (p->size + PAGE_SIZE - 1) / PAGE_SIZE; /* quadros necessários */
    met->total_allocs++;

    /* Busca first-fit */
    int start = -1, run = 0;
    for (int i = 0; i < N_FRAMES; i++) {
        if (m->frames[i].free) {
            if (run == 0) start = i;
            run++;
            if (run == needed) {
                /* Aloca */
                for (int j = start; j < start + needed; j++) {
                    m->frames[j].free       = 0;
                    m->frames[j].owner_pid  = p->pid;
                    m->frames[j].owner_page = j - start;
                }
                p->base = start * PAGE_SIZE;
                /* Fragmentação interna: espaço desperdiçado no último quadro */
                met->internal_frag += needed * PAGE_SIZE - p->size;
                return p->base;
            }
        } else {
            run = 0;
            start = -1;
        }
    }

    met->failed_allocs++;
    /* Amostra de fragmentação externa: há memória livre mas não contígua */
    if (total_free(m) >= needed * PAGE_SIZE) {
        met->external_frag_samples += largest_contiguous_free(m);
        met->n_samples++;
    }
    return -1;
}

/*
 * free_contiguous - libera todos os quadros alocados ao processo p.
 */
static void free_contiguous(Memory *m, Process *p)
{
    if (p->base < 0) return;
    int needed = (p->size + PAGE_SIZE - 1) / PAGE_SIZE;
    int start  = p->base / PAGE_SIZE;
    for (int i = start; i < start + needed; i++) {
        m->frames[i].free       = 1;
        m->frames[i].owner_pid  = -1;
        m->frames[i].owner_page = -1;
    }
    p->base = -1;
}

/* =========================================================================
 * Alocação PAGINADA - a implementar
 * ========================================================================= */

/*
 * lru_evict - expulsa o quadro menos recentemente usado.
 *
 * Percorre todos os quadros não-livres, encontra aquele cujo last_used
 * é o menor (mais antigo), invalida a entrada na tabela de páginas do
 * processo dono e marca o quadro como livre.
 *
 * Retorna o índice do quadro liberado.
 *
 * TODO: implemente esta função.
 */
static int lru_evict(Memory *m, Process procs[], int n_procs, Metrics *met)
{


  int last_used = 10000;
  int frame_id = 0;
  int lru_pid = 0;
  int lru_page_id = 0;

  for (int i = 0; i < N_FRAMES; i++) {

    if(!m->frames[i].free){
      int pid = m->frames[i].owner_pid;
      int page_id = m->frames[i].owner_page;
      int proc_index = -1;

      for(int pr = 0; pr < n_procs; pr++){
	if(procs[pr].pid == pid){
	  proc_index = pr;
	}
      }

      if(procs[proc_index].pt.entries[page_id].last_used < last_used){
	last_used = procs[proc_index].pt.entries[page_id].last_used;
	frame_id = i;
	lru_pid = proc_index;
	lru_page_id = page_id;
      }
    }
  }

  met->evictions++;
  procs[lru_pid].pt.entries[lru_page_id].valid = 0;
  m->frames[frame_id].free = 1;
  m->frames[frame_id].owner_pid = -1;

  return frame_id;
}

/*
 * alloc_paged - aloca as páginas do processo p usando paginação.
 *
 * Para cada página do processo:
 *   1. Procura um quadro livre em m->frames[].
 *   2. Se não houver quadro livre, chama lru_evict() para liberar um.
 *   3. Atribui o quadro à página, atualiza pt.entries[page].
 *
 * A fragmentação interna deve ser calculada para a última página
 * (igual à diferença entre o espaço alocado e o tamanho real do processo).
 *
 * Retorna 0 em caso de sucesso, -1 se não houver quadros suficientes
 * mesmo após tentativas de evicção.
 *
 * TODO: implemente esta função.
 */
static int alloc_paged(Memory *m, Process *p, Process procs[],
                       int n_procs, Metrics *met)
{

  met->total_allocs++;
  int n_pages = (p->size + PAGE_SIZE - 1) / PAGE_SIZE;
  for(int proc = 0; proc < n_pages; proc++){

    int i = 0;
    while(i < N_FRAMES && !m->frames[i].free){
      i++;
    }

    int free_frame = -1;

    if(i == N_FRAMES){
      free_frame = lru_evict(m, procs, n_procs, met);
      met->page_faults++;
    } else if (i >= 0){
      free_frame = i;
    }

    if(i == -1 && free_frame == -1){
      return -1;
    }

    p->pt.entries[proc].valid = 1;
    p->pt.entries[proc].frame = free_frame;
    p->pt.entries[proc].last_used = m->clock;
    p->pt.n_pages++;

    m->frames[free_frame].owner_page = proc;
    m->frames[free_frame].owner_pid = p->pid;
    m->frames[free_frame].free = 0;
  }
  met->internal_frag += n_pages * PAGE_SIZE - p->size;

  return 0;

}

/*
 * free_paged - libera todos os quadros alocados ao processo p.
 *
 * Para cada página válida na tabela de páginas do processo:
 *   - Marca o quadro como livre em m->frames[].
 *   - Invalida a entrada na tabela de páginas.
 *
 * TODO: implemente esta função.
 */
static void free_paged(Memory *m, Process *p)
{
    for (int i = 0; i < MAX_PAGES; i++) {
        if (p->pt.entries[i].valid) {
            int indice_frame = p->pt.entries[i].frame;

            if (indice_frame >= 0 && indice_frame < N_FRAMES) {
                m->frames[indice_frame].free = 1;
                m->frames[indice_frame].owner_pid = -1;
                m->frames[indice_frame].owner_page = -1;
            }

            p->pt.entries[i].valid = 0;
            p->pt.entries[i].frame = -1;
            p->pt.entries[i].last_used = 0;
        }
    }
}

/* =========================================================================
 * Tradução de endereço lógico → físico - a implementar
 * ========================================================================= */

/*
 * translate - traduz endereço lógico para físico usando a tabela de páginas.
 *
 * Dado um endereço lógico 'logical' e o processo p:
 *   1. Calcula o número de página: page = logical / PAGE_SIZE
 *   2. Calcula o offset:           offset = logical % PAGE_SIZE
 *   3. Verifica se a entrada é válida; se não, é uma falha de página
 *      (incrementa met->page_faults e retorna -1)
 *   4. Atualiza last_used com m->clock (acesso LRU)
 *   5. Retorna frame * PAGE_SIZE + offset
 *
 * TODO: implemente esta função.
 */
static int translate(Memory *m, Process *p, int logical, Metrics *met)
{
    int num_pagina = logical / PAGE_SIZE;
    int offset = logical % PAGE_SIZE;

    if (logical < 0 || num_pagina >= MAX_PAGES || !p->pt.entries[num_pagina].valid) {
        ++(met->page_faults);
        return -1;
    }

    p->pt.entries[num_pagina].last_used = m->clock;
    int indice_frame = p->pt.entries[num_pagina].frame;
    return indice_frame * PAGE_SIZE + offset;
}

/* =========================================================================
 * Carga de trabalho predefinida
 *
 * Projetada para que alocação contígua produza fragmentação externa
 * visível e paginação produza apenas fragmentação interna.
 * ========================================================================= */

#define N_EVENTS 20

typedef struct {
    int  type;       /* 0 = alloc, 1 = free                                */
    int  pid;        /* processo alvo                                       */
    char name[32];
    int  size;       /* tamanho (usado apenas em alloc)                     */
} Event;

static Event workload[N_EVENTS] = {
    {0,  1, "Alfa",    10},
    {0,  2, "Beta",     6},
    {0,  3, "Gama",    14},
    {1,  2, "",         0},   /* libera Beta → buraco de 6 unidades         */
    {0,  4, "Delta",    5},
    {1,  1, "",         0},   /* libera Alfa → buraco de 10 unidades        */
    {0,  5, "Épsilon", 18},   /* 18 unidades: cabe? depende da política     */
    {1,  4, "",         0},
    {0,  6, "Zeta",     8},
    {0,  7, "Eta",      4},
    {1,  3, "",         0},
    {0,  8, "Teta",    12},
    {1,  6, "",         0},
    {0,  9, "Iota",     9},
    {1,  7, "",         0},
    {0, 10, "Kapa",    16},
    {1,  5, "",         0},
    {0, 11, "Lambda",   7},
    {1,  8, "",         0},
    {0, 12, "Mi",      20},
};

/* =========================================================================
 * Driver da simulação
 * ========================================================================= */

static void simulate(int use_paging)
{
    Memory  mem;
    Process procs[MAX_PROCS];
    Metrics met;

    mem_init(&mem);
    memset(procs, 0, sizeof(procs));
    memset(&met,  0, sizeof(met));

    for (int i = 0; i < MAX_PROCS; i++) {
        procs[i].active = 0;
        procs[i].base   = -1;
        memset(&procs[i].pt, 0, sizeof(PageTable));
    }

    printf("\n=== Simulação: %s ===\n",
           use_paging ? "PAGINAÇÃO + LRU" : "ALOCAÇÃO CONTÍGUA");
    printf("Memória: %d unidades | Quadro: %d unidades | Quadros: %d\n\n",
           MEM_SIZE, PAGE_SIZE, N_FRAMES);

    for (int e = 0; e < N_EVENTS; e++) {
        Event *ev = &workload[e];
        mem.clock++;

        if (ev->type == 0) {
            /* Alocação */
            Process *p = &procs[ev->pid % MAX_PROCS];
            p->pid    = ev->pid;
            p->size   = ev->size;
            p->active = 1;
            strncpy(p->name, ev->name, 31);

            int ok;
            if (!use_paging) {
                ok = alloc_contiguous(&mem, p, &met) >= 0 ? 1 : 0;
            } else {
                memset(&p->pt, 0, sizeof(PageTable));
                ok = alloc_paged(&mem, p, procs, MAX_PROCS, &met) == 0 ? 1 : 0;
            }

            printf("t=%2d  ALLOC  pid=%-2d %-8s  tamanho=%-3d  %s\n",
                   mem.clock, p->pid, p->name, p->size,
                   ok ? "OK" : "FALHOU");
            print_memory(&mem, use_paging ? "pag" : "cnt");

        } else {
            /* Liberação */
            Process *p = &procs[ev->pid % MAX_PROCS];
            if (!use_paging)
                free_contiguous(&mem, p);
            else
                free_paged(&mem, p);
            p->active = 0;

            printf("t=%2d  FREE   pid=%-2d %-8s\n",
                   mem.clock, p->pid, p->name);
            print_memory(&mem, use_paging ? "pag" : "cnt");
        }
    }

    /* Relatório de métricas */
    printf("\n--- Métricas ---\n");
    printf("  Alocações totais   : %d\n",  met.total_allocs);
    printf("  Alocações falhas   : %d\n",  met.failed_allocs);
    if (use_paging) {
        printf("  Falhas de página   : %d\n",  met.page_faults);
        printf("  Evicções LRU       : %d\n",  met.evictions);
    }
    printf("  Frag. interna (tot): %ld unidades\n", met.internal_frag);
    if (!use_paging && met.n_samples > 0) {
        printf("  Frag. externa (avg): %.1f unidades (maior buraco livre)\n",
               (double)met.external_frag_samples / met.n_samples);
    }
    printf("  Memória livre final: %d unidades\n", total_free(&mem));
}

/* =========================================================================
 * Validação automática de translate()
 *
 * O oráculo abaixo foi pré-calculado a partir do estado final da simulação
 * paginada (após todos os 20 eventos da carga de trabalho). Cada entrada
 * descreve uma chamada esperada a translate() e o endereço físico correto.
 *
 * validate_translate() executa a simulação completa, depois percorre o
 * oráculo chamando translate() para cada caso e compara o resultado com o
 * valor esperado. Imprime PASS ou FAIL para cada caso e um resumo final.
 *
 * Não modifique esta seção.
 * ========================================================================= */

typedef struct {
    int pid;       /* PID do processo                          */
    int page;      /* número de página (endereço lógico base)  */
    int offset;    /* deslocamento dentro da página            */
    int expected;  /* endereço físico esperado                 */
} TranslateTest;

/* Oráculo gerado automaticamente - não modificar */
static const TranslateTest oracle[] = {
    { 9,  0,  0,  12},  /* pid=9  pg=0 off=0 */
    { 9,  0,  2,  14},  /* pid=9  pg=0 off=2 */
    { 9,  1,  0,  16},  /* pid=9  pg=1 off=0 */
    { 9,  1,  2,  18},  /* pid=9  pg=1 off=2 */
    { 9,  2,  0,  32},  /* pid=9  pg=2 off=0 */
    { 9,  2,  2,  34},  /* pid=9  pg=2 off=2 */
    {10,  0,  0,  44},  /* pid=10 pg=0 off=0 */
    {10,  0,  2,  46},  /* pid=10 pg=0 off=2 */
    {10,  1,  0,  48},  /* pid=10 pg=1 off=0 */
    {10,  1,  2,  50},  /* pid=10 pg=1 off=2 */
    {10,  2,  0,  52},  /* pid=10 pg=2 off=0 */
    {10,  2,  2,  54},  /* pid=10 pg=2 off=2 */
    {10,  3,  0,  56},  /* pid=10 pg=3 off=0 */
    {10,  3,  2,  58},  /* pid=10 pg=3 off=2 */
    {11,  0,  0,   0},  /* pid=11 pg=0 off=0 */
    {11,  0,  2,   2},  /* pid=11 pg=0 off=2 */
    {11,  1,  0,   4},  /* pid=11 pg=1 off=0 */
    {11,  1,  2,   6},  /* pid=11 pg=1 off=2 */
    {12,  0,  0,   8},  /* pid=12 pg=0 off=0 */
    {12,  0,  2,  10},  /* pid=12 pg=0 off=2 */
    {12,  1,  0,  20},  /* pid=12 pg=1 off=0 */
    {12,  1,  2,  22},  /* pid=12 pg=1 off=2 */
    {12,  2,  0,  24},  /* pid=12 pg=2 off=0 */
    {12,  2,  2,  26},  /* pid=12 pg=2 off=2 */
    {12,  3,  0,  28},  /* pid=12 pg=3 off=0 */
    {12,  3,  2,  30},  /* pid=12 pg=3 off=2 */
    {12,  4,  0,  36},  /* pid=12 pg=4 off=0 */
    {12,  4,  2,  38},  /* pid=12 pg=4 off=2 */
};
#define N_ORACLE 28

static void validate_translate(void)
{
    /* Reproduz a simulação paginada para obter o estado final de memória */
    Memory  mem;
    Process procs[MAX_PROCS];
    Metrics met;

    mem_init(&mem);
    memset(procs, 0, sizeof(procs));
    memset(&met,  0, sizeof(met));
    for (int i = 0; i < MAX_PROCS; i++) {
        procs[i].active = 0;
        procs[i].base   = -1;
        memset(&procs[i].pt, 0, sizeof(PageTable));
    }

    for (int e = 0; e < N_EVENTS; e++) {
        Event *ev = &workload[e];
        mem.clock++;
        if (ev->type == 0) {
            Process *p = &procs[ev->pid % MAX_PROCS];
            p->pid = ev->pid; p->size = ev->size; p->active = 1;
            strncpy(p->name, ev->name, 31);
            memset(&p->pt, 0, sizeof(PageTable));
            alloc_paged(&mem, p, procs, MAX_PROCS, &met);
        } else {
            Process *p = &procs[ev->pid % MAX_PROCS];
            free_paged(&mem, p);
            p->active = 0;
        }
    }

    /* Executa os casos do oracle */
    printf("\n=== Validação de translate() - %d casos ===\n", N_ORACLE);
    int passed = 0, failed = 0;

    for (int i = 0; i < N_ORACLE; i++) {
        const TranslateTest *tc = &oracle[i];
        int logical = tc->page * PAGE_SIZE + tc->offset;

        /* Encontra o processo */
        Process *p = NULL;
        for (int j = 0; j < MAX_PROCS; j++) {
            if (procs[j].pid == tc->pid && procs[j].active) {
                p = &procs[j];
                break;
            }
        }

        if (!p) {
            printf("  [SKIP] pid=%-2d pg=%-2d off=%-2d — processo não encontrado\n",
                   tc->pid, tc->page, tc->offset);
            continue;
        }

        int result = translate(&mem, p, logical, &met);

        if (result == tc->expected) {
            printf("  [PASS] pid=%-2d pg=%-2d off=%-2d  lógico=%-3d → físico=%-3d\n",
                   tc->pid, tc->page, tc->offset, logical, result);
            passed++;
        } else {
            printf("  [FAIL] pid=%-2d pg=%-2d off=%-2d  lógico=%-3d → físico=%-3d  esperado=%-3d\n",
                   tc->pid, tc->page, tc->offset, logical, result, tc->expected);
            failed++;
        }
    }

    printf("\nResultado: %d/%d PASS", passed, N_ORACLE);
    if (failed == 0)
        printf("  - translate() está correta.\n");
    else
        printf("  - %d falha(s) encontrada(s).\n", failed);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    if (argc < 2 || (strcmp(argv[1], "contiguous") != 0 &&
                     strcmp(argv[1], "paged")      != 0 &&
                     strcmp(argv[1], "validate")   != 0)) {
        fprintf(stderr,
            "Uso: %s contiguous | paged | validate\n"
            "  contiguous  - simula alocação contígua\n"
            "  paged       - simula paginação + LRU\n"
            "  validate    - valida translate() contra oráculo\n",
            argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "validate") == 0) {
        validate_translate();
    } else {
        int use_paging = strcmp(argv[1], "paged") == 0;
        simulate(use_paging);
    }
    return 0;
}
