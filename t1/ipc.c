/*
 * ipc.c — Módulo de IPC: fork() + pipes
 *
 * INF01142 — Sistemas Operacionais — 2026/1
 *
 * =========================================================================
 * ESTE É O ÚNICO ARQUIVO QUE DEVE SER MODIFICADO.
 * =========================================================================
 *
 * Implemente todas as funções declaradas em ipc.h.
 * Consulte os comentários em ipc.h para o contrato de cada função.
 *
 * Chamadas de sistema que você precisará:
 *   pipe(2), fork(2), read(2), write(2), close(2),
 *   waitpid(2), select(2) ou poll(2), exit(3), malloc(3), free(3)
 *
 * Referências rápidas:
 *   man 2 pipe      man 2 fork     man 2 waitpid
 *   man 2 select    man 2 read     man 2 write
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <errno.h>

#include "mandelbrot.h"
#include "ipc.h"

/* =========================================================================
 * Definição interna do Pool
 *
 * Sugestão de estrutura — pode-se alterar caso necessário.
 * ========================================================================= */
typedef struct {
    pid_t pid;      /* PID do filho, ou -1 se a entrada está livre */
    int   read_fd;  /* descritor de leitura do pipe de retorno      */
} PoolEntry;

struct Pool {
    int        max;     /* capacidade máxima do pool  */
    int        active;  /* número de filhos ativos    */
    PoolEntry *entries; /* array de entradas          */
};



/* =========================================================================
 * pool_create
 * ========================================================================= */
Pool *pool_create(int max_children)
{
    Pool *pool = malloc(sizeof(Pool));
    if (!pool) return NULL;

    pool->max     = max_children;
    pool->active  = 0;
    pool->entries = malloc(sizeof(PoolEntry) * max_children);
    if (!pool->entries) { free(pool); return NULL; }

    for (int i = 0; i < max_children; i++) {
        pool->entries[i].pid     = -1;
        pool->entries[i].read_fd = -1;
    }
    return pool;
}

/* =========================================================================
 * pool_destroy
 * ========================================================================= */
void pool_destroy(Pool *pool)
{
    if (!pool) return;
    for (int i = 0; i < pool->max; i++) {
        if (pool->entries[i].read_fd != -1)
            close(pool->entries[i].read_fd);
    }
    free(pool->entries);
    free(pool);
}

/* =========================================================================
 * pool_active
 * ========================================================================= */
int pool_active(const Pool *pool)
{
    return pool->active;
}

/* =========================================================================
 * launch_worker
 *
 * TODO: implemente esta função.
 * ========================================================================= */
void launch_worker(Pool *pool, const RenderParams *params, const Tile *t)
{

  int fd[2] = {0};

  if (pipe(fd) == -1) {
    perror("pipe");
    return;
  }

  pid_t pid = fork();

  if (pid < 0) {
    perror("fork");
    close(fd[0]);
    close(fd[1]);
    return;
  }

  if (pid > 0) { // pai
    close(fd[1]);
    for(int i = 0; i < pool->max; i++){

      if(pool->entries[i].pid == -1 && pool->entries[i].read_fd == -1){

        pool->entries[i].pid = pid;
        pool->entries[i].read_fd = fd[0];
        pool->active++;
        break;

      }
    }

    /* FILE *fptr; */
    /* fptr = fopen("log.txt", "a"); */

    /* fprintf(fptr, "Creating new process, pool is like:\n"); */

    /* for(int i = 0; i < pool->max; i++){ */

    /*   fprintf(fptr, "%d -> {%d, %d}\n", i, pool->entries[i].pid, pool->entries[i].read_fd); */
    /* } */
    /* fclose(fptr); */

  } else { // filho
    close(fd[0]);
    worker_main(params, t, fd[1]);
  }

}

/* =========================================================================
 * worker_main
 *
 * TODO: implemente esta função.
 * ========================================================================= */
void worker_main(const RenderParams *params, const Tile *tile, int write_fd)
{

  unsigned char *buf = (unsigned char*)  malloc(sizeof(char) * tile->h * tile->w);

  if (!buf) {
    perror("malloc");
    exit(1);
  }

  compute_tile(params, tile, buf);

  /* FILE *fptr; */

  /* fptr = fopen("log.txt", "a"); */
  /* fprintf(fptr, "Computed for: w = %d, h = %d, ox = %d, oy = %d\n", tile->w, tile->h, tile->ox, tile->oy); */

  size_t written = 0;
  do{
    written += write(write_fd, tile, sizeof(int) * 4);
  } while (written < sizeof(int) * 4);

  written = 0;
  do{
    written += write(write_fd, buf, sizeof(char) * tile->h * tile->w);
  } while (written < sizeof(char) * tile->h * tile->w);

  /* fprintf(fptr, "Sended\n"); */
  /* fclose(fptr); */

  close(write_fd);
  free(buf);
  exit(0);

}

/* =========================================================================
 * pool_collect_ready
 *
 * TODO: implemente esta função.
 * ========================================================================= */
int pool_collect_ready(Pool *pool, TileResult *result)
{
    if (pool->active == 0) return 0;

    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;
    for (int i = 0; i < pool->max; i++) {
      if (pool->entries[i].pid != -1 && pool->entries[i].read_fd != -1) {
        FD_SET(pool->entries[i].read_fd, &rfds);
        if (pool->entries[i].read_fd > maxfd){
          maxfd = pool->entries[i].read_fd;
        }
      }
    }


    struct timeval tv = {0, 0}; // timeout zero = não bloqueia
    int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (ready <= 0) return 0;


    for(int i = 0; i < pool->max; i++ ){
      if(pool->entries[i].read_fd != -1 && FD_ISSET(pool->entries[i].read_fd, &rfds)) {

	/* char NAME[100] = {0}; */
	/* sprintf(NAME, "file-for-%d.txt", i); */

	/* FILE *fptr; */

	/* fptr = fopen(NAME, "a"); //"log.txt", "a"); */
	/* fprintf(fptr, "Reading from %d\n", i); */

	  size_t readden = 0;
	do{
	  readden += read(pool->entries[i].read_fd, &result->tile, sizeof(int) * 4); //ler cabeçalho (4 ints: ox, oy, w, h)
	} while (readden < sizeof(int) * 4);

	/* result->tile = tile_result; */
	result->pixels = malloc(sizeof(char) * result->tile.h * result->tile.w); //alocar result->pixels com malloc(w  h)


	readden = 0;
        do{
	  readden += read(pool->entries[i].read_fd, result->pixels, sizeof(char) * result->tile.h * result->tile.w);
	} while (readden < sizeof(char) * result->tile.h * result->tile.w);

	/* /\* fptr = fopen("log.txt", "a"); *\/ */
	/* fprintf(fptr, "Data successfully read from %d\n", i); */
	/* fprintf(fptr, "Readed: w = %d, h = %d, ox = %d, oy = %d\n", result->tile.w, result->tile.h, result->tile.ox, result->tile.oy); */


	/* fclose(fptr); */

        close(pool->entries[i].read_fd);
	pool->entries[i].read_fd = -1;

	return 1;
      }
    }

    return 0;
}

/* =========================================================================
 * pool_reap
 *
 * TODO: implemente esta função.
 * ========================================================================= */
void pool_reap(Pool *pool)
{
  int status;
  pid_t pid;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {

    /* FILE *fptr; */

    /* fptr = fopen("log.txt", "a"); */

    /* fprintf(fptr, "Trying to collect from pid %d with status %d\n", pid, status); */
    /* fprintf(fptr, "pool is like:\n"); */

    /* for(int i = 0; i < pool->max; i++){ */
    /*   fprintf(fptr, "%d -> {%d, %d}\n", i, pool->entries[i].pid, pool->entries[i].read_fd); */
    /* } */

    /* // Close the file */
    /* fclose(fptr); */

    for(int i = 0; i < pool->max; i++){
      if(pool->entries[i].pid == pid){

        pool->entries[i].pid = -1;

        close(pool->entries[i].read_fd);
        pool->entries[i].read_fd = -1;

        pool->active--;
      }

    }
  }

}
