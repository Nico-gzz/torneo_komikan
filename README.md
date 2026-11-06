# Torneo de Komikan Concurrente

Tarea 2 de Sistemas Operativos. Simula un torneo de Komikan en el que N jugadores se emparejan (Puma contra Perros) y juegan en tiempo real, sin turnos, en hasta M mesas simultáneas. Está escrito en C con hilos POSIX, mutex, variables de condición y semáforos.

## Compilación

```
make
```

Usa `gcc -Wall -Wextra -std=c17 -pthread`. Para borrar el binario: `make clean`.

## Uso

```
./torneo N M [config.txt]
```

- `N`: número de jugadores (entero positivo y par).
- `M`: máximo de mesas activas al mismo tiempo.
- `config.txt`: opcional. Si no se entrega, se usan valores por defecto.

Ejemplo:

```
./torneo 6 2 config.txt
```

## Archivo de configuración

Una clave por línea. Lo que sigue a `#` se ignora.

| Clave | Descripción | Por defecto |
|---|---|---|
| `MIN_DELAY`, `MAX_DELAY` | Retardo aleatorio entre decisiones de cada ficha (ms) | 50 y 300 |
| `SEED` | Semilla para las decisiones y retardos de cada ficha | hora actual |
| `ARRIVAL_DELAY` | Tiempo entre llegadas de jugadores (ms) | 10 |
| `LOG_MOVIMIENTOS` | 1 imprime cada movimiento | 0 |
| `PUMA` | Casilla inicial del puma | 27 |
| `PERROS` | 12 casillas iniciales de los perros | 5 a 16 |
| `FALLO` | `FALLO <mesa> <PUMA\|PERROS> <ms>`: simula la caída de ese jugador (su hilo muere sin avisar) a los `<ms>` de iniciada la mesa | ninguno |

Numeración de casillas: la cuadrícula 5×5 va de 0 a 24 (id = fila·5 + columna, fila 0 abajo). El trono usa 25 (izquierda), 26 (derecha) y 27 (vértice superior).

## Qué hace

- Un hilo por jugador llega al servidor. Los jugadores se emparejan de a dos: el primero juega con el Puma y el segundo con los Perros.
- Un semáforo limita las mesas activas a M. Las parejas sin mesa esperan bloqueadas.
- Cada mesa tiene un hilo juez y 13 hilos de ficha (1 puma y 12 perros). El tablero se protege con un mutex.
- El juez imprime el tablero cada 500 ms, valida el estado y detecta el fin de la partida. Gana el Puma con 6 capturas y ganan los Perros si el puma queda sin movimientos.
- Abandono: cada ficha registra un latido en cada ciclo. Si el juez detecta que una ficha dejó de dar señales, declara ganador al oponente por abandono. El mutex del tablero es robusto, así que si el hilo caído retenía el candado, las demás fichas lo recuperan y la mesa se cierra sin bloquearse.
- Con Ctrl+C no se aceptan más jugadores, las mesas terminan en orden y se liberan todos los recursos.

## Archivos

- `main.c`: servidor, mesas, jueces y fichas.
- `config.txt`: ejemplo de configuración.
- `Makefile`: compilación.
- `informe.pdf`: informe de diseño y análisis de concurrencia.
