/*
 * Cliente del monitor inteligente de sistema.
 *
 * Este programa corre en la maquina que se quiere monitorear. Cada
 * cierto tiempo lee un conjunto de metricas directamente del sistema
 * operativo (a traves de /proc, que en Linux funciona como una interfaz
 * para consultar el estado del kernel) y se las manda al servidor
 * central por un socket TCP, para que este las guarde y mas adelante se
 * puedan analizar buscando comportamientos anomalos.
 *
 * Metricas que se recolectan en cada ronda:
 *   uso de CPU en porcentaje, leido de /proc/stat
 *   uso de memoria en porcentaje, leido de /proc/meminfo
 *   cantidad de procesos activos, contando los directorios numericos
 *   dentro de /proc
 *   trafico de red en bytes por segundo, leido de /proc/net/dev
 *
 * Uso: ./client <ip_servidor> <puerto> [intervalo_segundos]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

/* /proc/stat trae, entre otras cosas, cuanto tiempo lleva la CPU
 * acumulado en cada uno de estos estados desde que arranco el sistema.
 * Guardamos una foto de estos valores en dos momentos distintos para
 * despues calcular, por diferencia, que porcentaje del tiempo estuvo
 * la CPU realmente ocupada entre esos dos momentos. */
typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_times_t;

/* Lee la primera linea de /proc/stat, que resume el uso acumulado de
 * CPU de todos los nucleos juntos, y la guarda en la estructura que
 * recibe por parametro. */
static int read_cpu_times(cpu_times_t *t) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;
    char label[16];
    int n = fscanf(fp, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                    label, &t->user, &t->nice, &t->system, &t->idle,
                    &t->iowait, &t->irq, &t->softirq, &t->steal);
    fclose(fp);
    return (n == 9) ? 0 : -1;
}

/* /proc/stat no da directamente un porcentaje de uso, da tiempo
 * acumulado. Para sacar un porcentaje real hay que comparar dos
 * lecturas tomadas con algo de tiempo de diferencia: el tiempo total
 * que paso entre esas dos lecturas, contra cuanto de ese tiempo la CPU
 * estuvo en estado inactivo (idle mas iowait). Lo que no fue tiempo
 * inactivo se considera tiempo de uso. */
static double cpu_percent(cpu_times_t *a, cpu_times_t *b) {
    unsigned long long idle_a = a->idle + a->iowait;
    unsigned long long idle_b = b->idle + b->iowait;

    unsigned long long total_a = a->user + a->nice + a->system + a->idle +
                                  a->iowait + a->irq + a->softirq + a->steal;
    unsigned long long total_b = b->user + b->nice + b->system + b->idle +
                                  b->iowait + b->irq + b->softirq + b->steal;

    unsigned long long total_delta = total_b - total_a;
    unsigned long long idle_delta = idle_b - idle_a;

    if (total_delta == 0) return 0.0;
    return 100.0 * (double)(total_delta - idle_delta) / (double)total_delta;
}

/* /proc/meminfo lista, linea por linea, distintos valores relacionados
 * con la memoria del sistema. A nosotros nos interesan solo dos: el
 * total de memoria disponible en la maquina y la memoria que en este
 * momento esta realmente disponible para usarse (que ya tiene en
 * cuenta cosas como la cache del sistema). Con esos dos valores se
 * puede calcular que porcentaje de la memoria esta en uso. */
static double mem_percent(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1.0;

    char key[64];
    unsigned long long value;
    char unit[16];
    unsigned long long mem_total = 0, mem_available = 0;

    while (fscanf(fp, "%63s %llu %15s", key, &value, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0) mem_total = value;
        else if (strcmp(key, "MemAvailable:") == 0) mem_available = value;
        if (mem_total && mem_available) break;
    }
    fclose(fp);

    if (mem_total == 0) return -1.0;
    return 100.0 * (double)(mem_total - mem_available) / (double)mem_total;
}

/* En Linux, cada proceso que esta corriendo tiene su propio directorio
 * dentro de /proc, y ese directorio se llama exactamente igual que el
 * PID del proceso (por ejemplo /proc/1234). Asi que para contar
 * cuantos procesos hay activos en este momento, alcanza con recorrer
 * /proc y contar cuantas entradas tienen un nombre formado solo por
 * digitos. */
static int count_processes(void) {
    DIR *d = opendir("/proc");
    if (!d) return -1;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        int all_digits = (name[0] != '\0');
        for (const char *p = name; *p; p++) {
            if (!isdigit((unsigned char)*p)) { all_digits = 0; break; }
        }
        if (all_digits) count++;
    }
    closedir(d);
    return count;
}

/* /proc/net/dev muestra, para cada interfaz de red del sistema, cuantos
 * bytes se recibieron y cuantos se enviaron desde que arranco. Sumamos
 * los bytes recibidos y enviados de todas las interfaces, dejando
 * afuera la interfaz "lo" (loopback), que es solo trafico interno de la
 * propia maquina y no representa trafico de red real. El resultado es
 * un contador acumulado, asi que para sacar bytes por segundo hay que
 * comparar dos lecturas igual que se hace con la CPU. */
static unsigned long long read_net_bytes(void) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return 0;

    char line[512];
    unsigned long long total = 0;

    /* Las primeras dos lineas del archivo son encabezados, no datos. */
    fgets(line, sizeof(line), fp);
    fgets(line, sizeof(line), fp);

    while (fgets(line, sizeof(line), fp)) {
        char iface[64];
        unsigned long long rx_bytes, tx_bytes;
        unsigned long long skip;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = ' ';

        int n = sscanf(line, "%63s %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                        iface, &rx_bytes, &skip, &skip, &skip, &skip, &skip, &skip, &skip, &tx_bytes);
        if (n < 10) continue;
        if (strcmp(iface, "lo") == 0) continue;

        total += rx_bytes + tx_bytes;
    }
    fclose(fp);
    return total;
}

int main(int argc, char *argv[]) {
    /* Sin esto, la salida por consola queda guardada en un buffer
     * interno y no se muestra en el momento, sino recien cuando el
     * buffer se llena. Eso hace que, por ejemplo al ver los logs de un
     * contenedor de Docker, no se vea nada hasta bastante despues de
     * que realmente paso. Con el buffer por linea, cada printf se
     * muestra apenas se ejecuta. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 3) {
        fprintf(stderr, "Uso: %s <ip_servidor> <puerto> [intervalo_segundos]\n", argv[0]);
        return 1;
    }
    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    int interval = argc >= 4 ? atoi(argv[3]) : 5;

    /* El primer argumento puede ser una direccion IP como 127.0.0.1,
     * pero tambien puede ser un nombre de host, por ejemplo el nombre
     * de un servicio dentro de Docker Compose. Para que ambos casos
     * funcionen usamos getaddrinfo, que sabe resolver nombres, en vez
     * de convertir el texto a IP a mano. */
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai_err = getaddrinfo(server_ip, port_str, &hints, &res);
    if (gai_err != 0) {
        fprintf(stderr, "No se pudo resolver %s: %s\n", server_ip, gai_strerror(gai_err));
        return 1;
    }

    /* getaddrinfo puede devolver mas de una direccion posible para el
     * mismo nombre, asi que probamos una por una hasta lograr conectar. */
    int sock = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock < 0) {
        fprintf(stderr, "No se pudo conectar a %s:%d\n", server_ip, port);
        return 1;
    }

    printf("Conectado a %s:%d. Enviando metricas cada %d segundos.\n",
           server_ip, port, interval);

    /* Tomamos una primera lectura antes de entrar al bucle principal,
     * para tener con que comparar en la primera vuelta. */
    cpu_times_t prev_cpu;
    read_cpu_times(&prev_cpu);
    unsigned long long prev_net = read_net_bytes();

    while (1) {
        sleep(interval);

        cpu_times_t cur_cpu;
        read_cpu_times(&cur_cpu);
        double cpu_pct = cpu_percent(&prev_cpu, &cur_cpu);
        prev_cpu = cur_cpu;

        double mem_pct = mem_percent();
        int proc_count = count_processes();

        unsigned long long cur_net = read_net_bytes();
        unsigned long long net_bps = (cur_net >= prev_net)
            ? (cur_net - prev_net) / (unsigned long long)interval
            : 0;
        prev_net = cur_net;

        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_info);

        /* Armamos la linea que se va a mandar al servidor, en el mismo
         * orden de columnas que despues espera el CSV. */
        char line[256];
        int len = snprintf(line, sizeof(line), "%s,%.2f,%.2f,%d,%llu\n",
                            ts, cpu_pct, mem_pct, proc_count, net_bps);

        if (write(sock, line, len) < 0) {
            perror("write");
            break;
        }
        printf("Enviado: %s", line);
    }

    close(sock);
    return 0;
}
