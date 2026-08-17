/*
 * Servidor central del monitor inteligente de sistema.
 *
 * Este programa se queda escuchando en un puerto TCP y puede atender a
 * varios clientes al mismo tiempo. Cada cliente conectado va mandando
 * lineas de texto con las metricas que recolecta de su propio sistema
 * (uso de CPU, memoria, cantidad de procesos y trafico de red), y el
 * servidor simplemente las va guardando en un archivo CSV a medida que
 * llegan, ademas de mostrarlas por pantalla.
 *
 * Uso: ./server <puerto> [archivo_salida.csv]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

/* FD_SETSIZE es el limite de descriptores que select() puede vigilar a
 * la vez, asi que lo usamos directamente como cantidad maxima de
 * clientes simultaneos que el servidor puede atender. */
#define MAX_CLIENTS FD_SETSIZE
#define BUF_SIZE 1024

/* Cada cliente conectado necesita su propio "estado": el socket por el
 * que se comunica, su direccion IP en texto, y un buffer propio donde
 * se va acumulando lo que llega hasta encontrar un salto de linea
 * completo. Sin este buffer por cliente, si dos clientes mandan datos
 * casi al mismo tiempo se podrian mezclar los mensajes. */
typedef struct {
    int fd;
    char ip[INET_ADDRSTRLEN];
    char buf[BUF_SIZE];
    size_t len;
} client_t;

static client_t clients[MAX_CLIENTS];
static FILE *out_fp;

/* Genera la hora actual en un formato legible, para poder marcar en el
 * CSV el momento exacto en que el servidor recibio cada metrica. */
static void timestamp_now(char *dst, size_t n) {
    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(dst, n, "%Y-%m-%d %H:%M:%S", &tm_info);
}

/* Se llama cada vez que se termina de recibir una linea completa de un
 * cliente. La linea que llega ya trae el formato que arma client.c:
 * ts_cliente,cpu_pct,mem_pct,proc_count,net_bps
 * Aca solo la mostramos en pantalla para poder seguir el monitoreo en
 * vivo, y la guardamos en el archivo CSV agregandole ademas la hora en
 * que el servidor la recibio y la IP de quien la mando. */
static void handle_line(const char *client_ip, char *line) {
    char recv_ts[32];
    timestamp_now(recv_ts, sizeof(recv_ts));

    printf("[%s] %s -> %s\n", recv_ts, client_ip, line);
    fflush(stdout);

    fprintf(out_fp, "%s,%s,%s\n", recv_ts, client_ip, line);
    fflush(out_fp);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <puerto> [archivo_salida.csv]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *out_path = argc >= 3 ? argv[2] : "metrics.csv";

    /* Si el archivo ya existia de una corrida anterior, seguimos
     * agregando datos al final en vez de sobreescribir todo, y no
     * volvemos a escribir el encabezado del CSV. */
    int existed = access(out_path, F_OK) == 0;
    out_fp = fopen(out_path, "a");
    if (!out_fp) {
        perror("fopen");
        return 1;
    }
    if (!existed) {
        fprintf(out_fp, "recv_ts,client_ip,client_ts,cpu_pct,mem_pct,proc_count,net_bps\n");
        fflush(out_fp);
    }

    /* Creamos el socket que va a quedar escuchando conexiones entrantes. */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    /* Sin esta opcion, si el servidor se reinicia rapido despues de
     * pararlo, el sistema operativo puede negarse a reusar el mismo
     * puerto durante un rato. */
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        perror("listen");
        return 1;
    }

    /* Marcamos todos los slots de clientes como libres antes de arrancar. */
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    printf("Servidor escuchando en puerto %d, guardando en %s\n", port, out_path);

    /* Este es el corazon del servidor concurrente. En vez de atender un
     * cliente a la vez, usamos select() para preguntarle al sistema
     * operativo cuales sockets tienen datos esperando (tanto el socket
     * que escucha conexiones nuevas como los sockets de cada cliente ya
     * conectado), y solo trabajamos sobre los que realmente tienen algo
     * que leer. Asi un servidor de un solo proceso puede atender a
     * muchos clientes al mismo tiempo sin bloquearse esperando a uno
     * solo. */
    fd_set read_fds;
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        int max_fd = listen_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1) {
                FD_SET(clients[i].fd, &read_fds);
                if (clients[i].fd > max_fd) max_fd = clients[i].fd;
            }
        }

        /* Aca el proceso se queda dormido hasta que pase algo en
         * cualquiera de los sockets que le pasamos. */
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("select");
            break;
        }

        /* Si el socket que escucha tiene actividad, es porque llego un
         * cliente nuevo pidiendo conectarse. */
        if (FD_ISSET(listen_fd, &read_fds)) {
            struct sockaddr_in cli_addr;
            socklen_t cli_len = sizeof(cli_addr);
            int cfd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
            if (cfd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd == -1) { slot = i; break; }
                }
                if (slot == -1) {
                    fprintf(stderr, "Maximo de clientes alcanzado, rechazando conexion\n");
                    close(cfd);
                } else {
                    clients[slot].fd = cfd;
                    clients[slot].len = 0;
                    inet_ntop(AF_INET, &cli_addr.sin_addr, clients[slot].ip, sizeof(clients[slot].ip));
                    printf("Nuevo cliente conectado: %s (slot %d)\n", clients[slot].ip, slot);
                }
            }
        }

        /* Ahora revisamos, uno por uno, si algun cliente ya conectado
         * mando datos nuevos. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd == -1 || !FD_ISSET(clients[i].fd, &read_fds)) continue;

            char tmp[BUF_SIZE];
            ssize_t n = read(clients[i].fd, tmp, sizeof(tmp) - 1);
            if (n <= 0) {
                /* read() devuelve 0 o error cuando el cliente cerro la
                 * conexion, asi que liberamos su slot. */
                printf("Cliente desconectado: %s\n", clients[i].ip);
                close(clients[i].fd);
                clients[i].fd = -1;
                continue;
            }
            tmp[n] = '\0';

            /* TCP no garantiza que cada read() traiga exactamente una
             * linea completa, puede traer una linea a medias o varias
             * juntas. Por eso vamos acumulando todo en el buffer del
             * cliente y recien procesamos lo que ya viene con su salto
             * de linea al final. */
            if (clients[i].len + (size_t)n < BUF_SIZE) {
                memcpy(clients[i].buf + clients[i].len, tmp, n);
                clients[i].len += n;
                clients[i].buf[clients[i].len] = '\0';
            } else {
                /* Si el buffer se llenaria, algo raro esta pasando
                 * (una linea demasiado larga), asi que lo vaciamos y
                 * seguimos para no romper nada. */
                clients[i].len = 0;
                continue;
            }

            /* Recorremos el buffer buscando saltos de linea. Cada vez
             * que encontramos uno, ya tenemos una linea completa para
             * procesar. Lo que sobre despues del ultimo salto de linea
             * (un mensaje a medias) se deja guardado para la proxima
             * vuelta. */
            char *start = clients[i].buf;
            char *nl;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = '\0';
                if (strlen(start) > 0) handle_line(clients[i].ip, start);
                start = nl + 1;
            }
            size_t remaining = strlen(start);
            memmove(clients[i].buf, start, remaining + 1);
            clients[i].len = remaining;
        }
    }

    fclose(out_fp);
    close(listen_fd);
    return 0;
}
