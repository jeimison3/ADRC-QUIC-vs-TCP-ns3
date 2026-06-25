#!/usr/bin/env python3
"""
Orquestrador paralelo de experimentos — TCP/TLS vs QUIC no ns-3

Usa todos os núcleos da CPU (-1) para executar simulações em paralelo.

Uso:
  python3 orchestrator.py                         # todos os 144 experimentos
  python3 orchestrator.py --dry-run               # só mostra o que seria executado
  python3 orchestrator.py --max-runs 10           # executa só os 10 primeiros
  python3 orchestrator.py --resume                # retoma de onde parou
  python3 orchestrator.py --parallel 8            # 8 simulações simultâneas
"""

import subprocess
import os
import sys
import time
import itertools
import math
from datetime import datetime
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

# ===========================================================================
# CONSTANTES MODIFICÁVEIS
# ===========================================================================

DURACAO = 30
TLS_PAYLOAD = 3000
CONN_PER_CLIENT = 1   # ⚠ QUIC: manter =1 (bug 0-RTT no ns-3.47)
CONN_INTERVAL = 0.5
CONN_TIMEOUT = 5
BG_TOTAL_RATE = "50Mbps"   # largura total de fundo (dividida entre numBgNodes)
ACCESS_BW = "10Mbps"      # enlace de acesso dos clientes (Ethernet)
BOTTLENECK_BW = "100Mbps"    # gargalo
SERV_DELAY = "5ms"
ACCESS_DELAY = "2ms"
GARGALO_DELAY = "20ms"
WIFI_STANDARD = "80211ac"
WIFI_RATE = "VhtMcs7"
WIFI_DISTANCE = 20

# ===========================================================================
# PARÂMETROS DOS LOOPS ANINHADOS (outer → inner)
# ===========================================================================

LINK_TECH = ["Ethernet", "WiFi5GHz"]
LOSSES = ["0", "0.03", "0.15"]
PROTOCOLS = ["QUIC", "TCP-TLS"]
ACTIVE_CLIENTS = ["5", "45"]
BG_CLIENTS = ["30", "100", "240"]

# ===========================================================================
# DERIVAÇÕES
# ===========================================================================

NS3_DIR = Path(__file__).resolve().parent.parent


def build_output_dir(link_tech: str, loss: str,
                     protocol: str, active: str, bg: str) -> str:
    base = Path(__file__).resolve().parent / "RESULTADOS"
    subdir = f"{link_tech}/perda-{loss}/{protocol}/clientes-{active}/fundo-{bg}"
    return str(base / subdir)


def build_ns3_args(output_dir: str,
                   link_tech: str, loss: str,
                   protocol: str, active: str, bg: str) -> list[str]:
    args = ["./ns3", "run", "connection-success"]
    sim_args = [
        f"--duration={DURACAO}",
        f"--connPerClient={CONN_PER_CLIENT}",
        f"--connInterval={CONN_INTERVAL}",
        f"--connTimeout={CONN_TIMEOUT}",
        f"--bottleneckBw={BOTTLENECK_BW}",
        f"--bottleneckDelay={GARGALO_DELAY}",
        f"--bottleneckError={loss}",
        f"--serverDelay={SERV_DELAY}",
        f"--accessBw={ACCESS_BW}",
        f"--accessDelay={ACCESS_DELAY}",
        f"--outputDir={output_dir}",
    ]
    if "Ethernet" in link_tech:
        sim_args.append("--enableWifi=0")
    else:
        sim_args.extend([
            "--enableWifi=1",
            f"--wifiStandard={WIFI_STANDARD}",
            f"--wifiRate={WIFI_RATE}",
            f"--wifiDistance={WIFI_DISTANCE}",
        ])
    if "QUIC" in protocol:
        sim_args.extend(["--enableQuic=1", f"--numQuicClients={active}",
                         "--numTcpClients=0", "--tlsPayloadSize=0"])
    else:
        sim_args.extend(["--enableQuic=0", f"--numTcpClients={active}",
                         "--numQuicClients=0", f"--tlsPayloadSize={TLS_PAYLOAD}"])
    sim_args.extend(["--background=1", f"--numBgNodes={bg}",
                     f"--bgTotalRate={BG_TOTAL_RATE}"])
    args.append("--")
    args.extend(sim_args)
    return args


# ===========================================================================
# EXECUÇÃO DE UM EXPERIMENTO
# ===========================================================================

def executa_experimento(comando: list[str], output_dir: str):
    """Executa um experimento ns-3. Retorna (True/False, output_dir)."""
    os.makedirs(output_dir, exist_ok=True)
    with open(os.path.join(output_dir, "command.txt"), "w") as f:
        f.write(" ".join(comando) + "\n")
    try:
        result = subprocess.run(
            comando, cwd=NS3_DIR,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            # timeout=DURACAO * 4 + 30,
        )
        if result.returncode != 0:
            with open(os.path.join(output_dir, "ERROR.log"), "w") as f:
                f.write(result.stderr or "")
            return False, output_dir
        return True, output_dir
    except subprocess.TimeoutExpired:
        return False, output_dir


def descricao_experimento(link: str, loss: str,
                          proto: str, active: str, bg: str, idx: int, total: int) -> str:
    loss_pct = float(loss) * 100
    return (f"[{idx+1:{len(str(total))}}/{total}] "
            f"{link:10s} perda={loss_pct:3.0f}% "
            f"{proto:7s} clientes={active:>2s} fundo={bg:>3s}")


# ===========================================================================
# MAIN PARALELA
# ===========================================================================

def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="Orquestrador paralelo de experimentos TCP/TLS vs QUIC")
    parser.add_argument("--dry-run", action="store_true",
                        help="Apenas mostra os comandos")
    parser.add_argument("--max-runs", type=int, default=0,
                        help="Máximo de experimentos (0 = todos)")
    parser.add_argument("--resume", action="store_true",
                        help="Pula experimentos já concluídos")
    parser.add_argument("--from-idx", type=int, default=0,
                        help="Começa do índice N")
    parser.add_argument("--parallel", type=int, default=0,
                        help="Simulações simultâneas (0 = autodetect: CPUs-1)")
    args = parser.parse_args()

    # Detecta número de CPUs
    n_cpus = os.cpu_count() or 4
    n_parallel = args.parallel if args.parallel > 0 else max(1, n_cpus - 1)
    MEM_WARN = 0.5

    # Validação: QUIC + connPerClient > 1 crasha no ns-3.47
    if CONN_PER_CLIENT > 1:
        print(f"⚠  CONN_PER_CLIENT={CONN_PER_CLIENT} com QUIC causa crash "
              "(bug 0-RTT no módulo QUIC do ns-3.47).")
        print("   Experimentos QUIC usarão 1 conexão por cliente.")
        print("   Para mais amostras, aumente --numQuicClients.\n")  # GB estimados por simulação paralela
    if n_parallel * MEM_WARN > 8:
        print(f"⚠  {n_parallel} paralelos ~{n_parallel * MEM_WARN:.0f} GB de RAM (est.)")

    # Gera combinações (gargalo fixo em BOTTLENECK_BW, acesso fixo em ACCESS_BW)
    combos = list(itertools.product(
        LINK_TECH, LOSSES, PROTOCOLS, ACTIVE_CLIENTS, BG_CLIENTS
    ))
    total = len(combos)

    print(f"\n{'='*60}")
    print(f"  Orquestrador Paralelo de Experimentos")
    print(f"  {total} experimentos  (gargalo={BOTTLENECK_BW}, acesso={ACCESS_BW})")
    print(f"  Paralelismo: {n_parallel} simulações simultâneas ({n_cpus} CPUs)")
    print(f"  Duração: {DURACAO}s simulados por experimento")
    print(f"  Dirs: {len(LINK_TECH)}×{len(LOSSES)}×"
          f"{len(PROTOCOLS)}×{len(ACTIVE_CLIENTS)}×{len(BG_CLIENTS)} = "
          f"{total} combinações")
    print(f"{'='*60}\n")

    # Filtra e prepara experimentos
    fila = []
    for idx, (link, loss, proto, active, bg) in enumerate(combos):
        if idx < args.from_idx:
            continue
        output_dir = build_output_dir(link, loss, proto, active, bg)
        if args.resume and os.path.exists(os.path.join(output_dir, "summary.csv")):
            continue
        comando = build_ns3_args(output_dir, link, loss, proto, active, bg)
        desc = descricao_experimento(link, loss, proto, active, bg, idx, total)
        fila.append((comando, output_dir, desc))

    if not fila:
        print("Nada a executar (tudo já concluído ou filtrado).")
        return

    if args.max_runs > 0:
        fila = fila[:args.max_runs]

    print(f"Fila de execução: {len(fila)} experimentos\n")

    if args.dry_run:
        for _, _, desc in fila:
            print(f"  [DRY] {desc}")
        print(f"\nTotal: {len(fila)} experimentos")
        return

    # -----------------------------------------------------------------------
    # Execução paralela
    # -----------------------------------------------------------------------
    total_fila = len(fila)
    concluidos = [0]  # lista mutável para closure
    acertos = [0]
    erros = [0]
    inicio = time.time()
    lock = __import__("threading").Lock()
    _print = __import__("threading").Lock()

    def exec_com_status(item):
        comando, output_dir, desc = item
        ok, _ = executa_experimento(comando, output_dir)
        with lock:
            concluidos[0] += 1
            feito = concluidos[0]
            if ok:
                acertos[0] += 1
            else:
                erros[0] += 1
        with _print:
            decorrer = time.time() - inicio
            restam = total_fila - feito
            eta = restam * (decorrer / feito) / 60 if feito > 0 else 0
            status = "✓" if ok else "✗"
            print(f"  {status} {desc}  ({feito}/{total_fila} | "
                  f"ETA: {eta:.0f}min)")
        return ok, output_dir

    # Submete ao pool de threads (1 thread = 1 subprocesso ns-3)
    with ThreadPoolExecutor(max_workers=n_parallel) as pool:
        futuros = [pool.submit(exec_com_status, item) for item in fila]
        for f in as_completed(futuros):
            pass  # Callbacks já tratam a saída

    # Sumário final
    decorrido = time.time() - inicio
    print(f"\n{'='*60}")
    print(f"  Concluído!")
    print(f"  Sucessos: {acertos[0]}  Erros: {erros[0]}")
    print(f"  Tempo: {decorrido/60:.1f} min ({decorrido:.0f}s)")
    if acertos[0] > 0:
        media = decorrido / acertos[0]
        ganho = media * total_fila / decorrido if decorrido > 0 else 1
        print(f"  Média/exp: {media:.1f}s  Ganho paralelo: ~{ganho:.1f}×")
    print(f"{'='*60}\n")


if __name__ == "__main__":
    main()
