#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>

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
    int perros_capturados;
    int pos_puma;
    pthread_t fichas_tid[13];
} Mesa;

typedef struct {
    Mesa* mesa;
    int tipo;
    int id_ficha;
    int pos_actual;
} FichaArgs;

void manejador_sigint(int sig) {
    (void)sig; 
    torneo_activo = 0;
}

bool es_adyacente(int o, int d) {
    if (o < 0 || o > 25 || d < 0 || d > 25 || o == d) return false;
    if (o == 25) return (d == 1 || d == 2 || d == 3);
    if (d == 25) return (o == 1 || o == 2 || o == 3);
    
    int ox = o % 5, oy = o / 5;
    int dx = d % 5, dy = d / 5;
    
    int dist_x = abs(ox - dx);
    int dist_y = abs(oy - dy);
    
    if (dist_x > 1 || dist_y > 1) return false;
    if ((ox + oy) % 2 != 0 && dist_x == 1 && dist_y == 1) return false;
    
    return true;
}

int nodo_intermedio(int o, int d) {
    if (o == 25 || d == 25) return -1; 
    
    int ox = o % 5, oy = o / 5;
    int dx = d % 5, dy = d / 5;
    
    if (abs(ox - dx) == 0 && abs(oy - dy) == 2) return o + ((dy - oy) / 2) * 5;
    if (abs(oy - dy) == 0 && abs(ox - dx) == 2) return o + (dx - ox) / 2;
    if (abs(ox - dx) == 2 && abs(oy - dy) == 2) {
        if ((ox + oy) % 2 == 0) return o + ((dx - ox) / 2) + ((dy - oy) / 2) * 5;
    }
    return -1;
}

bool puma_sin_movimientos(Mesa* mesa) {
    int o = mesa->pos_puma;
    for (int d = 0; d < 26; d++) {
        if (mesa->tablero.casillas[d] == VACIO && es_adyacente(o, d)) {
            return false;
        }
        if (mesa->tablero.casillas[d] == VACIO) {
            int mid = nodo_intermedio(o, d);
            if (mid != -1 && mesa->tablero.casillas[mid] == PERRO) {
                return false;
            }
        }
    }
    return true;
}

void* hilo_ficha(void* arg) {
    FichaArgs* args = (FichaArgs*)arg;
    Mesa* mesa = args->mesa;
    
    while (mesa->activa && mesa->ganador == 0) {
        usleep((rand() % 251 + 50) * 1000);
        
        pthread_mutex_lock(&mesa->tablero.lock);
        
        if (!mesa->activa || mesa->ganador != 0) {
            pthread_mutex_unlock(&mesa->tablero.lock);
            break;
        }

        int posibles[26];
        int num_posibles = 0;
        int es_captura[26];

        for (int d = 0; d < 26; d++) {
            if (mesa->tablero.casillas[d] == VACIO) {
                if (es_adyacente(args->pos_actual, d)) {
                    es_captura[num_posibles] = -1;
                    posibles[num_posibles++] = d;
                } else if (args->tipo == PUMA) {
                    int mid = nodo_intermedio(args->pos_actual, d);
                    if (mid != -1 && mesa->tablero.casillas[mid] == PERRO) {
                        es_captura[num_posibles] = mid;
                        posibles[num_posibles++] = d;
                    }
                }
            }
        }

        if (num_posibles > 0) {
            int idx = rand() % num_posibles;
            int destino = posibles[idx];
            int captura = es_captura[idx];

            mesa->tablero.casillas[args->pos_actual] = VACIO;
            mesa->tablero.casillas[destino] = args->tipo;
            args->pos_actual = destino;

            if (args->tipo == PUMA) {
                mesa->pos_puma = destino;
                if (captura != -1) {
                    mesa->tablero.casillas[captura] = VACIO;
                    mesa->perros_capturados++;
                }
            }

            pthread_mutex_lock(&mesa->lock_juez);
            if (mesa->ganador == 0) {
                if (mesa->perros_capturados >= 6) {
                    mesa->ganador = PUMA;
                    pthread_cond_signal(&mesa->cv_juez);
                } else if (args->tipo == PERRO && puma_sin_movimientos(mesa)) {
                    mesa->ganador = PERRO;
                    pthread_cond_signal(&mesa->cv_juez);
                }
            }
            pthread_mutex_unlock(&mesa->lock_juez);
        }

        pthread_mutex_unlock(&mesa->tablero.lock);
        
        if (args->tipo == PERRO && args->pos_actual == -1) {
            break;
        }
    }
    
    free(args);
    return NULL;
}

void* hilo_juez(void* arg) {
    Mesa* mesa = (Mesa*)arg;
    
    pthread_mutex_lock(&mesa->lock_juez);
    while (mesa->activa && mesa->ganador == 0) {
        pthread_cond_wait(&mesa->cv_juez, &mesa->lock_juez);
    }
    pthread_mutex_unlock(&mesa->lock_juez);
    
    printf("Mesa %d finalizada. Ganador: %d (Capturas: %d)\n", mesa->id_mesa, mesa->ganador, mesa->perros_capturados);
    
    mesa->activa = false;
    
    for (int i = 0; i < 13; i++) {
        pthread_join(mesa->fichas_tid[i], NULL);
    }
    
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
        nueva_mesa->perros_capturados = 0;
        nueva_mesa->pos_puma = 25;
        
        pthread_mutex_init(&nueva_mesa->tablero.lock, NULL);
        pthread_mutex_init(&nueva_mesa->lock_juez, NULL);
        pthread_cond_init(&nueva_mesa->cv_juez, NULL);
        
        for(int i = 0; i < NUM_CASILLAS; i++) {
            nueva_mesa->tablero.casillas[i] = VACIO;
        }

        int pos_iniciales_perros[12] = {13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
        
        nueva_mesa->tablero.casillas[25] = PUMA;
        for (int i = 0; i < 12; i++) {
            nueva_mesa->tablero.casillas[pos_iniciales_perros[i]] = PERRO;
        }

        printf("Mesa %d asignada.\n", mi_mesa);

        for (int i = 0; i < 13; i++) {
            FichaArgs* f_args = malloc(sizeof(FichaArgs));
            f_args->mesa = nueva_mesa;
            f_args->id_ficha = i;
            
            if (i == 0) {
                f_args->tipo = PUMA;
                f_args->pos_actual = 25;
            } else {
                f_args->tipo = PERRO;
                f_args->pos_actual = pos_iniciales_perros[i - 1];
            }
            
            pthread_create(&nueva_mesa->fichas_tid[i], NULL, hilo_ficha, f_args);
        }

        pthread_t juez_tid;
        pthread_create(&juez_tid, NULL, hilo_juez, nueva_mesa);
        
        pthread_join(juez_tid, NULL);

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

    srand(time(NULL));
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