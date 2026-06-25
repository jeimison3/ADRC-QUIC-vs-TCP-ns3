#!/usr/bin/env python3
"""
Plot — resultados do orquestrador de experimentos.

Lê a árvore scratch/RESULTADOS/ gerada pelo orchestrator.py e produz
gráficos comparativos entre TCP/TLS e QUIC nas 6 dimensões do experimento.

Uso:
  python3 plot-results.py                              # pasta default
  python3 plot-results.py -d scratch/RESULTADOS
  python3 plot-results.py -d . -o meus-graficos
  python3 plot-results.py --experimento Ethernet/30Mbps/perda-0
"""

import argparse
import csv
import glob
import os
import re
import sys
from collections import defaultdict

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ---------------------------------------------------------------------------
# Constantes
# ---------------------------------------------------------------------------
plt.rcParams.update({
    "figure.dpi": 120,
    "savefig.dpi": 150,
    "savefig.bbox": "tight",
    "font.size": 11,
    "axes.titlesize": 13,
    "axes.labelsize": 12,
})

COR = {"TCP": "#2196F3", "QUIC": "#4CAF50"}
MARCA = {"TCP": "o", "QUIC": "^"}
ROTULO_PROTO = {"TCP": "TCP/TLS", "QUIC": "QUIC"}

# ---------------------------------------------------------------------------
# Carga dos dados
# ---------------------------------------------------------------------------

# Mapeamento pt → en para colunas do summary.csv
PT_EN = {
    "Protocolo": "Protocol", "Execucao": "Run",
    "Sucessos": "Sucessos", "Tentativas": "Tentativas",
    "TaxaSucesso": "TaxaSucesso",
    "LatenciaMediaMs": "LatenciaMediaMs",
    "LatenciaMinMs": "LatenciaMinMs",
    "LatenciaMaxMs": "LatenciaMaxMs",
    "GargaloBw": "GargaloBw",
    "GargaloDelay": "GargaloDelay",
    "GargaloErro": "GargaloErro",
    "ServidorBw": "ServidorBw",
    "ServidorDelay": "ServidorDelay",
    "AcessoBw": "AcessoBw",
    "AcessoDelay": "AcessoDelay",
    "WiFi": "WiFi",
    "WiFiPadrao": "WiFiPadrao",
    "WiFiTaxa": "WiFiTaxa",
    "DisciplinaFila": "DisciplinaFila",
    "TraficoFundo": "TraficoFundo",
    "TaxaFundo": "TaxaFundo",
    "NosFundo": "NosFundo",
    "NumClientes": "NumClientes",
    "ConexoesPorCliente": "ConexoesPorCliente",
    "IntervaloConexoes": "IntervaloConexoes",
    "TimeoutHandshake": "TimeoutHandshake",
    "Duracao": "Duracao",
    "Fluxos0RTT": "Fluxos0RTT",
}

# Expressão regular para extrair parâmetros do caminho
RE_CAMINHO = re.compile(
    r"(Ethernet|WiFi5GHz)"
    r"/(\d+Mbps)"
    r"/perda-([\d.]+)"
    r"/(QUIC|TCP-TLS)"
    r"/clientes-(\d+)"
    r"/fundo-(\d+)"
)


def extrai_parametros(caminho: str) -> dict:
    """Extrai parâmetros do caminho do diretório."""
    m = RE_CAMINHO.search(caminho)
    if not m:
        return {}
    return {
        "Tecnologia": m.group(1),
        "Banda": m.group(2),
        "Perda": float(m.group(3)),
        "ProtocoloExp": m.group(4),
        "ClientesAtivos": int(m.group(5)),
        "ClientesFundo": int(m.group(6)),
    }


def normaliza(linha: dict) -> dict:
    """Normaliza chaves pt → en."""
    return {PT_EN.get(k, k): v for k, v in linha.items()}


def carrega(caminho_dir: str) -> list[dict]:
    """Carrega todos os summary.csv recursivamente."""
    padrao = os.path.join(caminho_dir, "**", "summary.csv")
    arquivos = sorted(glob.glob(padrao, recursive=True))
    if not arquivos:
        print(f"Nenhum summary.csv encontrado em {caminho_dir}")
        sys.exit(1)

    todas = []
    for f in arquivos:
        try:
            with open(f) as fh:
                for linha in csv.DictReader(fh):
                    r = normaliza(linha)
                    # Extrai parâmetros do caminho ANTES da conversão
                    params = extrai_parametros(f)
                    r.update(params)
                    # Converte numéricos
                    for chave in ("TaxaSucesso", "LatenciaMediaMs", "LatenciaMinMs",
                                  "LatenciaMaxMs", "GargaloErro", "Duracao"):
                        try:
                            r[chave] = float(r[chave])
                        except (ValueError, TypeError):
                            r[chave] = 0.0
                    for chave in ("NumClientes", "ClientesFundo", "ClientesAtivos",
                                  "NosFundo", "Sucessos", "Tentativas", "WiFi",
                                  "TraficoFundo"):
                        try:
                            r[chave] = int(float(r[chave]))
                        except (ValueError, TypeError):
                            r[chave] = 0
                    # Adiciona nome legível do cenário
                r["Cenario"] = "/".join([
                    params.get("Tecnologia", "?"),
                    params.get("Banda", "?"),
                    f"perda-{params.get('Perda', '?')}",
                    params.get("ProtocoloExp", "?"),
                ])
                todas.append(r)
        except Exception as e:
            print(f"  Aviso: {f}: {e}")

    if not todas:
        print("Nenhum dado válido encontrado")
        sys.exit(1)

    # Converte Chave_Protocolo para nome curto
    for r in todas:
        p = r.get("ProtocoloExp", "")
        if p == "TCP-TLS":
            r["Protocol"] = "TCP"
        elif p == "QUIC":
            r["Protocol"] = "QUIC"
        else:
            r["Protocol"] = r.get("ProtocoloExp", r.get("Protocol", "?"))

    # ConverteWiFi para rótulo
    for r in todas:
        r["RotuloWiFi"] = "WiFi" if r.get("WiFi") else "Ethernet"

    print(f"Carregados {len(arquivos)} arquivos, {len(todas)} linhas")
    return todas


def filtra(dados: list[dict], **kwargs) -> list[dict]:
    """Filtra dados por pares chave=valor."""
    resultado = dados
    for k, v in kwargs.items():
        resultado = [r for r in resultado if r.get(k) == v]
    return resultado


def extrai(dados: list[dict], chave: str) -> np.ndarray:
    return np.array([r[chave] for r in dados if chave in r and r[chave] is not None])


def media_std(vals: np.ndarray):
    if len(vals) == 0:
        return 0, 0
    return float(np.mean(vals)), float(np.std(vals))


def ordena_por(vals, chaves):
    """Ordena chaves pela média do grupo."""
    medias = {k: np.mean([v for v in vals if v is not None]) for k in vals}
    return sorted(chaves, key=lambda x: medias.get(x, 0), reverse=True)


# ===================================================================
# GRÁFICOS
# ===================================================================

def salva(fig, nome, outdir):
    path = os.path.join(outdir, nome)
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


def grafico_barras_sucesso(dados: list[dict], outdir: str):
    """Barra: taxa de sucesso TCP vs QUIC (média geral)."""
    fig, ax = plt.subplots(figsize=(7, 5))
    medias, desvios, rotulos = [], [], []
    for proto in ["TCP", "QUIC"]:
        sub = filtra(dados, Protocol=proto)
        vals = extrai(sub, "TaxaSucesso")
        if len(vals) == 0:
            continue
        m, s = media_std(vals)
        medias.append(m)
        desvios.append(s)
        rotulos.append(ROTULO_PROTO.get(proto, proto))
    x = range(len(rotulos))
    ax.bar(x, medias, yerr=desvios, capsize=8,
           color=[COR.get(p, "#999") for p in ["TCP", "QUIC"]],
           edgecolor="white", width=0.5)
    for i, (m, s) in enumerate(zip(medias, desvios)):
        ax.text(i, m + s + 1, f"{m:.1f}%", ha="center", fontweight="bold")
    ax.set_xticks(x)
    ax.set_xticklabels(rotulos)
    ax.set_ylabel("Taxa de Sucesso (%)")
    ax.set_title("Taxa de Sucesso — TCP/TLS vs QUIC\n(média sobre todos os experimentos)")
    ax.set_ylim(0, 110)
    ax.grid(axis="y", alpha=0.3)
    salva(fig, "01-sucesso-geral.png", outdir)


def grafico_barras_latencia(dados: list[dict], outdir: str):
    """Barra: latência média TCP vs QUIC."""
    fig, ax = plt.subplots(figsize=(7, 5))
    medias, desvios, rotulos = [], [], []
    for proto in ["TCP", "QUIC"]:
        sub = filtra(dados, Protocol=proto)
        vals = extrai(sub, "LatenciaMediaMs")
        vals = vals[vals > 0]
        if len(vals) == 0:
            continue
        m, s = media_std(vals)
        medias.append(m)
        desvios.append(s)
        rotulos.append(ROTULO_PROTO.get(proto, proto))
    x = range(len(rotulos))
    ax.bar(x, medias, yerr=desvios, capsize=8,
           color=[COR.get(p, "#999") for p in ["TCP", "QUIC"]],
           edgecolor="white", width=0.5)
    for i, (m, s) in enumerate(zip(medias, desvios)):
        ax.text(i, m + s + 1, f"{m:.1f} ms", ha="center", fontweight="bold")
    ax.set_xticks(x)
    ax.set_xticklabels(rotulos)
    ax.set_ylabel("Latência Média (ms)")
    ax.set_title("Latência de Handshake — TCP/TLS vs QUIC\n(HANDSHAKE + TLS)")
    ax.grid(axis="y", alpha=0.3)
    salva(fig, "02-latencia-geral.png", outdir)


def grafico_sucesso_vs_perda(dados: list[dict], outdir: str):
    """Linha: taxa de sucesso × perda para cada protocolo."""
    fig, ax = plt.subplots(figsize=(8, 5))
    for proto in ["TCP", "QUIC"]:
        sub = filtra(dados, Protocol=proto)
        niveis = sorted(set(r["Perda"] for r in sub))
        medias, desvios, xs = [], [], []
        for p in niveis:
            vals = extrai(filtra(sub, Perda=p), "TaxaSucesso")
            if len(vals) == 0:
                continue
            m, s = media_std(vals)
            medias.append(m)
            desvios.append(s)
            xs.append(p * 100)  # converte para %
        ax.errorbar(xs, medias, yerr=desvios,
                    marker=MARCA.get(proto, "o"),
                    color=COR.get(proto, "#999"),
                    capsize=5, label=ROTULO_PROTO.get(proto, proto),
                    linewidth=2, markersize=8)
    ax.set_xlabel("Perda de Pacotes (%)")
    ax.set_ylabel("Taxa de Sucesso (%)")
    ax.set_title("Taxa de Sucesso × Perda de Pacotes")
    ax.legend()
    ax.grid(alpha=0.3)
    ax.set_ylim(0, 110)
    ax.set_xlim(-1, 18)
    salva(fig, "03-sucesso-vs-perda.png", outdir)


def grafico_sucesso_vs_banda(dados: list[dict], outdir: str):
    """Barra agrupada: taxa de sucesso × banda (30 vs 100 Mbps)."""
    fig, ax = plt.subplots(figsize=(8, 5))
    bandas = ["30Mbps", "100Mbps"]
    largura = 0.35
    x = np.arange(len(protos := ["TCP", "QUIC"]))
    for i, banda in enumerate(bandas):
        medias = []
        for proto in protos:
            sub = filtra(dados, Protocol=proto, Banda=banda)
            vals = extrai(sub, "TaxaSucesso")
            medias.append(np.mean(vals) if len(vals) > 0 else 0)
        offset = (i - 0.5) * largura
        ax.bar(x + offset, medias, largura, label=banda,
               color=["#90CAF9", "#A5D6A7"][i], edgecolor="white")
    ax.set_xticks(x)
    ax.set_xticklabels([ROTULO_PROTO.get(p, p) for p in protos])
    ax.set_ylabel("Taxa de Sucesso (%)")
    ax.set_title("Taxa de Sucesso × Largura de Banda")
    ax.legend(title="Banda")
    ax.grid(axis="y", alpha=0.3)
    salva(fig, "04-sucesso-vs-banda.png", outdir)


def grafico_sucesso_vs_wifi(dados: list[dict], outdir: str):
    """Barra agrupada: Ethernet vs WiFi."""
    fig, ax = plt.subplots(figsize=(8, 5))
    modos = ["ethernet", "wifi"]
    # Filtra por RotuloWiFi
    rot_wifi = ["Ethernet", "WiFi"]
    largura = 0.35
    x = np.arange(len(protos := ["TCP", "QUIC"]))
    for i, modo in enumerate(rot_wifi):
        medias = []
        for proto in protos:
            sub = filtra(dados, Protocol=proto, RotuloWiFi=modo)
            vals = extrai(sub, "TaxaSucesso")
            medias.append(np.mean(vals) if len(vals) > 0 else 0)
        offset = (i - 0.5) * largura
        ax.bar(x + offset, medias, largura, label=modo,
               color=["#FFAB91", "#CE93D8"][i], edgecolor="white")
    ax.set_xticks(x)
    ax.set_xticklabels([ROTULO_PROTO.get(p, p) for p in protos])
    ax.set_ylabel("Taxa de Sucesso (%)")
    ax.set_title("Taxa de Sucesso — Ethernet vs WiFi")
    ax.legend(title="Enlace")
    ax.grid(axis="y", alpha=0.3)
    salva(fig, "05-sucesso-vs-wifi.png", outdir)


def grafico_sucesso_vs_clientes(dados: list[dict], outdir: str):
    """Linha: sucesso × número de clientes ativos."""
    fig, ax = plt.subplots(figsize=(8, 5))
    for proto in ["TCP", "QUIC"]:
        sub = filtra(dados, Protocol=proto)
        niveis = sorted(set(r["ClientesAtivos"] for r in sub))
        medias, xs = [], []
        for n in niveis:
            vals = extrai(filtra(sub, ClientesAtivos=n), "TaxaSucesso")
            if len(vals) == 0:
                continue
            medias.append(np.mean(vals))
            xs.append(n)
        ax.plot(xs, medias, marker=MARCA.get(proto, "o"),
                color=COR.get(proto, "#999"),
                label=ROTULO_PROTO.get(proto, proto),
                linewidth=2, markersize=8)
    ax.set_xlabel("Clientes Ativos")
    ax.set_ylabel("Taxa de Sucesso (%)")
    ax.set_title("Taxa de Sucesso × Clientes Ativos")
    ax.legend()
    ax.grid(alpha=0.3)
    ax.set_ylim(0, 110)
    salva(fig, "06-sucesso-vs-clientes.png", outdir)


def grafico_sucesso_vs_fundo(dados: list[dict], outdir: str):
    """Linha: sucesso × número de nós de fundo (30, 100, 240)."""
    fig, ax = plt.subplots(figsize=(8, 5))
    for proto in ["TCP", "QUIC"]:
        sub = filtra(dados, Protocol=proto)
        niveis = sorted(set(r["ClientesFundo"] for r in sub))
        medias, xs = [], []
        for n in niveis:
            vals = extrai(filtra(sub, ClientesFundo=n), "TaxaSucesso")
            if len(vals) == 0:
                continue
            medias.append(np.mean(vals))
            xs.append(n)
        ax.plot(xs, medias, marker=MARCA.get(proto, "o"),
                color=COR.get(proto, "#999"),
                label=ROTULO_PROTO.get(proto, proto),
                linewidth=2, markersize=8)
    ax.set_xlabel("Nós de Tráfego de Fundo")
    ax.set_ylabel("Taxa de Sucesso (%)")
    ax.set_title("Taxa de Sucesso × Tráfego de Fundo")
    ax.legend()
    ax.grid(alpha=0.3)
    ax.set_ylim(0, 110)
    salva(fig, "07-sucesso-vs-fundo.png", outdir)


def grafico_latencia_vs_perda(dados: list[dict], outdir: str):
    """Linha: latência × perda."""
    fig, ax = plt.subplots(figsize=(8, 5))
    for proto in ["TCP", "QUIC"]:
        sub = [r for r in filtra(dados, Protocol=proto) if r.get("LatenciaMediaMs", 0) > 0]
        niveis = sorted(set(r["Perda"] for r in sub))
        medias, xs = [], []
        for p in niveis:
            vals = extrai(filtra(sub, Perda=p), "LatenciaMediaMs")
            if len(vals) == 0:
                continue
            medias.append(np.mean(vals))
            xs.append(p * 100)
        ax.plot(xs, medias, marker=MARCA.get(proto, "o"),
                color=COR.get(proto, "#999"),
                label=ROTULO_PROTO.get(proto, proto),
                linewidth=2, markersize=8)
    ax.set_xlabel("Perda de Pacotes (%)")
    ax.set_ylabel("Latência Média (ms)")
    ax.set_title("Latência de Handshake × Perda de Pacotes")
    ax.legend()
    ax.grid(alpha=0.3)
    salva(fig, "08-latencia-vs-perda.png", outdir)


def grafico_mapa_cenarios(dados: list[dict], outdir: str):
    """Heatmap: sucesso por cenário e protocolo."""
    cenarios = sorted(set(r["Cenario"] for r in dados), key=lambda x: x.split("/")[::-1])
    protocolos = ["TCP", "QUIC"]
    grid = np.zeros((len(cenarios), len(protocolos)))
    for i, c in enumerate(cenarios):
        for j, p in enumerate(protocolos):
            sub = filtra(dados, Cenario=c, Protocol=p)
            vals = extrai(sub, "TaxaSucesso")
            grid[i, j] = np.mean(vals) if len(vals) > 0 else np.nan

    fig, ax = plt.subplots(figsize=(max(8, len(protocolos) * 2.5),
                                    max(6, len(cenarios) * 0.5)))
    im = ax.imshow(grid, cmap="RdYlGn", aspect="auto", vmin=0, vmax=100)
    for i in range(len(cenarios)):
        for j in range(len(protocolos)):
            val = grid[i, j]
            if not np.isnan(val):
                ax.text(j, i, f"{val:.0f}%", ha="center", va="center",
                        fontweight="bold", fontsize=9,
                        color="white" if val < 50 else "black")
    ax.set_xticks(range(len(protocolos)))
    ax.set_xticklabels([ROTULO_PROTO.get(p, p) for p in protocolos], fontsize=10)
    ax.set_yticks(range(len(cenarios)))
    ax.set_yticklabels([c.replace("/", "\n") for c in cenarios], fontsize=8)
    ax.set_title("Taxa de Sucesso — Protocolo × Cenário", fontweight="bold")
    fig.colorbar(im, ax=ax, shrink=0.8, label="Sucesso %")
    salva(fig, "09-mapa-cenarios.png", outdir)


def grafico_boxplot_latencia(dados: list[dict], outdir: str):
    """Boxplot: distribuição da latência TCP vs QUIC."""
    data, rotulos = [], []
    for proto in ["TCP", "QUIC"]:
        vals = extrai(
            [r for r in dados if r.get("Protocol") == proto and r.get("LatenciaMediaMs", 0) > 0],
            "LatenciaMediaMs"
        )
        if len(vals) > 0:
            data.append(vals)
            rotulos.append(ROTULO_PROTO.get(proto, proto))

    if not data:
        return

    fig, ax = plt.subplots(figsize=(7, 5))
    bp = ax.boxplot(data, labels=rotulos, patch_artist=True,
                    widths=0.4, showmeans=True,
                    meanprops={"marker": "D", "markerfacecolor": "red", "markersize": 6})
    for patch, p in zip(bp["boxes"], rotulos):
        cor = COR.get("TCP" if "TCP" in p else "QUIC", "#999")
        patch.set_facecolor(cor)
        patch.set_alpha(0.7)
    ax.set_ylabel("Latência (ms)")
    ax.set_title("Distribuição da Latência de Handshake")
    ax.grid(axis="y", alpha=0.3)
    salva(fig, "10-boxplot-latencia.png", outdir)


def grafico_latencia_vs_cenario(dados: list[dict], outdir: str):
    """Barra: latência por cenário e protocolo."""
    cenarios = sorted(set(r["Cenario"] for r in dados), key=lambda x: x.split("/")[::-1])
    protocolos = ["TCP", "QUIC"]
    largura = 0.35
    x = np.arange(len(cenarios))

    fig, ax = plt.subplots(figsize=(max(10, len(cenarios) * 0.6), 5))
    for i, proto in enumerate(protocolos):
        medias = []
        for c in cenarios:
            sub = filtra(dados, Cenario=c, Protocol=proto)
            vals = extrai([r for r in sub if r.get("LatenciaMediaMs", 0) > 0], "LatenciaMediaMs")
            medias.append(np.mean(vals) if len(vals) > 0 else 0)
        offset = (i - 0.5) * largura
        ax.bar(x + offset, medias, largura, label=ROTULO_PROTO.get(proto, proto),
               color=COR.get(proto, "#999"), edgecolor="white", alpha=0.85)
    ax.set_xticks(x)
    ax.set_xticklabels([c.replace("/", "\n") for c in cenarios], fontsize=8, rotation=20)
    ax.set_ylabel("Latência Média (ms)")
    ax.set_title("Latência de Handshake por Cenário")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    salva(fig, "11-latencia-vs-cenario.png", outdir)


# ===================================================================
# MAIN
# ===================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Gráficos dos experimentos TCP/TLS vs QUIC")
    parser.add_argument("-d", "--directory",
                        default=os.path.join(
                            os.path.dirname(os.path.abspath(__file__)), "RESULTADOS"),
                        help="Diretório com summary.csv (recursivo)")
    parser.add_argument("-o", "--output",
                        default=os.path.join(
                            os.path.dirname(os.path.abspath(__file__)), "plots"),
                        help="Diretório de saída para os gráficos")
    args = parser.parse_args()

    os.makedirs(args.output, exist_ok=True)

    dados = carrega(args.directory)

    # Sumário
    print(f"\nProtocolos encontrados: {set(r['Protocol'] for r in dados)}")
    print(f"Tecnologias: {set(r.get('Tecnologia', '?') for r in dados)}")
    print(f"Cenários: {len(set(r['Cenario'] for r in dados))}")

    print("\nGerando gráficos...")
    grafico_barras_sucesso(dados, args.output)
    grafico_barras_latencia(dados, args.output)
    grafico_sucesso_vs_perda(dados, args.output)
    grafico_sucesso_vs_banda(dados, args.output)
    grafico_sucesso_vs_wifi(dados, args.output)
    grafico_sucesso_vs_clientes(dados, args.output)
    grafico_sucesso_vs_fundo(dados, args.output)
    grafico_latencia_vs_perda(dados, args.output)
    grafico_mapa_cenarios(dados, args.output)
    grafico_boxplot_latencia(dados, args.output)
    grafico_latencia_vs_cenario(dados, args.output)

    n = len([f for f in os.listdir(args.output) if f.endswith(".png")])
    print(f"\nConcluído — {n} gráficos salvos em {args.output}/")


if __name__ == "__main__":
    main()
