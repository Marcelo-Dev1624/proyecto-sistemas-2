# Monitor inteligente de sistema (anomaly detection)

Prototipo del proyecto de investigacion de Sistemas Operativos II. Arquitectura
cliente-servidor que recolecta metricas del sistema operativo desde `/proc` en
un servidor Ubuntu Server y aplica un modelo de inteligencia artificial
(Isolation Forest) para detectar comportamientos anomalos.

## Componentes

- `server.c`: servidor central. Acepta conexiones concurrentes de varios
  clientes (via `select()`) y guarda las metricas recibidas en `metrics.csv`.
- `client.c`: agente que corre en el nodo monitoreado. Lee `/proc/stat`,
  `/proc/meminfo`, `/proc` (conteo de procesos) y `/proc/net/dev`, y envia una
  linea de metricas al servidor cada N segundos.
- `analyze.py`: modulo de IA. Aplica `IsolationForest` (scikit-learn) sobre
  `metrics.csv` y reporta los registros marcados como anomalos.

## Compilar (en Ubuntu Server)

```bash
gcc -O2 -Wall -o server server.c
gcc -O2 -Wall -o client client.c
```

## Ejecutar

En el servidor central:

```bash
./server 9000
```

En el (los) nodo(s) monitoreado(s), apuntando a la IP del servidor:

```bash
./client 192.168.1.10 9000 5
```

El tercer argumento es el intervalo en segundos entre envios de metricas
(por defecto 5).

Para probar todo en una sola maquina Ubuntu Server, se puede correr el
servidor y el cliente en dos terminales distintas usando `127.0.0.1` como IP.

## Despliegue con Docker

El proyecto incluye `Dockerfile.server`, `Dockerfile.client` y
`docker-compose.yml` para desplegar el prototipo de forma reproducible.

```bash
sudo apt install -y docker.io docker-compose-v2
cd monitor
docker compose up --build
```

Esto levanta el servidor (puerto 9000 expuesto al host) y un cliente que se
conecta a el por la red interna de Docker, generando metricas de forma
continua. Las metricas quedan persistidas en `monitor/data/metrics.csv` en el
host, gracias al volumen montado en `docker-compose.yml`.

Nota: dentro de un contenedor, `/proc` refleja el propio contenedor (su uso de
CPU, memoria y red), no necesariamente el host completo. Para recolectar
metricas del servidor Ubuntu Server como tal (y no solo del contenedor), el
cliente tambien puede compilarse y correrse directamente sobre el host, como
se describe en la seccion "Ejecutar" mas arriba, apuntando al servidor
expuesto por Docker en el puerto 9000.

## Analizar metricas

Una vez que `metrics.csv` tenga suficientes filas (idealmente varios minutos
de datos, incluyendo algun momento de carga alta para generar anomalias
reales, por ejemplo corriendo `stress` o `yes > /dev/null`):

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python3 analyze.py metrics.csv
```

El script imprime cuantas anomalias detecto y guarda el detalle en
`anomalies.csv`.

## Notas para el informe

- `metrics.csv` y `anomalies.csv` son los datos que respaldan la seccion
  "Resultados preliminares" y "Analisis de resultados" de la semana 14.
- Para generar una anomalia de prueba, se puede saturar CPU temporalmente:
  `sudo apt install stress -y && stress --cpu 2 --timeout 30`
