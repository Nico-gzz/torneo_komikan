#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NUM_GRID 25
#define NODO_IZQ 25
#define NODO_DER 26
#define NODO_TRONO 27
#define NUM_CASILLAS 28
#define NUM_PERROS 12
#define NUM_FICHAS (NUM_PERROS + 1)
#define MAX_ADY 6
#define MAX_SALTOS 8
#define CAPTURAS_VICTORIA 6
#define MAX_FALLOS 32
#define MARGEN_LATIDO_MS 1000

#define VACIO 0
#define PUMA 1
#define PERRO 2

enum { CAUSA_NINGUNA, CAUSA_CAPTURAS, CAUSA_ACORRALADO, CAUSA_ABANDONO,
       CAUSA_INTERRUPCION, CAUSA_ERROR };

typedef struct {
    int mesa;
    int bando;
    int ms;
} Fallo;

typedef struct {
    int min_delay, max_delay;
    unsigned int seed;
    bool seed_definida;
    int llegada_ms;
    bool log_movimientos;
    int pos_puma;
    int pos_perros[NUM_PERROS];
    int n_fallos;
    Fallo fallos[MAX_FALLOS];
} Config;

static Config cfg;

typedef struct { int medio; int destino; } Salto;

static int ady[NUM_CASILLAS][MAX_ADY];
static int n_ady[NUM_CASILLAS];
static Salto saltos[NUM_CASILLAS][MAX_SALTOS];
static int n_saltos[NUM_CASILLAS];
static int coord_x[NUM_CASILLAS], coord_y[NUM_CASILLAS];

static void conectar(int a, int b) {
    ady[a][n_ady[a]++] = b;
    ady[b][n_ady[b]++] = a;
}

static void construir_topologia(void) {
    for (int i = 0; i < NUM_GRID; i++) {
        coord_x[i] = i % 5;
        coord_y[i] = i / 5;
        if (coord_x[i] < 4) conectar(i, i + 1);
        if (coord_y[i] < 4) conectar(i, i + 5);
    }
    coord_x[NODO_IZQ] = 1;   coord_y[NODO_IZQ] = 5;
    coord_x[NODO_DER] = 3;   coord_y[NODO_DER] = 5;
    coord_x[NODO_TRONO] = 2; coord_y[NODO_TRONO] = 6;

    conectar(NODO_TRONO, NODO_IZQ);
    conectar(NODO_TRONO, NODO_DER);
    conectar(NODO_IZQ, NODO_DER);

    conectar(NODO_TRONO, 22);
    conectar(NODO_IZQ, 20);
    conectar(NODO_IZQ, 21);
    conectar(NODO_DER, 23);
    conectar(NODO_DER, 24);

    for (int o = 0; o < NUM_CASILLAS; o++) {
        for (int i = 0; i < n_ady[o]; i++) {
            int m = ady[o][i];
            for (int j = 0; j < n_ady[m]; j++) {
                int d = ady[m][j];
                if (d == o) continue;
                int vx = coord_x[m] - coord_x[o], vy = coord_y[m] - coord_y[o];
                int wx = coord_x[d] - coord_x[m], wy = coord_y[d] - coord_y[m];
                if (vx * wy - vy * wx == 0 && vx * wx + vy * wy > 0) {
                    saltos[o][n_saltos[o]].medio = m;
                    saltos[o][n_saltos[o]].destino = d;
                    n_saltos[o]++;
                }
            }
        }
    }
}

typedef struct {
    int id;
    int casillas[NUM_CASILLAS];
    int ficha_en[NUM_CASILLAS];
    bool capturada[NUM_FICHAS];
    atomic_long latido[NUM_FICHAS];
    atomic_bool viva[NUM_FICHAS];
    int pos_puma;
    int perros_capturados;
    pthread_mutex_t lock_tablero;

    pthread_mutex_t lock_juez;
    pthread_cond_t cv_juez;
    atomic_bool terminada;
    int ganador;
    int causa;

    int id_jugador_puma, id_jugador_perros;
    struct timespec inicio;
    int n_fichas;
    pthread_t fichas_tid[NUM_FICHAS];
} Mesa;

typedef struct {
    Mesa *mesa;
    int tipo;
    int id_ficha;
    int pos_inicial;
} FichaArgs;

typedef struct Pareja {
    int id_puma;
    bool emparejada;
    bool fin;
    pthread_cond_t cv;
} Pareja;

static volatile sig_atomic_t torneo_activo = 1;

static sem_t mesas_disponibles;
static pthread_mutex_t lock_matchmaker = PTHREAD_MUTEX_INITIALIZER;
static Pareja *pareja_pendiente = NULL;
static atomic_int contador_mesas = 0;

static pthread_mutex_t lock_stats = PTHREAD_MUTEX_INITIALIZER;
static int st_jugadas = 0, st_puma = 0, st_perros = 0;
static int st_abandonos = 0, st_interrumpidas = 0, st_errores = 0;

static void manejador_sigint(int sig) {
    (void)sig;
    torneo_activo = 0;
}

static void sumar_ms(struct timespec *t, long ms) {
    t->tv_nsec += (ms % 1000) * 1000000L;
    t->tv_sec += ms / 1000;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec++;
    }
}

static long ms_desde(const struct timespec *t0) {
    struct timespec ahora;
    clock_gettime(CLOCK_MONOTONIC, &ahora);
    return (ahora.tv_sec - t0->tv_sec) * 1000L +
           (ahora.tv_nsec - t0->tv_nsec) / 1000000L;
}

static void dormir_ms(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int rand_rango(unsigned int *semilla, int lo, int hi) {
    return lo + (int)(rand_r(semilla) % (unsigned)(hi - lo + 1));
}

static int crear_hilo(pthread_t *t, void *(*fn)(void *), void *arg) {
    sigset_t nuevo, viejo;
    sigemptyset(&nuevo);
    sigaddset(&nuevo, SIGINT);
    pthread_sigmask(SIG_BLOCK, &nuevo, &viejo);
    int rc = pthread_create(t, NULL, fn, arg);
    pthread_sigmask(SIG_SETMASK, &viejo, NULL);
    return rc;
}

static int init_mutex_robusto(pthread_mutex_t *m) {
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_setrobust(&at, PTHREAD_MUTEX_ROBUST);
    int rc = pthread_mutex_init(m, &at);
    pthread_mutexattr_destroy(&at);
    return rc;
}

static void lock_tablero(Mesa *m) {
    int rc = pthread_mutex_lock(&m->lock_tablero);
    if (rc == EOWNERDEAD) {
        pthread_mutex_consistent(&m->lock_tablero);
        printf("[Mesa %d] Candado del tablero recuperado: su dueno murio "
               "reteniendolo (EOWNERDEAD)\n", m->id);
    }
}

static void unlock_tablero(Mesa *m) {
    pthread_mutex_unlock(&m->lock_tablero);
}

static void config_por_defecto(Config *c) {
    memset(c, 0, sizeof *c);
    c->min_delay = 50;
    c->max_delay = 300;
    c->llegada_ms = 10;
    c->pos_puma = NODO_TRONO;
    for (int i = 0; i < NUM_PERROS; i++) c->pos_perros[i] = 5 + i;
}

static bool config_valida(const Config *c) {
    if (c->min_delay < 0 || c->max_delay < c->min_delay) {
        fprintf(stderr, "Config: MIN_DELAY/MAX_DELAY invalidos\n");
        return false;
    }
    if (c->llegada_ms < 0) {
        fprintf(stderr, "Config: ARRIVAL_DELAY invalido\n");
        return false;
    }
    bool usada[NUM_CASILLAS] = { false };
    if (c->pos_puma < 0 || c->pos_puma >= NUM_CASILLAS) {
        fprintf(stderr, "Config: posicion del puma fuera de rango\n");
        return false;
    }
    usada[c->pos_puma] = true;
    for (int i = 0; i < NUM_PERROS; i++) {
        int p = c->pos_perros[i];
        if (p < 0 || p >= NUM_CASILLAS || usada[p]) {
            fprintf(stderr, "Config: posicion de perro invalida o repetida (%d)\n", p);
            return false;
        }
        usada[p] = true;
    }
    return true;
}

static bool cargar_config(const char *ruta, Config *c) {
    FILE *f = fopen(ruta, "r");
    if (!f) {
        perror("No se pudo abrir el archivo de configuracion");
        return false;
    }
    char linea[512];
    int nl = 0;
    bool ok = true;
    while (ok && fgets(linea, sizeof linea, f)) {
        nl++;
        char *com = strchr(linea, '#');
        if (com) *com = '\0';
        char clave[64];
        int off = 0;
        if (sscanf(linea, "%63s%n", clave, &off) != 1) continue;
        char *resto = linea + off;
        int v;

        if (strcmp(clave, "MIN_DELAY") == 0 && sscanf(resto, "%d", &v) == 1) {
            c->min_delay = v;
        } else if (strcmp(clave, "MAX_DELAY") == 0 && sscanf(resto, "%d", &v) == 1) {
            c->max_delay = v;
        } else if (strcmp(clave, "SEED") == 0 && sscanf(resto, "%d", &v) == 1) {
            c->seed = (unsigned int)v;
            c->seed_definida = true;
        } else if (strcmp(clave, "ARRIVAL_DELAY") == 0 && sscanf(resto, "%d", &v) == 1) {
            c->llegada_ms = v;
        } else if (strcmp(clave, "LOG_MOVIMIENTOS") == 0 && sscanf(resto, "%d", &v) == 1) {
            c->log_movimientos = (v != 0);
        } else if (strcmp(clave, "PUMA") == 0 && sscanf(resto, "%d", &v) == 1) {
            c->pos_puma = v;
        } else if (strcmp(clave, "PERROS") == 0) {
            char *p = resto;
            for (int i = 0; i < NUM_PERROS && ok; i++) {
                char *fin;
                long x = strtol(p, &fin, 10);
                if (fin == p) {
                    fprintf(stderr, "Config linea %d: PERROS necesita %d posiciones\n",
                            nl, NUM_PERROS);
                    ok = false;
                } else {
                    c->pos_perros[i] = (int)x;
                    p = fin;
                }
            }
        } else if (strcmp(clave, "FALLO") == 0) {
            int mesa, ms;
            char bando[16];
            if (c->n_fallos < MAX_FALLOS &&
                sscanf(resto, "%d %15s %d", &mesa, bando, &ms) == 3 &&
                (strcmp(bando, "PUMA") == 0 || strcmp(bando, "PERROS") == 0)) {
                Fallo *fa = &c->fallos[c->n_fallos++];
                fa->mesa = mesa;
                fa->bando = (strcmp(bando, "PUMA") == 0) ? PUMA : PERRO;
                fa->ms = ms;
            } else {
                fprintf(stderr, "Config linea %d: FALLO <mesa> <PUMA|PERROS> <ms>\n", nl);
                ok = false;
            }
        } else {
            fprintf(stderr, "Config linea %d: clave o valor no reconocido: %s\n", nl, clave);
            ok = false;
        }
    }
    fclose(f);
    return ok && config_valida(c);
}

static bool puma_sin_movimientos(const Mesa *m) {
    int o = m->pos_puma;
    for (int i = 0; i < n_ady[o]; i++)
        if (m->casillas[ady[o][i]] == VACIO) return false;
    for (int i = 0; i < n_saltos[o]; i++)
        if (m->casillas[saltos[o][i].medio] == PERRO &&
            m->casillas[saltos[o][i].destino] == VACIO) return false;
    return true;
}

static bool declarar_fin(Mesa *m, int ganador, int causa) {
    bool declarado = false;
    pthread_mutex_lock(&m->lock_juez);
    if (!atomic_load(&m->terminada)) {
        m->ganador = ganador;
        m->causa = causa;
        atomic_store(&m->terminada, true);
        pthread_cond_signal(&m->cv_juez);
        declarado = true;
    }
    pthread_mutex_unlock(&m->lock_juez);
    return declarado;
}

static void evaluar_victoria(Mesa *m) {
    if (m->perros_capturados >= CAPTURAS_VICTORIA)
        declarar_fin(m, PUMA, CAUSA_CAPTURAS);
    else if (puma_sin_movimientos(m))
        declarar_fin(m, PERRO, CAUSA_ACORRALADO);
}

static bool mover_perro(Mesa *m, int id_ficha, int *pos, unsigned int *sem,
                        char *evento, size_t n) {
    int op[MAX_ADY], k = 0;
    for (int i = 0; i < n_ady[*pos]; i++)
        if (m->casillas[ady[*pos][i]] == VACIO) op[k++] = ady[*pos][i];
    if (k == 0) return false;
    int d = op[rand_rango(sem, 0, k - 1)];
    m->casillas[*pos] = VACIO;
    m->casillas[d] = PERRO;
    m->ficha_en[*pos] = -1;
    m->ficha_en[d] = id_ficha;
    if (cfg.log_movimientos)
        snprintf(evento, n, "[Mesa %d] Perro %d: %02d -> %02d\n", m->id, id_ficha, *pos, d);
    *pos = d;
    return true;
}

static bool mover_puma(Mesa *m, int *pos, unsigned int *sem, char *evento, size_t n) {
    int cap_i[MAX_SALTOS], kc = 0;
    for (int i = 0; i < n_saltos[*pos]; i++)
        if (m->casillas[saltos[*pos][i].medio] == PERRO &&
            m->casillas[saltos[*pos][i].destino] == VACIO) cap_i[kc++] = i;

    if (kc > 0) {
        Salto s = saltos[*pos][cap_i[rand_rango(sem, 0, kc - 1)]];
        m->casillas[*pos] = VACIO;
        m->casillas[s.medio] = VACIO;
        m->casillas[s.destino] = PUMA;
        m->ficha_en[*pos] = -1;
        m->capturada[m->ficha_en[s.medio]] = true;
        m->ficha_en[s.medio] = -1;
        m->ficha_en[s.destino] = 0;
        m->perros_capturados++;
        snprintf(evento, n, ">>> [Mesa %d] PUMA salta %02d -> %02d y captura al perro de %02d "
                 "(capturas: %d/%d)\n", m->id, *pos, s.destino, s.medio,
                 m->perros_capturados, CAPTURAS_VICTORIA);
        *pos = s.destino;
        m->pos_puma = *pos;
        return true;
    }

    int op[MAX_ADY], k = 0;
    for (int i = 0; i < n_ady[*pos]; i++)
        if (m->casillas[ady[*pos][i]] == VACIO) op[k++] = ady[*pos][i];
    if (k == 0) return false;
    int d = op[rand_rango(sem, 0, k - 1)];
    m->casillas[*pos] = VACIO;
    m->casillas[d] = PUMA;
    m->ficha_en[*pos] = -1;
    m->ficha_en[d] = 0;
    if (cfg.log_movimientos)
        snprintf(evento, n, "[Mesa %d] Puma: %02d -> %02d\n", m->id, *pos, d);
    *pos = d;
    m->pos_puma = d;
    return true;
}

static bool debe_fallar(const Mesa *m, int id_ficha) {
    for (int i = 0; i < cfg.n_fallos; i++) {
        const Fallo *f = &cfg.fallos[i];
        if (f->mesa != m->id) continue;

        int rep = (f->bando == PUMA) ? 0 : 1;
        if (id_ficha == rep && ms_desde(&m->inicio) >= f->ms) return true;
    }
    return false;
}

static void *hilo_ficha(void *arg) {
    FichaArgs a = *(FichaArgs *)arg;
    free(arg);

    Mesa *m = a.mesa;
    int pos = a.pos_inicial;
    unsigned int semilla = cfg.seed + 1000003u * (unsigned)m->id + 7919u * (unsigned)a.id_ficha;

    while (!atomic_load(&m->terminada) && torneo_activo) {
        atomic_store(&m->latido[a.id_ficha], ms_desde(&m->inicio));
        dormir_ms(rand_rango(&semilla, cfg.min_delay, cfg.max_delay));
        if (atomic_load(&m->terminada) || !torneo_activo) break;
        atomic_store(&m->latido[a.id_ficha], ms_desde(&m->inicio));

        if (debe_fallar(m, a.id_ficha)) {
            lock_tablero(m);
            printf("[Mesa %d] FALLO: el jugador %s cae reteniendo el candado del tablero\n",
                   m->id, a.tipo == PUMA ? "Puma" : "Perros");
            return NULL;
        }

        char evento[160] = "";
        lock_tablero(m);
        if (atomic_load(&m->terminada) || m->capturada[a.id_ficha]) {
            unlock_tablero(m);
            break;
        }
        bool movio = (a.tipo == PUMA)
            ? mover_puma(m, &pos, &semilla, evento, sizeof evento)
            : mover_perro(m, a.id_ficha, &pos, &semilla, evento, sizeof evento);
        if (movio) evaluar_victoria(m);
        unlock_tablero(m);

        if (evento[0]) fputs(evento, stdout);
    }
    atomic_store(&m->viva[a.id_ficha], false);
    return NULL;
}

static void imprimir_tablero(Mesa *m) {
    int c[NUM_CASILLAS], capt;
    lock_tablero(m);
    memcpy(c, m->casillas, sizeof c);
    capt = m->perros_capturados;
    unlock_tablero(m);

    char s[NUM_CASILLAS];
    for (int i = 0; i < NUM_CASILLAS; i++)
        s[i] = (c[i] == PUMA) ? 'P' : (c[i] == PERRO) ? 'X' : '.';

    char buf[1400];
    int n = 0;
    n += snprintf(buf + n, sizeof buf - n,
                  "\n===== MESA %d | t=%.1fs | capturas %d/%d =====\n",
                  m->id, ms_desde(&m->inicio) / 1000.0, capt, CAPTURAS_VICTORIA);
    n += snprintf(buf + n, sizeof buf - n, "                (%c)\n", s[NODO_TRONO]);
    n += snprintf(buf + n, sizeof buf - n, "          /  |  \\\n");
    n += snprintf(buf + n, sizeof buf - n, "      (%c)----+----(%c)\n", s[NODO_IZQ], s[NODO_DER]);
    n += snprintf(buf + n, sizeof buf - n, "    /  |     |     |  \\\n");
    for (int y = 4; y >= 0; y--) {
        n += snprintf(buf + n, sizeof buf - n, "(%c)---(%c)---(%c)---(%c)---(%c)\n",
                      s[y * 5], s[y * 5 + 1], s[y * 5 + 2], s[y * 5 + 3], s[y * 5 + 4]);
        if (y > 0)
            n += snprintf(buf + n, sizeof buf - n, " |     |     |     |     |\n");
    }
    n += snprintf(buf + n, sizeof buf - n, "P=puma  X=perro  .=libre\n");
    fputs(buf, stdout);
}

static void validar_invariantes(Mesa *m) {
    lock_tablero(m);
    int pumas = 0, perros = 0;
    for (int i = 0; i < NUM_CASILLAS; i++) {
        if (m->casillas[i] == PUMA) pumas++;
        else if (m->casillas[i] == PERRO) perros++;
    }
    bool ok = (pumas == 1) && (m->casillas[m->pos_puma] == PUMA) &&
              (perros == NUM_PERROS - m->perros_capturados);
    unlock_tablero(m);
    if (!ok)
        printf("[Juez %d] ERROR: estado inconsistente (pumas=%d, perros=%d)\n",
               m->id, pumas, perros);
}

static void finalizar_mesa(Mesa *m) {
    for (int i = 0; i < m->n_fichas; i++)
        pthread_join(m->fichas_tid[i], NULL);

    lock_tablero(m);
    unlock_tablero(m);

    const char *quien = (m->ganador == PUMA) ? "PUMA" : "PERROS";
    int jg = (m->ganador == PUMA) ? m->id_jugador_puma : m->id_jugador_perros;
    switch (m->causa) {
    case CAUSA_CAPTURAS:
        printf("Mesa %d finalizada. Ganador: %s (jugador %d) por capturas (%d perros)\n",
               m->id, quien, jg, m->perros_capturados);
        break;
    case CAUSA_ACORRALADO:
        printf("Mesa %d finalizada. Ganador: %s (jugador %d): puma acorralado "
               "(capturas: %d)\n", m->id, quien, jg, m->perros_capturados);
        break;
    case CAUSA_ABANDONO:
        printf("Mesa %d finalizada. Ganador: %s (jugador %d) por abandono del oponente\n",
               m->id, quien, jg);
        break;
    case CAUSA_INTERRUPCION:
        printf("Mesa %d finalizada sin ganador: interrumpida por SIGINT\n", m->id);
        break;
    default:
        printf("Mesa %d abortada por error interno\n", m->id);
        break;
    }

    pthread_mutex_lock(&lock_stats);
    if (m->causa == CAUSA_INTERRUPCION) st_interrumpidas++;
    else if (m->causa == CAUSA_ERROR) st_errores++;
    else {
        st_jugadas++;
        if (m->ganador == PUMA) st_puma++; else st_perros++;
        if (m->causa == CAUSA_ABANDONO) st_abandonos++;
    }
    pthread_mutex_unlock(&lock_stats);

    pthread_mutex_destroy(&m->lock_tablero);
    pthread_mutex_destroy(&m->lock_juez);
    pthread_cond_destroy(&m->cv_juez);

    free(m);
    sem_post(&mesas_disponibles);
}

static void detectar_abandono(Mesa *m) {
    long limite = cfg.max_delay + MARGEN_LATIDO_MS;
    long ahora = ms_desde(&m->inicio);
    for (int i = 0; i < m->n_fichas; i++) {
        if (atomic_load(&m->viva[i]) && ahora - atomic_load(&m->latido[i]) > limite) {
            if (declarar_fin(m, i == 0 ? PERRO : PUMA, CAUSA_ABANDONO))
                printf("[Juez %d] Abandono detectado: el jugador %s dejo de responder\n",
                       m->id, i == 0 ? "Puma" : "Perros");
            return;
        }
    }
}

static void *hilo_juez(void *arg) {
    Mesa *m = (Mesa *)arg;
    long ultimo_print = -1000000;
    struct timespec t0 = m->inicio;

    for (;;) {
        pthread_mutex_lock(&m->lock_juez);
        if (!atomic_load(&m->terminada) && torneo_activo) {
            struct timespec lim;
            clock_gettime(CLOCK_REALTIME, &lim);
            sumar_ms(&lim, 100);
            pthread_cond_timedwait(&m->cv_juez, &m->lock_juez, &lim);
        }
        pthread_mutex_unlock(&m->lock_juez);

        if (!torneo_activo) declarar_fin(m, 0, CAUSA_INTERRUPCION);
        if (!atomic_load(&m->terminada)) detectar_abandono(m);

        bool fin = atomic_load(&m->terminada);
        long t = ms_desde(&t0);
        if (fin || t - ultimo_print >= 500) {
            imprimir_tablero(m);
            validar_invariantes(m);
            ultimo_print = t;
        }
        if (fin) break;
    }
    finalizar_mesa(m);
    return NULL;
}

static void jugar_partida(int id_puma, int id_perros) {
    for (;;) {
        if (!torneo_activo) {
            printf("[Matchmaker] Pareja (%d,%d) cancelada por SIGINT antes de obtener mesa\n",
                   id_puma, id_perros);
            return;
        }
        struct timespec lim;
        clock_gettime(CLOCK_REALTIME, &lim);
        sumar_ms(&lim, 100);
        if (sem_timedwait(&mesas_disponibles, &lim) == 0) break;
        if (errno != ETIMEDOUT && errno != EINTR) {
            perror("sem_timedwait");
            return;
        }
    }
    if (!torneo_activo) {
        sem_post(&mesas_disponibles);
        printf("[Matchmaker] Pareja (%d,%d) cancelada por SIGINT antes de obtener mesa\n",
               id_puma, id_perros);
        return;
    }

    Mesa *m = calloc(1, sizeof *m);
    if (!m) {
        perror("calloc");
        sem_post(&mesas_disponibles);
        return;
    }
    m->id = atomic_fetch_add(&contador_mesas, 1) + 1;
    m->id_jugador_puma = id_puma;
    m->id_jugador_perros = id_perros;
    atomic_init(&m->terminada, false);
    init_mutex_robusto(&m->lock_tablero);
    pthread_mutex_init(&m->lock_juez, NULL);
    pthread_cond_init(&m->cv_juez, NULL);

    for (int i = 0; i < NUM_CASILLAS; i++) {
        m->casillas[i] = VACIO;
        m->ficha_en[i] = -1;
    }
    m->casillas[cfg.pos_puma] = PUMA;
    m->ficha_en[cfg.pos_puma] = 0;
    m->pos_puma = cfg.pos_puma;
    for (int i = 0; i < NUM_PERROS; i++) {
        m->casillas[cfg.pos_perros[i]] = PERRO;
        m->ficha_en[cfg.pos_perros[i]] = i + 1;
    }

    printf("Mesa %d asignada: jugador %d (Puma) vs jugador %d (Perros)\n",
           m->id, id_puma, id_perros);

    clock_gettime(CLOCK_MONOTONIC, &m->inicio);
    evaluar_victoria(m);

    for (int i = 0; i < NUM_FICHAS; i++) {
        FichaArgs *fa = malloc(sizeof *fa);
        if (!fa) { declarar_fin(m, 0, CAUSA_ERROR); break; }
        fa->mesa = m;
        fa->id_ficha = i;
        fa->tipo = (i == 0) ? PUMA : PERRO;
        fa->pos_inicial = (i == 0) ? cfg.pos_puma : cfg.pos_perros[i - 1];
        atomic_store(&m->latido[i], 0);
        atomic_store(&m->viva[i], true);
        if (crear_hilo(&m->fichas_tid[i], hilo_ficha, fa) != 0) {
            atomic_store(&m->viva[i], false);
            free(fa);
            declarar_fin(m, 0, CAUSA_ERROR);
            break;
        }
        m->n_fichas++;
    }

    pthread_t juez;
    if (crear_hilo(&juez, hilo_juez, m) != 0) {
        declarar_fin(m, 0, CAUSA_ERROR);
        finalizar_mesa(m);
        return;
    }
    pthread_join(juez, NULL);
}

static void *hilo_jugador(void *arg) {
    int id = *(int *)arg;
    free(arg);

    pthread_mutex_lock(&lock_matchmaker);
    if (!torneo_activo) {
        pthread_mutex_unlock(&lock_matchmaker);
        return NULL;
    }

    if (pareja_pendiente == NULL) {
        Pareja *p = calloc(1, sizeof *p);
        if (!p) {
            pthread_mutex_unlock(&lock_matchmaker);
            return NULL;
        }
        p->id_puma = id;
        pthread_cond_init(&p->cv, NULL);
        pareja_pendiente = p;

        while (!p->fin) {
            if (!p->emparejada && !torneo_activo) {
                pareja_pendiente = NULL;
                break;
            }
            struct timespec lim;
            clock_gettime(CLOCK_REALTIME, &lim);
            sumar_ms(&lim, 100);
            pthread_cond_timedwait(&p->cv, &lock_matchmaker, &lim);
        }
        bool sin_pareja = !p->emparejada;
        pthread_cond_destroy(&p->cv);
        free(p);
        pthread_mutex_unlock(&lock_matchmaker);
        if (sin_pareja)
            printf("[Matchmaker] Jugador %d liberado sin pareja (SIGINT)\n", id);
    } else {
        Pareja *p = pareja_pendiente;
        pareja_pendiente = NULL;
        p->emparejada = true;
        int id_puma = p->id_puma;
        pthread_mutex_unlock(&lock_matchmaker);

        printf("[Matchmaker] Jugadores %d (Puma) y %d (Perros) emparejados\n", id_puma, id);
        jugar_partida(id_puma, id);

        pthread_mutex_lock(&lock_matchmaker);
        p->fin = true;
        pthread_cond_signal(&p->cv);
        pthread_mutex_unlock(&lock_matchmaker);
    }
    return NULL;
}

static bool leer_entero_pos(const char *s, int *out) {
    char *fin;
    long v = strtol(s, &fin, 10);
    if (*s == '\0' || *fin != '\0' || v <= 0 || v > 1000000) return false;
    *out = (int)v;
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Uso: %s N M [config.txt]\n", argv[0]);
        return 1;
    }
    int N, M;
    if (!leer_entero_pos(argv[1], &N) || !leer_entero_pos(argv[2], &M) || N % 2 != 0) {
        fprintf(stderr, "N debe ser un entero positivo par y M un entero positivo\n");
        return 1;
    }

    config_por_defecto(&cfg);
    if (argc == 4 && !cargar_config(argv[3], &cfg)) return 1;
    if (!cfg.seed_definida) cfg.seed = (unsigned int)time(NULL);

    setvbuf(stdout, NULL, _IOLBF, 0);
    construir_topologia();

    if (sem_init(&mesas_disponibles, 0, (unsigned int)M) != 0) {
        perror("sem_init");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    printf("--- Torneo Komikan --- jugadores=%d mesas_max=%d delay=[%d,%d]ms seed=%u\n",
           N, M, cfg.min_delay, cfg.max_delay, cfg.seed);

    pthread_t *hilos = malloc((size_t)N * sizeof *hilos);
    if (!hilos) {
        perror("malloc");
        sem_destroy(&mesas_disponibles);
        return 1;
    }

    int creados = 0;
    for (int i = 0; i < N && torneo_activo; i++) {
        int *id = malloc(sizeof *id);
        if (!id) break;
        *id = i + 1;
        if (crear_hilo(&hilos[creados], hilo_jugador, id) != 0) {
            free(id);
            break;
        }
        creados++;
        dormir_ms(cfg.llegada_ms);
    }
    if (!torneo_activo)
        printf("\n[Servidor] SIGINT recibido: no se aceptan mas jugadores (llegaron %d de %d). "
               "Cerrando las partidas activas...\n", creados, N);

    for (int i = 0; i < creados; i++) pthread_join(hilos[i], NULL);

    printf("\n[Servidor] Resumen: %d partidas jugadas (Puma %d, Perros %d, por abandono %d), "
           "%d interrumpidas, %d con error\n",
           st_jugadas, st_puma, st_perros, st_abandonos, st_interrumpidas, st_errores);

    free(hilos);
    sem_destroy(&mesas_disponibles);
    pthread_mutex_destroy(&lock_matchmaker);
    pthread_mutex_destroy(&lock_stats);
    printf("[Servidor] Recursos liberados. Torneo finalizado limpiamente.\n");
    return 0;
}
