"""
Modulo de deteccion de anomalias del monitor inteligente de sistema.

El servidor (server.c) va guardando en metrics.csv todas las metricas
que le llegan de los clientes. Este script toma ese archivo y le aplica
Isolation Forest, un modelo de aprendizaje automatico pensado justamente
para encontrar observaciones que se comportan distinto al resto de los
datos, sin necesidad de definir reglas o umbrales fijos a mano.

La idea de fondo es simple. Un pico raro de CPU, memoria, cantidad de
procesos o trafico de red deberia quedar "aislado" del resto de los
datos con menos esfuerzo que una observacion normal, y eso es justo lo
que el algoritmo aprovecha para detectarlo.

Uso:
    python3 analyze.py metrics.csv
"""

import sys
import pandas as pd
from sklearn.ensemble import IsolationForest

# Estas son las columnas del CSV que realmente se usan como entrada del
# modelo. El resto de las columnas (por ejemplo la hora o la IP del
# cliente) son solo informativas y no entran en el calculo.
FEATURES = ["cpu_pct", "mem_pct", "proc_count", "net_bps"]


def load_metrics(path):
    """Carga el CSV de metricas y descarta filas incompletas."""
    df = pd.read_csv(path)
    df = df.dropna(subset=FEATURES)
    return df


def detect_anomalies(df, contamination=0.05):
    """
    Entrena Isolation Forest sobre las metricas recolectadas y marca
    cuales observaciones se consideran anomalas.

    El parametro contamination le dice al modelo, de forma aproximada,
    que proporcion de los datos se espera que sean anomalias (por
    defecto un 5%). No es un numero exacto, es mas bien una guia, y es
    uno de los primeros valores que conviene ajustar si el modelo
    marca de mas o de menos comparado con lo que se observa a simple
    vista en los datos.
    """
    model = IsolationForest(
        n_estimators=200,
        contamination=contamination,
        random_state=42,
    )
    df = df.copy()
    # fit_predict entrena el modelo y de una vez devuelve la
    # clasificacion de cada fila: -1 para anomalia, 1 para
    # comportamiento normal.
    df["anomaly_score"] = model.fit_predict(df[FEATURES])
    df["is_anomaly"] = df["anomaly_score"] == -1
    return df, model


def main():
    if len(sys.argv) < 2:
        print(f"Uso: python3 {sys.argv[0]} metrics.csv")
        sys.exit(1)

    path = sys.argv[1]
    df = load_metrics(path)

    if df.empty:
        print("No hay suficientes datos en el archivo de metricas todavia.")
        sys.exit(0)

    result, _ = detect_anomalies(df)

    total = len(result)
    anomalies = result[result["is_anomaly"]]

    print(f"Registros analizados: {total}")
    print(f"Anomalias detectadas: {len(anomalies)}")
    print()

    if not anomalies.empty:
        # Mostramos solo las columnas relevantes para que sea facil
        # leer de un vistazo que paso y en que momento.
        cols = ["recv_ts", "client_ip"] + FEATURES
        print(anomalies[cols].to_string(index=False))

    # Ademas de mostrarlo en pantalla, dejamos el detalle guardado en
    # un archivo aparte, para poder usarlo despues como evidencia en el
    # informe o en la presentacion.
    out_path = "anomalies.csv"
    anomalies.to_csv(out_path, index=False)
    print(f"\nDetalle guardado en {out_path}")


if __name__ == "__main__":
    main()
