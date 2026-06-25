#!/usr/bin/env python3
"""
Plot results from connection-success.cc experiments.

Reads summary.csv files from scratch/RESULTADOS/ and generates comparative
plots between TCP, UDP, and QUIC.

Dependencies (all pre-installed in ns-3 environment):
  - matplotlib (already present)
  - numpy      (already present)
  - csv, os, glob, sys (stdlib)

Usage:
  python3 plot-results.py                          # scan RESULTADOS/ recursively
  python3 plot-results.py -d scratch/RESULTADOS    # specific directory
  python3 plot-results.py -d . -o plots/           # custom output dir
  python3 plot-results.py --csv cenario1/summary.csv  # single file
"""

import argparse
import csv
import glob
import os
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SEARCH = os.path.join(BASE_DIR, "RESULTADOS")
DEFAULT_OUTPUT = os.path.join(BASE_DIR, "plots")

plt.rcParams.update({
    "figure.dpi": 120,
    "savefig.dpi": 150,
    "savefig.bbox": "tight",
    "font.size": 11,
    "axes.titlesize": 13,
    "axes.labelsize": 12,
})

COLORS = {"TCP": "#2196F3", "UDP": "#FF9800", "QUIC": "#4CAF50"}
MARKERS = {"TCP": "o", "UDP": "s", "QUIC": "^"}

# Tradução cabeçalhos CSV português → chaves internas inglês
PT_TO_EN = {
    "Protocolo": "Protocol", "Execucao": "Run",
    "Sucessos": "Successes", "Tentativas": "Attempts",
    "TaxaSucesso": "SuccessRate",
    "LatenciaMediaMs": "AvgLatencyMs",
    "LatenciaMinMs": "MinLatencyMs",
    "LatenciaMaxMs": "MaxLatencyMs",
    "TaxaEntrega": "DeliveryRate", "Fluxos0RTT": "0RTTFlows",
    "VarianteTCP": "TcpVariant",
    "GargaloBw": "BottleneckBw", "GargaloDelay": "BottleneckDelay",
    "GargaloErro": "BottleneckError",
    "ServidorBw": "ServerBw", "ServidorDelay": "ServerDelay",
    "AcessoBw": "AccessBw", "AcessoDelay": "AccessDelay",
    "WiFi": "Wifi", "WiFiPadrao": "WifiStandard", "WiFiTaxa": "WifiRate",
    "DisciplinaFila": "QueueDisc",
    "TraficoFundo": "Background", "TaxaFundo": "BgRate", "NosFundo": "BgNodes",
    "NumClientes": "NumClients", "ConexoesPorCliente": "ConnPerClient",
    "IntervaloConexoes": "ConnInterval", "TimeoutHandshake": "ConnTimeout",
    "Duracao": "Duration", "PacotesEnviadosEst": "EstPacketsSent",
    "BytesRx": "BytesRx", "PacotesRx": "PacketsRx",
}
PT_TO_EN_REVERSE = {v: k for k, v in PT_TO_EN.items()}


def _normalize(row: dict) -> dict:
    return {PT_TO_EN.get(k, k): v for k, v in row.items()}


# ---------------------------------------------------------------------------
# CSV loading (stdlib only)
# ---------------------------------------------------------------------------
def load_csv(filepath: str) -> list[dict]:
    """Load a summary.csv into list of dicts."""
    rows = []
    with open(filepath, newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            r = _normalize(r)  # pt → en
            # Convert numeric fields
            for key in ("SuccessRate", "AvgLatencyMs", "MinLatencyMs",
                        "MaxLatencyMs", "DeliveryRate", "BottleneckError",
                        "ConnInterval", "ConnTimeout", "Duration",
                        "NumClients", "ConnPerClient", "BgNodes"):
                if key in r:
                    try:
                        r[key] = float(r[key])
                    except (ValueError, TypeError):
                        pass
            for key in ("Successes", "Attempts", "Run", "Wifi",
                        "Background", "0RTTFlows", "BytesRx",
                        "PacketsRx", "EstPacketsSent"):
                if key in r:
                    try:
                        r[key] = int(float(r[key]))
                    except (ValueError, TypeError):
                        pass
            rows.append(r)
    return rows


def load_all(directory: str) -> list[dict]:
    """Recursively find and concatenate all summary.csv files."""
    pattern = os.path.join(directory, "**", "summary.csv")
    files = sorted(glob.glob(pattern, recursive=True))
    if not files:
        print(f"No summary.csv files found under {directory}")
        sys.exit(1)
    all_rows = []
    for f in files:
        try:
            rows = load_csv(f)
            scenario = os.path.basename(os.path.dirname(f))
            for r in rows:
                r["Scenario"] = scenario
            all_rows.extend(rows)
        except Exception as e:
            print(f"Warning: skipping {f}: {e}")
    if not all_rows:
        print("No valid CSV files found")
        sys.exit(1)
    print(f"Loaded {len(files)} files, {len(all_rows)} rows")
    return all_rows


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def filter_proto(rows: list[dict], proto: str) -> list[dict]:
    return [r for r in rows if r.get("Protocol") == proto]


def extract(rows: list[dict], key: str) -> np.ndarray:
    return np.array([r[key] for r in rows if key in r and r[key] is not None])


def mean_std(vals: np.ndarray):
    if len(vals) == 0:
        return 0, 0
    return float(np.mean(vals)), float(np.std(vals))


def unique_sorted(rows: list[dict], key: str):
    vals = sorted(set(r[key] for r in rows if key in r and r[key] is not None))
    return vals


def parse_bw(val) -> float:
    """Parse '10Mbps' or '1Gbps' to float Mbps."""
    if isinstance(val, (int, float)):
        return float(val)
    s = str(val).upper().replace("MBPS", "").replace("GBPS", "").strip()
    if "G" in str(val).upper():
        return float(s) * 1000
    try:
        return float(s)
    except ValueError:
        return 0.0


def summary_text(rows: list[dict]) -> str:
    """Return a text table of protocol summaries."""
    lines = []
    for proto in ["TCP", "UDP", "QUIC"]:
        sub = filter_proto(rows, proto)
        if not sub:
            continue
        sr_vals = extract(sub, "SuccessRate")
        lat_vals = extract(sub, "AvgLatencyMs")
        lat_vals = lat_vals[lat_vals > 0]
        dr_vals = extract(sub, "DeliveryRate")
        lines.append(f"  {proto:5s}: n={len(sub):3d}  "
                     f"SuccessRate={mean_std(sr_vals)[0]:.1f}%  "
                     f"AvgLatency={mean_std(lat_vals)[0]:.1f}ms  "
                     f"DeliveryRate={mean_std(dr_vals)[0]:.1f}%")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Plot 1: Success Rate bar chart
# ---------------------------------------------------------------------------
def plot_success_rate_bar(rows: list[dict], outdir: str):
    means, stds, labels, color_list = [], [], [], []
    for proto in ["TCP", "UDP", "QUIC"]:
        sub = filter_proto(rows, proto)
        if not sub:
            continue
        vals = extract(sub, "SuccessRate")
        if len(vals) == 0:
            continue
        m, s = mean_std(vals)
        means.append(m)
        stds.append(s)
        labels.append(proto)
        color_list.append(COLORS.get(proto, "#999"))

    if not means:
        return

    fig, ax = plt.subplots(figsize=(8, 5))
    x = np.arange(len(means))
    ax.bar(x, means, yerr=stds, capsize=8, color=color_list,
           edgecolor="white", linewidth=0.8, width=0.55)
    for i, (m, s) in enumerate(zip(means, stds)):
        ax.text(i, m + s + 1.5, f"{m:.1f}%", ha="center", fontweight="bold", fontsize=10)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Success Rate (%)")
    ax.set_title("Connection Success Rate by Protocol\n(mean ± std across runs)")
    ax.set_ylim(0, 110)
    ax.grid(axis="y", alpha=0.3)
    path = os.path.join(outdir, "01-success-rate-bar.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ---------------------------------------------------------------------------
# Plot 2: Latency box plot
# ---------------------------------------------------------------------------
def plot_latency_box(rows: list[dict], outdir: str):
    data, labels = [], []
    for proto in ["TCP", "QUIC"]:
        sub = filter_proto(rows, proto)
        vals = extract(sub, "AvgLatencyMs")
        vals = vals[vals > 0]
        if len(vals) == 0:
            continue
        data.append(vals)
        labels.append(proto)

    if not data:
        return

    fig, ax = plt.subplots(figsize=(7, 5))
    bp = ax.boxplot(data, labels=labels, patch_artist=True,
                    widths=0.4, showmeans=True,
                    meanprops={"marker": "D", "markerfacecolor": "red", "markersize": 6})
    for patch, lbl in zip(bp["boxes"], labels):
        patch.set_facecolor(COLORS.get(lbl, "#999"))
        patch.set_alpha(0.7)
    ax.set_ylabel("Handshake Latency (ms)")
    ax.set_title("Handshake Latency Distribution\n(TCP SYN→ESTABLISHED | QUIC HANDSHAKE→OPEN)")
    ax.grid(axis="y", alpha=0.3)
    path = os.path.join(outdir, "02-latency-boxplot.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ---------------------------------------------------------------------------
# Plot 3: Success Rate vs Packet Loss
# ---------------------------------------------------------------------------
def plot_success_vs_loss(rows: list[dict], outdir: str):
    fig, ax = plt.subplots(figsize=(8, 5))
    for proto in ["TCP", "QUIC"]:
        sub = filter_proto(rows, proto)
        if not sub:
            continue
        loss_levels = unique_sorted(sub, "BottleneckError")
        means, stds, xs = [], [], []
        for loss in loss_levels:
            vals = extract([r for r in sub if r.get("BottleneckError") == loss],
                           "SuccessRate")
            if len(vals) == 0:
                continue
            m, s = mean_std(vals)
            means.append(m)
            stds.append(s)
            xs.append(loss)
        if xs:
            ax.errorbar(xs, means, yerr=stds, marker=MARKERS.get(proto, "o"),
                        color=COLORS.get(proto, "#999"), capsize=5,
                        label=proto, linewidth=1.8, markersize=8)
    ax.set_xlabel("Packet Loss Rate")
    ax.set_ylabel("Success Rate (%)")
    ax.set_title("Success Rate vs Packet Loss")
    ax.legend()
    ax.grid(alpha=0.3)
    ax.set_ylim(0, 110)
    path = os.path.join(outdir, "03-success-vs-loss.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ---------------------------------------------------------------------------
# Plot 4: Success vs Bandwidth
# ---------------------------------------------------------------------------
def plot_success_vs_bandwidth(rows: list[dict], outdir: str):
    fig, ax = plt.subplots(figsize=(8, 5))
    for proto in ["TCP", "QUIC"]:
        sub = filter_proto(rows, proto)
        if not sub:
            continue
        # Group by bandwidth
        groups = defaultdict(list)
        for r in sub:
            bw = parse_bw(r.get("BottleneckBw", "10Mbps"))
            if "SuccessRate" in r:
                groups[bw].append(r["SuccessRate"])
        bws = sorted(groups.keys())
        means, stds, xs = [], [], []
        for bw in bws:
            vals = np.array(groups[bw])
            m, s = mean_std(vals)
            means.append(m)
            stds.append(s)
            xs.append(bw)
        if xs:
            ax.errorbar(xs, means, yerr=stds, marker=MARKERS.get(proto, "o"),
                        color=COLORS.get(proto, "#999"), capsize=5,
                        label=proto, linewidth=1.8, markersize=8)
    ax.set_xlabel("Bottleneck Bandwidth (Mbps)")
    ax.set_ylabel("Success Rate (%)")
    ax.set_title("Success Rate vs Bottleneck Bandwidth")
    ax.legend()
    ax.grid(alpha=0.3)
    ax.set_ylim(0, 110)
    path = os.path.join(outdir, "04-success-vs-bandwidth.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ---------------------------------------------------------------------------
# Plot 5: UDP Delivery Rate
# ---------------------------------------------------------------------------
def plot_udp_delivery(rows: list[dict], outdir: str):
    sub = filter_proto(rows, "UDP")
    if not sub:
        return
    scenarios = unique_sorted(sub, "Scenario")
    means = []
    for s in scenarios:
        vals = extract([r for r in sub if r.get("Scenario") == s], "DeliveryRate")
        m, _ = mean_std(vals)
        means.append(m)

    fig, ax = plt.subplots(figsize=(max(8, len(scenarios) * 1.2), 5))
    x = range(len(scenarios))
    ax.bar(x, means, color=COLORS["UDP"], alpha=0.8, edgecolor="white", width=0.55)
    ax.set_xticks(x)
    ax.set_xticklabels(scenarios, rotation=45, ha="right", fontsize=9)
    ax.set_ylabel("Delivery Rate (%)")
    ax.set_title("UDP Packet Delivery Rate by Scenario")
    ax.set_ylim(0, 110)
    ax.grid(axis="y", alpha=0.3)
    for i, m in enumerate(means):
        ax.text(i, m + 2, f"{m:.1f}%", ha="center", fontsize=9)
    path = os.path.join(outdir, "05-udp-delivery-rate.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ---------------------------------------------------------------------------
# Plot 6: Latency vs Loss
# ---------------------------------------------------------------------------
def plot_latency_vs_loss(rows: list[dict], outdir: str):
    fig, ax = plt.subplots(figsize=(8, 5))
    for proto in ["TCP", "QUIC"]:
        sub = filter_proto(rows, proto)
        sub = [r for r in sub if r.get("AvgLatencyMs", 0) > 0]
        if not sub:
            continue
        loss_levels = unique_sorted(sub, "BottleneckError")
        means, stds, xs = [], [], []
        for loss in loss_levels:
            vals = extract([r for r in sub if r.get("BottleneckError") == loss],
                           "AvgLatencyMs")
            if len(vals) == 0:
                continue
            m, s = mean_std(vals)
            means.append(m)
            stds.append(s)
            xs.append(loss)
        if xs:
            ax.errorbar(xs, means, yerr=stds, marker=MARKERS.get(proto, "o"),
                        color=COLORS.get(proto, "#999"), capsize=5,
                        label=proto, linewidth=1.8, markersize=8)
    ax.set_xlabel("Packet Loss Rate")
    ax.set_ylabel("Handshake Latency (ms)")
    ax.set_title("Handshake Latency vs Packet Loss")
    ax.legend()
    ax.grid(alpha=0.3)
    path = os.path.join(outdir, "06-latency-vs-loss.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ---------------------------------------------------------------------------
# Plot 7: Scenario grid (heatmap)
# ---------------------------------------------------------------------------
def plot_scenario_grid(rows: list[dict], outdir: str):
    scenarios = unique_sorted(rows, "Scenario")
    protocols = [p for p in ["TCP", "UDP", "QUIC"]
                 if filter_proto(rows, p)]
    if len(scenarios) < 2 or len(protocols) < 1:
        return

    grid = np.zeros((len(scenarios), len(protocols)))
    for i, s in enumerate(scenarios):
        for j, p in enumerate(protocols):
            vals = extract([r for r in rows
                            if r.get("Scenario") == s and r.get("Protocol") == p],
                           "SuccessRate")
            if len(vals) > 0:
                grid[i, j] = np.mean(vals)
            else:
                grid[i, j] = np.nan

    fig, ax = plt.subplots(figsize=(max(8, len(protocols) * 3),
                                    max(5, len(scenarios) * 0.6)))
    im = ax.imshow(grid, cmap="RdYlGn", aspect="auto", vmin=0, vmax=100)
    for i in range(len(scenarios)):
        for j in range(len(protocols)):
            val = grid[i, j]
            text = f"{val:.0f}%" if not np.isnan(val) else "N/A"
            color = "white" if (not np.isnan(val) and val < 50) else "black"
            ax.text(j, i, text, ha="center", va="center",
                    fontweight="bold", color=color, fontsize=11)
    ax.set_xticks(range(len(protocols)))
    ax.set_xticklabels(protocols, fontsize=12)
    ax.set_yticks(range(len(scenarios)))
    ax.set_yticklabels(scenarios, fontsize=10)
    ax.set_title("Success Rate (%) — Protocol × Scenario", fontweight="bold")
    fig.colorbar(im, ax=ax, shrink=0.8, label="Success Rate %")
    path = os.path.join(outdir, "07-scenario-grid.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ---------------------------------------------------------------------------
# Plot 8: QUIC 0-RTT
# ---------------------------------------------------------------------------
def plot_quic_0rtt(rows: list[dict], outdir: str):
    quic = filter_proto(rows, "QUIC")
    if not quic:
        return

    has = [r for r in quic if r.get("0RTTFlows", 0) > 0]
    not_has = [r for r in quic if r.get("0RTTFlows", 0) == 0]

    if not has and not not_has:
        return

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))

    # Success rate
    ax = axes[0]
    groups, names = [], []
    if not_has:
        groups.append(extract(not_has, "SuccessRate"))
        names.append("Without 0-RTT")
    if has:
        groups.append(extract(has, "SuccessRate"))
        names.append("With 0-RTT")
    if groups:
        ax.boxplot(groups, labels=names, patch_artist=True)
        ax.set_ylabel("Success Rate (%)")
        ax.set_title("QUIC Success Rate\n0-RTT enabled vs disabled")
        ax.grid(axis="y", alpha=0.3)

    # Latency
    ax = axes[1]
    groups2, names2 = [], []
    if not_has:
        vals = extract(not_has, "AvgLatencyMs")
        vals = vals[vals > 0]
        if len(vals) > 0:
            groups2.append(vals)
            names2.append("Without 0-RTT")
    if has:
        vals = extract(has, "AvgLatencyMs")
        vals = vals[vals > 0]
        if len(vals) > 0:
            groups2.append(vals)
            names2.append("With 0-RTT")
    if groups2:
        ax.boxplot(groups2, labels=names2, patch_artist=True)
        ax.set_ylabel("Handshake Latency (ms)")
        ax.set_title("QUIC Handshake Latency\n0-RTT enabled vs disabled")
        ax.grid(axis="y", alpha=0.3)

    path = os.path.join(outdir, "08-quic-0rtt.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ---------------------------------------------------------------------------
# Plot 9: TCP Variants
# ---------------------------------------------------------------------------
def plot_tcp_variants(rows: list[dict], outdir: str):
    tcp = filter_proto(rows, "TCP")
    if not tcp:
        return
    variants = unique_sorted(tcp, "TcpVariant")
    if len(variants) < 2:
        return

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))

    # Success rate
    ax = axes[0]
    means, stds, names = [], [], []
    for v in variants:
        vals = extract([r for r in tcp if r.get("TcpVariant") == v], "SuccessRate")
        if len(vals) == 0:
            continue
        m, s = mean_std(vals)
        means.append(m)
        stds.append(s)
        names.append(v.replace("ns3::", ""))

    x = range(len(names))
    ax.bar(x, means, yerr=stds, capsize=6, color=COLORS["TCP"], alpha=0.8,
           edgecolor="white", width=0.5)
    ax.set_xticks(x)
    ax.set_xticklabels(names, rotation=20, ha="right", fontsize=9)
    ax.set_ylabel("Success Rate (%)")
    ax.set_title("TCP Success Rate by Variant")
    ax.set_ylim(0, 110)
    ax.grid(axis="y", alpha=0.3)

    # Latency
    ax = axes[1]
    data2, names2 = [], []
    for v in variants:
        vals = extract([r for r in tcp
                        if r.get("TcpVariant") == v and r.get("AvgLatencyMs", 0) > 0],
                       "AvgLatencyMs")
        if len(vals) == 0:
            continue
        data2.append(vals)
        names2.append(v.replace("ns3::", ""))

    if data2:
        ax.boxplot(data2, labels=names2, patch_artist=True)
        ax.set_ylabel("Handshake Latency (ms)")
        ax.set_title("TCP Handshake Latency by Variant")
        ax.grid(axis="y", alpha=0.3)

    path = os.path.join(outdir, "09-tcp-variants.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ---------------------------------------------------------------------------
# Plot 10: WiFi vs Wired
# ---------------------------------------------------------------------------
def plot_wifi_vs_wired(rows: list[dict], outdir: str):
    wifi_on = [r for r in rows if r.get("Wifi") == 1]
    wifi_off = [r for r in rows if r.get("Wifi") == 0]
    if not wifi_on or not wifi_off:
        return

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))
    for ax, (label, sub) in zip(axes, [("WiFi", wifi_on), ("Wired", wifi_off)]):
        mean_list = []
        for proto in ["TCP", "QUIC"]:
            vals = extract(filter_proto(sub, proto), "SuccessRate")
            if len(vals) > 0:
                mean_list.append((proto, np.mean(vals)))
        if mean_list:
            x = range(len(mean_list))
            ax.bar(x, [m[1] for m in mean_list],
                   color=[COLORS.get(m[0], "#999") for m in mean_list],
                   edgecolor="white", width=0.45)
            ax.set_xticks(x)
            ax.set_xticklabels([m[0] for m in mean_list])
            ax.set_ylabel("Success Rate (%)")
            ax.set_title(f"Success Rate — {label}")
            ax.set_ylim(0, 110)
            ax.grid(axis="y", alpha=0.3)

    path = os.path.join(outdir, "10-wifi-vs-wired.png")
    fig.savefig(path)
    plt.close(fig)
    print(f"  -> {path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Plot connection-success.cc experiment results")
    parser.add_argument("-d", "--directory", default=DEFAULT_SEARCH,
                        help=f"Directory with summary.csv files (default: {DEFAULT_SEARCH})")
    parser.add_argument("-o", "--output", default=DEFAULT_OUTPUT,
                        help=f"Output directory for plots (default: {DEFAULT_OUTPUT})")
    parser.add_argument("--csv", default=None,
                        help="Plot a single summary.csv file (shortcut)")
    parser.add_argument("--no-grid", action="store_true",
                        help="Skip scenario grid plot")
    parser.add_argument("--no-variants", action="store_true",
                        help="Skip TCP variants plot")
    args = parser.parse_args()

    os.makedirs(args.output, exist_ok=True)

    # Load
    if args.csv:
        rows = load_csv(args.csv)
        for r in rows:
            r["Scenario"] = os.path.basename(
                os.path.dirname(args.csv) or ".")
        print(f"Loaded {args.csv}: {len(rows)} rows")
    else:
        rows = load_all(args.directory)

    print("\n--- Protocol Summary ---")
    print(summary_text(rows))

    # Generate plots
    print("\nGenerating plots...")
    plot_success_rate_bar(rows, args.output)
    plot_latency_box(rows, args.output)
    plot_success_vs_loss(rows, args.output)
    plot_success_vs_bandwidth(rows, args.output)
    plot_udp_delivery(rows, args.output)
    plot_latency_vs_loss(rows, args.output)
    plot_quic_0rtt(rows, args.output)
    plot_wifi_vs_wired(rows, args.output)
    if not args.no_grid:
        plot_scenario_grid(rows, args.output)
    if not args.no_variants:
        plot_tcp_variants(rows, args.output)

    n_plots = len([f for f in os.listdir(args.output) if f.endswith(".png")])
    print(f"\nDone — {n_plots} plots saved to {args.output}/")


if __name__ == "__main__":
    main()
