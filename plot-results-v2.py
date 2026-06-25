#!/usr/bin/env python3
"""
Plot v2 — análise cruzada dos experimentos orquestrados.

Usa BARRAS AGRUPADAS (não linhas) porque os eixos têm poucos valores discretos.
Gera ~25 gráficos em subdiretórios temáticos.

Uso:
  python3 plot-results-v2.py
  python3 plot-results-v2.py -d scratch/RESULTADOS -o scratch/plots-v2
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
plt.rcParams.update({
    "figure.dpi": 120, "savefig.dpi": 150, "savefig.bbox": "tight",
    "font.size": 10, "axes.titlesize": 12, "axes.labelsize": 11,
})

COR = {"TCP": "#2196F3", "QUIC": "#4CAF50", "Ethernet": "#FF7043", "WiFi": "#AB47BC"}
ROT = {"TCP": "TCP/TLS", "QUIC": "QUIC"}
LARG = 0.32

# ---------------------------------------------------------------------------
RE_CAM = re.compile(
    r"(Ethernet|WiFi5GHz)"
    r"/perda-([\d.]+)"
    r"/(QUIC|TCP-TLS)"
    r"/clientes-(\d+)"
    r"/fundo-(\d+)")

def extrai_cam(c):
    m = RE_CAM.search(c)
    if not m: return {}
    return {"Tecnologia": m.group(1),
            "Perda": float(m.group(2)), "ProtocoloExp": m.group(3),
            "ClientesAtivos": int(m.group(4)), "ClientesFundo": int(m.group(5))}

PT_EN = {"Protocolo":"Protocol","TaxaSucesso":"TaxaSucesso",
         "LatenciaMediaMs":"LatenciaMediaMs","GargaloErro":"GargaloErro",
         "WiFi":"WiFi","NosFundo":"NosFundo","NumClientes":"NumClientes"}

def carrega(dir_):
    files = sorted(glob.glob(os.path.join(dir_,"**","summary.csv"), recursive=True))
    if not files: print(f"Nada em {dir_}"); sys.exit(1)
    rows = []
    for f in files:
        try:
            with open(f) as fh:
                for linha in csv.DictReader(fh):
                    r = {PT_EN.get(k,k):v for k,v in linha.items()}
                    r.update(extrai_cam(f))
                    for k in ("TaxaSucesso","LatenciaMediaMs","GargaloErro"):
                        try: r[k]=float(r[k])
                        except: r[k]=0.0
                    for k in ("ClientesAtivos","ClientesFundo","NumClientes"):
                        try: r[k]=int(float(r[k]))
                        except: r[k]=0
                    for k in ("Sucessos","Tentativas"):
                        try: r[k]=int(float(r[k]))
                        except: r[k]=0
                    exp=r.get("ProtocoloExp","")
                    # r["Protocol"] already set from CSV column via PT_EN mapping
                    csvProto = r.get("Protocol","")
                    # Fix: "WiFi" column is string "0"/"1", both truthy
                    wifi_val = r.get("WiFi","0")
                    r["RotWiFi"]="WiFi" if int(float(wifi_val)) else "Ethernet"
                    # Banda from CSV GargaloBw column (not from dir path anymore)
                    bw = r.get("GargaloBw","")
                    r["Banda"] = bw if bw else "?Mbps"
                    # Filter garbage rows: TCP=0/0 row in QUIC experiment folder
                    is_quic_exp = (exp == "QUIC")
                    is_tcp_exp = (exp == "TCP-TLS")
                    if csvProto == "TCP" and is_quic_exp and r.get("Tentativas",1)==0:
                        continue  # garbage TCP row in QUIC-only experiment
                    if csvProto == "QUIC" and is_tcp_exp and r.get("Tentativas",1)==0:
                        continue  # QUIC row in TCP-only experiment (shouldn't exist)
                    rows.append(r)
        except: pass
    print(f"  {len(files)} CSVs, {len(rows)} linhas")
    return rows

def filt(d,**kw):
    return [r for r in d if all(r.get(k)==v for k,v in kw.items())]

def ext(d,k):
    return np.array([r[k] for r in d if k in r and r[k] is not None])

def ms(v):
    if len(v)==0: return 0,0
    return float(np.mean(v)), float(np.std(v))

# ===================================================================
# GRÁFICO BASE: barras agrupadas
# ===================================================================

def barras_agrupadas(dados, eixo_x, grupos, y_attr, nome_x, nome_y, titulo,
                      outdir, nome_arq, cores=None, ylim=None, yfmt="{:.1f}"):
    """
    eixo_x: atributo para o eixo X (ex: "Perda", "Banda")
    grupos: atributo cujos valores serão barras lado a lado (ex: "Protocol")
    y_attr: métrica no eixo Y (ex: "TaxaSucesso")
    """
    vals_x = sorted(set(r[eixo_x] for r in dados if eixo_x in r),
                    key=lambda v: (isinstance(v,str), v))
    vals_g = sorted(set(r[grupos] for r in dados if grupos in r),
                    key=lambda v: (isinstance(v,str), v))

    # Prepara dados
    medias = {g: [] for g in vals_g}
    desvios = {g: [] for g in vals_g}
    for xv in vals_x:
        for g in vals_g:
            v = ext(filt(dados, **{eixo_x: xv, grupos: g}), y_attr)
            if len(v):
                medias[g].append(np.mean(v))
                desvios[g].append(np.std(v))
            else:
                medias[g].append(0)
                desvios[g].append(0)

    # Prep X-tick labels
    def rot_x(v):
        if eixo_x=="Perda": return f"{float(v)*100:.0f}%"
        if eixo_x=="Banda": return v.replace("Mbps","")  # "30", "100"
        return str(v)

    # Enforce sensible Y-axis limits
    if ylim is None:
        if y_attr in ("TaxaSucesso", "QoE"):
            ylim = (0, 100)
        elif y_attr == "LatenciaMediaMs":
            ylim = (0, None)
    # Clip error bars so they don't extend below 0 or above 100
    for g in vals_g:
        for i in range(len(desvios[g])):
            m = medias[g][i]
            d = desvios[g][i]
            if m - d < 0:
                desvios[g][i] = m
            if y_attr in ("TaxaSucesso", "QoE") and m + d > 100:
                desvios[g][i] = 100 - m

    fig, ax = plt.subplots(figsize=(max(7, len(vals_x)*2.5), 5.5))
    n_g = len(vals_g)
    x = np.arange(len(vals_x))

    for i, g in enumerate(vals_g):
        off = (i - (n_g-1)/2) * LARG
        cor = (cores or {}).get(g, f"C{i}")
        bars = ax.bar(x + off, medias[g], LARG, yerr=desvios[g],
                      capsize=4, label=ROT.get(g,g) if g in ROT else str(g),
                      color=cor, edgecolor="white", alpha=0.85)

    ax.set_xticks(x)
    ax.set_xticklabels([rot_x(v) for v in vals_x])
    ax.set_xlabel(nome_x)
    ax.set_ylabel(nome_y)
    ax.set_title(titulo)
    if ylim: ax.set_ylim(*ylim)
    ax.legend(fontsize=9)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    path = os.path.join(outdir, nome_arq)
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ===================================================================
# GRÁFICO: heatmap 2D
# ===================================================================

def heatmap(dados, eixo_x, eixo_y, y_attr, titulo, outdir, nome_arq,
            vmin=0, vmax=100, fmt="{:.0f}"):
    vals_x = sorted(set(r[eixo_x] for r in dados),
                    key=lambda v: (isinstance(v,str), v))
    vals_y = sorted(set(r[eixo_y] for r in dados),
                    key=lambda v: (isinstance(v,str), v))
    grid = np.zeros((len(vals_y), len(vals_x)))
    for i, yv in enumerate(vals_y):
        for j, xv in enumerate(vals_x):
            v = ext(filt(dados, **{eixo_x: xv, eixo_y: yv}), y_attr)
            grid[i,j] = np.mean(v) if len(v) else np.nan

    fig, ax = plt.subplots(figsize=(max(6, len(vals_x)*1.8), max(5, len(vals_y)*1.2)))
    im = ax.imshow(grid, cmap="RdYlGn", aspect="auto", vmin=vmin, vmax=vmax)
    for i in range(len(vals_y)):
        for j in range(len(vals_x)):
            val = grid[i,j]
            if not np.isnan(val):
                ax.text(j, i, fmt.format(val), ha="center", va="center",
                        fontweight="bold", fontsize=11,
                        color="white" if val < 50 else "black")
    def rt(v):
        if eixo_x=="Perda": return f"{v*100:.0f}%"
        if isinstance(v, str): return v
        return str(v)
    ax.set_xticks(range(len(vals_x)))
    ax.set_xticklabels([rt(v) for v in vals_x])
    ax.set_yticks(range(len(vals_y)))
    ax.set_yticklabels([str(v) for v in vals_y])
    ax.set_title(titulo)
    fig.colorbar(im, ax=ax, shrink=0.7)
    fig.tight_layout()
    path = os.path.join(outdir, nome_arq)
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ===================================================================
# GRÁFICO: boxplot por grupo
# ===================================================================

def boxplot_grupo(dados, grupos, y_attr, nome_y, titulo, outdir, nome_arq, cores=None):
    vals_g = sorted(set(r[grupos] for r in dados),
                    key=lambda v: (isinstance(v,str), v))
    data, labels = [], []
    for g in vals_g:
        v = ext(filt(dados, **{grupos: g}), y_attr)
        if len(v):
            data.append(v)
            labels.append(ROT.get(g,str(g)) if g in ROT else str(g))
    if not data: return
    fig, ax = plt.subplots(figsize=(max(6, len(data)*1.5), 5))
    bp = ax.boxplot(data, labels=labels, patch_artist=True, widths=0.4,
                    showmeans=True, meanprops={"marker":"D","markerfacecolor":"red","markersize":5})
    for patch, g in zip(bp["boxes"], vals_g):
        cor = (cores or {}).get(g, "#999")
        patch.set_facecolor(cor); patch.set_alpha(0.7)
    ax.set_ylabel(nome_y)
    ax.set_title(titulo)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    path = os.path.join(outdir, nome_arq)
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ===================================================================
# RELATÓRIO
# ===================================================================

# ===================================================================
# MÉTRICA QOE — Website Loading Experience
# ===================================================================
# Modelo: um site carrega via N conexões paralelas. A experiência depende de:
#   - quantas conexões são bem-sucedidas (TaxaSucesso)
#   - quão rápido cada handshake completa (LatenciaMediaMs)
#
# QoE = TaxaSucesso * exp(-LatenciaMediaMs / 200)
#   - 100% @ 0ms   -> 100.0
#   - 100% @ 200ms ->  36.8
#   -  50% @ 55ms  ->  38.0
#   -   0% @ any   ->   0.0

def qoe_score(taxa, lat):
    """Website loading experience score (0-100)."""
    if taxa <= 0 or lat < 0:
        return 0.0
    return taxa * np.exp(-lat / 200.0)

def add_qoe(dados):
    """Adiciona coluna QoE a cada linha."""
    for r in dados:
        r["QoE"] = qoe_score(r.get("TaxaSucesso",0), r.get("LatenciaMediaMs",0))
    return dados

def relatorio(dados, outdir):
    path = os.path.join(outdir, "resumo.txt")
    with open(path, "w") as f:
        f.write("=== RESUMO ===\n\n")
        f.write(f"Linhas: {len(dados)}\n")
        f.write(f"Protocolos: {set(r['Protocol'] for r in dados)}\n\n")
        for proto in ["TCP","QUIC"]:
            sub = filt(dados, Protocol=proto)
            sr = ext(sub, "TaxaSucesso")
            lt = ext([r for r in sub if r.get("LatenciaMediaMs",0)>0], "LatenciaMediaMs")
            qe = ext(sub, "QoE")
            f.write(f"{ROT.get(proto,proto)}: n={len(sub)}  "
                    f"sucesso={ms(sr)[0]:.1f}%  "
                    f"lat={ms(lt)[0]:.1f}ms  "
                    f"QoE={ms(qe)[0]:.1f}\n")
    print(f"  -> {path}")


# ===================================================================
# MAIN
# ===================================================================

def main():
    parser = argparse.ArgumentParser(description="Plot v2")
    parser.add_argument("-d","--directory",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "RESULTADOS"))
    parser.add_argument("-o","--output",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "plots-v2"))
    args = parser.parse_args()

    os.makedirs(args.output, exist_ok=True)
    dados = carrega(args.directory)
    dados_lat = [r for r in dados if r.get("LatenciaMediaMs",0) > 0]

    # Add QoE metric
    dados = add_qoe(dados)
    # Re-filter lat with QoE
    dados_lat = [r for r in dados if r.get("LatenciaMediaMs",0) > 0]

    # ===============================================================
    # RAÍZ — visão geral (Sucesso, Latência, QoE)
    # ===============================================================
    print("\nGeral...")

    # Sucesso geral
    fig, ax = plt.subplots(figsize=(6, 4.5))
    vals = [ext(filt(dados, Protocol=p), "TaxaSucesso") for p in ["TCP","QUIC"]]
    med = [np.mean(v) if len(v) else 0 for v in vals]
    std = [np.std(v) if len(v) else 0 for v in vals]
    ax.bar([0,1], med, yerr=std, capsize=6, color=[COR["TCP"], COR["QUIC"]],
           edgecolor="white", width=0.45, tick_label=["TCP/TLS", "QUIC"])
    ax.set_ylabel("Taxa de Sucesso (%)")
    ax.set_title("Taxa de Sucesso — TCP/TLS vs QUIC")
    ax.set_ylim(0, 110)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(os.path.join(args.output, "00-sucesso-geral.png"))
    plt.close(fig)
    print(f"  -> {args.output}/00-sucesso-geral.png")

    # Latência geral
    fig, ax = plt.subplots(figsize=(6, 4.5))
    vals = [ext(filt(dados_lat, Protocol=p), "LatenciaMediaMs") for p in ["TCP","QUIC"]]
    med = [np.mean(v) if len(v) else 0 for v in vals]
    std = [np.std(v) if len(v) else 0 for v in vals]
    ax.bar([0,1], med, yerr=std, capsize=6, color=[COR["TCP"], COR["QUIC"]],
           edgecolor="white", width=0.45, tick_label=["TCP/TLS", "QUIC"])
    ax.set_ylabel("Latência Média (ms)")
    ax.set_title("Latência de Handshake — TCP/TLS vs QUIC")
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(os.path.join(args.output, "01-latencia-geral.png"))
    plt.close(fig)
    print(f"  -> {args.output}/01-latencia-geral.png")

    # QoE geral (website loading experience)
    fig, ax = plt.subplots(figsize=(6, 4.5))
    vals = [ext(filt(dados, Protocol=p), "QoE") for p in ["TCP","QUIC"]]
    med = [np.mean(v) if len(v) else 0 for v in vals]
    std = [np.std(v) if len(v) else 0 for v in vals]
    ax.bar([0,1], med, yerr=std, capsize=6, color=[COR["TCP"], COR["QUIC"]],
           edgecolor="white", width=0.45, tick_label=["TCP/TLS", "QUIC"])
    ax.set_ylabel("QoE Score (0–100)")
    ax.set_title("Website Loading Experience — TCP/TLS vs QUIC\n(QoE = Sucesso × exp(-Latência/200ms))")
    ax.set_ylim(0, 110)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(os.path.join(args.output, "02-qoe-geral.png"))
    plt.close(fig)
    print(f"  -> {args.output}/02-qoe-geral.png")

    boxplot_grupo(dados_lat, "Protocol", "LatenciaMediaMs",
                  "Latência (ms)", "Distribuição da Latência — TCP/TLS vs QUIC",
                  args.output, "03-boxplot-latencia.png", cores=COR)

    # ===============================================================
    # 1. SUCESSO
    # ===============================================================
    subdir = os.path.join(args.output, "sucesso")
    os.makedirs(subdir, exist_ok=True)
    print("sucesso/...")

    # Sucesso × Perda (barras agrupadas por protocolo)
    barras_agrupadas(dados, "Perda", "Protocol", "TaxaSucesso",
                     "Perda de Pacotes", "Taxa de Sucesso (%)",
                     "Sucesso × Perda — TCP/TLS vs QUIC",
                     subdir, "por-perda-e-protocolo.png", cores=COR)

    # Sucesso × Tecnologia
    barras_agrupadas(dados, "RotWiFi", "Protocol", "TaxaSucesso",
                     "Tecnologia do Enlace", "Taxa de Sucesso (%)",
                     "Sucesso × Tecnologia — TCP/TLS vs QUIC",
                     subdir, "por-tecnologia-e-protocolo.png", cores=COR)

    # Sucesso × Taxa de Transmissão
    barras_agrupadas(dados, "Banda", "Protocol", "TaxaSucesso",
                     "Taxa de Transmissão", "Taxa de Sucesso (%)",
                     "Sucesso × Taxa de Transmissão — TCP/TLS vs QUIC",
                     subdir, "por-banda-e-protocolo.png", cores=COR)

    # Sucesso × Clientes Ativos
    barras_agrupadas(dados, "ClientesAtivos", "Protocol", "TaxaSucesso",
                     "Clientes Ativos", "Taxa de Sucesso (%)",
                     "Sucesso × Clientes Ativos — TCP/TLS vs QUIC",
                     subdir, "por-clientes-e-protocolo.png", cores=COR)

    # Sucesso × Clientes Fundo
    barras_agrupadas(dados, "ClientesFundo", "Protocol", "TaxaSucesso",
                     "Nós de Tráfego de Fundo", "Taxa de Sucesso (%)",
                     "Sucesso × Tráfego de Fundo — TCP/TLS vs QUIC",
                     subdir, "por-fundo-e-protocolo.png", cores=COR)

    # Sucesso × Perda × Taxa de Transmissão
    for banda in sorted(set(r["Banda"] for r in dados)):
        sub = filt(dados, Banda=banda)
        lbl = banda.replace("Mbps"," Mbps")
        barras_agrupadas(sub, "Perda", "Protocol", "TaxaSucesso",
                         "Perda de Pacotes", "Taxa de Sucesso (%)",
                         f"Sucesso × Perda ({lbl}) — TCP/TLS vs QUIC",
                         subdir, f"por-perda-{banda.lower()}.png", cores=COR)

    # Sucesso × Perda × Tecnologia
    for tec in ["Ethernet","WiFi"]:
        sub = filt(dados, RotWiFi=tec)
        barras_agrupadas(sub, "Perda", "Protocol", "TaxaSucesso",
                         "Perda de Pacotes", "Taxa de Sucesso (%)",
                         f"Sucesso × Perda ({tec}) — TCP/TLS vs QUIC",
                         subdir, f"por-perda-{tec.lower()}.png", cores=COR)

    # ===============================================================
    # 2. LATÊNCIA
    # ===============================================================
    subdir = os.path.join(args.output, "latencia")
    os.makedirs(subdir, exist_ok=True)
    print("latencia/...")

    barras_agrupadas(dados_lat, "Perda", "Protocol", "LatenciaMediaMs",
                     "Perda de Pacotes", "Latência Média (ms)",
                     "Latência × Perda — TCP/TLS vs QUIC",
                     subdir, "por-perda-e-protocolo.png", cores=COR)

    barras_agrupadas(dados_lat, "RotWiFi", "Protocol", "LatenciaMediaMs",
                     "Tecnologia", "Latência Média (ms)",
                     "Latência × Tecnologia",
                     subdir, "por-tecnologia-e-protocolo.png", cores=COR)

    barras_agrupadas(dados_lat, "Banda", "Protocol", "LatenciaMediaMs",
                     "Taxa de Transmissão", "Latência Média (ms)",
                     "Latência × Taxa de Transmissão",
                     subdir, "por-banda-e-protocolo.png", cores=COR)

    barras_agrupadas(dados_lat, "ClientesAtivos", "Protocol", "LatenciaMediaMs",
                     "Clientes Ativos", "Latência Média (ms)",
                     "Latência × Clientes Ativos",
                     subdir, "por-clientes-e-protocolo.png", cores=COR)

    barras_agrupadas(dados_lat, "ClientesFundo", "Protocol", "LatenciaMediaMs",
                     "Nós de Fundo", "Latência Média (ms)",
                     "Latência × Tráfego de Fundo",
                     subdir, "por-fundo-e-protocolo.png", cores=COR)

    # ===============================================================
    # 3. HEATMAPS
    # ===============================================================
    subdir = os.path.join(args.output, "heatmaps")
    os.makedirs(subdir, exist_ok=True)
    print("heatmaps/...")

    heatmap(dados, "Perda", "Protocol", "TaxaSucesso",
            "Sucesso × Perda × Protocolo",
            subdir, "sucesso-perda-protocolo.png", vmin=0, vmax=100)

    heatmap(dados, "ClientesAtivos", "Protocol", "TaxaSucesso",
            "Sucesso × Clientes × Protocolo",
            subdir, "sucesso-clientes-protocolo.png", vmin=0, vmax=100)

    heatmap(dados, "ClientesFundo", "Protocol", "TaxaSucesso",
            "Sucesso × Fundo × Protocolo",
            subdir, "sucesso-fundo-protocolo.png", vmin=0, vmax=100)

    # heatmap 2D: perda × taxa de transmissão
    for proto in ["TCP","QUIC"]:
        sub = filt(dados, Protocol=proto)
        if not sub: continue
        heatmap(sub, "Perda", "Banda", "TaxaSucesso",
                f"Sucesso × Perda × Tx ({ROT.get(proto,proto)})",
                subdir, f"sucesso-perda-banda-{proto.lower()}.png", vmin=0, vmax=100)

    # ===============================================================
    # 4. QoE — Website Loading Experience
    # ===============================================================
    subdir = os.path.join(args.output, "qoe")
    os.makedirs(subdir, exist_ok=True)
    print("qoe/...")

    # QoE × Perda
    barras_agrupadas(dados, "Perda", "Protocol", "QoE",
                     "Taxa de Perda de Pacotes", "QoE Score (0–100)",
                     "QoE × Perda — TCP/TLS vs QUIC",
                     subdir, "por-perda-e-protocolo.png", cores=COR)

    # QoE × Taxa de Transmissão
    barras_agrupadas(dados, "Banda", "Protocol", "QoE",
                     "Taxa de Transmissão", "QoE Score (0–100)",
                     "QoE × Taxa de Transmissão — TCP/TLS vs QUIC",
                     subdir, "por-banda-e-protocolo.png", cores=COR)

    # QoE × Tecnologia
    barras_agrupadas(dados, "RotWiFi", "Protocol", "QoE",
                     "Tecnologia do Enlace", "QoE Score (0–100)",
                     "QoE × Tecnologia — TCP/TLS vs QUIC",
                     subdir, "por-tecnologia-e-protocolo.png", cores=COR)

    # QoE × Clientes Ativos
    barras_agrupadas(dados, "ClientesAtivos", "Protocol", "QoE",
                     "Clientes Ativos", "QoE Score (0–100)",
                     "QoE × Clientes Ativos — TCP/TLS vs QUIC",
                     subdir, "por-clientes-e-protocolo.png", cores=COR)

    # QoE × Clientes Fundo
    barras_agrupadas(dados, "ClientesFundo", "Protocol", "QoE",
                     "Nós de Tráfego de Fundo", "QoE Score (0–100)",
                     "QoE × Tráfego de Fundo — TCP/TLS vs QUIC",
                     subdir, "por-fundo-e-protocolo.png", cores=COR)

    # QoE × Perda × Tecnologia
    for tec in ["Ethernet","WiFi"]:
        sub = filt(dados, RotWiFi=tec)
        barras_agrupadas(sub, "Perda", "Protocol", "QoE",
                         "Taxa de Perda de Pacotes", "QoE Score (0–100)",
                         f"QoE × Perda ({tec}) — TCP/TLS vs QUIC",
                         subdir, f"por-perda-{tec.lower()}.png", cores=COR)

    # QoE × Perda × Taxa de Transmissão
    for banda in sorted(set(r["Banda"] for r in dados)):
        sub = filt(dados, Banda=banda)
        lbl = banda.replace("Mbps"," Mbps")
        barras_agrupadas(sub, "Perda", "Protocol", "QoE",
                         "Taxa de Perda de Pacotes", "QoE Score (0–100)",
                         f"QoE × Perda ({lbl}) — TCP/TLS vs QUIC",
                         subdir, f"por-perda-{banda.lower()}.png", cores=COR)

    # ===============================================================
    # 5. RELATÓRIO
    # ===============================================================
    relatorio(dados, args.output)

    n = sum(len(files) for _,_,files in os.walk(args.output))
    print(f"\nConcluído — {n} arquivos em {args.output}/")


if __name__ == "__main__":
    main()
