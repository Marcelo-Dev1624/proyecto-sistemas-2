"""
Modulo de deteccion de anomalias del monitor inteligente de sistema.

Lee las metricas recolectadas por el servidor (metrics.csv) y aplica
Isolation Forest para identificar observaciones que se comportan de
forma distinta al resto (posibles anomalias: picos de CPU, memoria,
procesos o trafico de red).

Uso:
    python3 analyze.py metrics.csv
"""

import sys
import pandas as pd
from sklearn.ensemble import IsolationForest

FEATURES = ["cpu_pct", "mem_pct", "proc_count", "net_bps"]


def load_metrics(path):
    df = pd.read_csv(path)
    df = df.dropna(subset=FEATURES)
    return df


def detect_anomalies(df, contamination=0.05):
    model = IsolationForest(
        n_estimators=200,
        contamination=contamination,
        random_state=42,
    )
    df = df.copy()
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
        cols = ["recv_ts", "client_ip"] + FEATURES
        print(anomalies[cols].to_string(index=False))

    out_path = "anomalies.csv"
    anomalies.to_csv(out_path, index=False)
    print(f"\nDetalle guardado en {out_path}")


if __name__ == "__main__":
    main()
