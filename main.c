#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>

#define NUM_CASILLAS 26
#define VACIO 0
#define PUMA 1
#define PERRO 2

volatile sig_atomic_t torneo_activo = 1;

sem_t mesas_disponibles;
pthread_mutex_t lock_matchmaker = PTHREAD_MUTEX_INITIALIZER;
int jugadores_en_espera = 0;
int total_mesas_creadas = 0;

typedef struct {
    int casillas[NUM_CASILLAS];
    pthread_mutex_t lock;
} Tablero;

typedef struct {
    int id_mesa;
    Tablero tablero;
    pthread_cond_t cv_juez;
    pthread_mutex_t lock_juez;
    int ganador;
    bool activa;
} Mesa;

void manejador_sigint(int sig) {
    (void)sig; 
    torneo_activo = 0;
}

void* hilo_juez(void* arg) {
    Mesa* mesa = (Mesa*)arg;
    
    pthread_mutex_lock(&mesa->lock_juez);
    while (mesa->activa && mesa->ganador == 0) {
        pthread_cond_wait(&mesa->cv_juez, &mesa->lock_juez);
    }
    pthread_mutex_unlock(&mesa->lock_juez);
    
    printf("Mesa %d finalizada. Ganador: %d\n", mesa->id_mesa, mesa->ganador);
    
    pthread_mutex_destroy(&mesa->tablero.lock);
    pthread_mutex_destroy(&mesa->lock_juez);
    pthread_cond_destroy(&mesa->cv_juez);
    
    sem_post(&mesas_disponibles);
    free(mesa);
    return NULL;
}

void* conexion_jugador(void* arg) {
    int id_jugador = *(int*)arg;
    (void)id_jugador;
    free(arg);

    pthread_mutex_lock(&lock_matchmaker);
    jugadores_en_espera++;
    
    if (jugadores_en_espera >= 2) {
        jugadores_en_espera -= 2;
        pthread_mutex_unlock(&lock_matchmaker);

        sem_wait(&mesas_disponibles);
        
        pthread_mutex_lock(&lock_matchmaker);
        total_mesas_creadas++;
        int mi_mesa = total_mesas_creadas;
        pthread_mutex_unlock(&lock_matchmaker);

        Mesa* nueva_mesa = malloc(sizeof(Mesa));
        nueva_mesa->id_mesa = mi_mesa;
        nueva_mesa->activa = true;
        nueva_mesa->ganador = 0;
        
        pthread_mutex_init(&nueva_mesa->tablero.lock, NULL);
        pthread_mutex_init(&nueva_mesa->lock_juez, NULL);
        pthread_cond_init(&nueva_mesa->cv_juez, NULL);
        
        for(int i = 0; i < NUM_CASILLAS; i++) {
            nueva_mesa->tablero.casillas[i] = VACIO;
        }

        printf("Mesa %d asignada.\n", mi_mesa);

        pthread_t juez_tid;
        pthread_create(&juez_tid, NULL, hilo_juez, nueva_mesa);
        
        sleep(2);
        
        pthread_mutex_lock(&nueva_mesa->lock_juez);
        nueva_mesa->ganador = PUMA;
        pthread_cond_signal(&nueva_mesa->cv_juez);
        pthread_mutex_unlock(&nueva_mesa->lock_juez);

        pthread_detach(juez_tid);

    } else {
        pthread_mutex_unlock(&lock_matchmaker);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 4) {
        return 1;
    }

    int N = atoi(argv[1]);
    int M = atoi(argv[2]);

    if (N <= 0 || M <= 0 || N % 2 != 0) {
        return 1;
    }

    sem_init(&mesas_disponibles, 0, M);

    struct sigaction sa;
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    pthread_t* hilos_clientes = malloc(N * sizeof(pthread_t));

    for (int i = 0; i < N; i++) {
        int* id_jugador = malloc(sizeof(int));
        *id_jugador = i + 1;
        pthread_create(&hilos_clientes[i], NULL, conexion_jugador, id_jugador);
        usleep(50000);
    }

    for (int i = 0; i < N; i++) {
        pthread_join(hilos_clientes[i], NULL);
    }

    free(hilos_clientes);
    sem_destroy(&mesas_disponibles);
    
    return 0;
}