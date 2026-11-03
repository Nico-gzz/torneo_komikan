#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>

// Bandera atómica para el apagado seguro
volatile sig_atomic_t torneo_activo = 1;

void manejador_sigint(int sig) {
    (void)sig; // Evita warning de variable sin uso
    torneo_activo = 0;
    // Aquí agregaremos la liberación de memoria y semáforos
}

int main(int argc, char *argv[]) {
    // 1. Validación estricta de parámetros
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Uso: %s N M [config.txt]\n", argv[0]);
        return 1;
    }

    int N = atoi(argv[1]);
    int M = atoi(argv[2]);

    if (N <= 0 || M <= 0 || N % 2 != 0) {
        fprintf(stderr, "Error: N y M deben ser positivos. N debe ser par.\n");
        return 1;
    }

    // 2. Configurar captura de Ctrl+C (SIGINT)
    struct sigaction sa;
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    printf("Servidor Komikan iniciado con %d jugadores y %d mesas maximas.\n", N, M);
    printf("Presiona Ctrl+C para un apagado seguro.\n");

    // Bucle temporal para mantener el servidor vivo
    while(torneo_activo) {
        sleep(1);
    }

    // 3. Limpieza de recursos al salir
    printf("\nApagando servidor y limpiando recursos...\n");
    return 0;
}