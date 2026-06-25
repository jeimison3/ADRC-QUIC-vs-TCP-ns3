# Experimento — Orquestrador de Simulações TCP/TLS vs QUIC

## Visão Geral

O **orquestrador** (`orchestrator.py`) automatiza a execução de **144 experimentos**
no ns-3, varrendo sistematicamente 6 dimensões de parâmetros em loops aninhados.
Cada combinação gera um diretório hierárquico com resultados completos.

## Estrutura dos Loops (outer → inner)

```
Loop 1: Tecnologia do enlace      →  Ethernet, WiFi5GHz
Loop 2: Taxa de transmissão       →  30Mbps, 100Mbps
Loop 3: Taxa de perda             →  0%, 3%, 15%
Loop 4: Protocolo                 →  QUIC, TCP-TLS
Loop 5: Clientes ativos           →  5, 45
Loop 6: Clientes de fundo         →  30, 100, 240
```

Total: 2 × 2 × 3 × 2 × 2 × 3 = **144 combinações**.

## Hierarquia de Diretórios

```
scratch/RESULTADOS/
├── Ethernet/
│   ├── 30Mbps/
│   │   ├── perda-0/
│   │   │   ├── QUIC/
│   │   │   │   ├── clientes-5/
│   │   │   │   │   ├── fundo-30/
│   │   │   │   │   │   ├── summary.csv        ← métricas agregadas por protocolo
│   │   │   │   │   │   ├── connections.csv    ← latência por conexão individual
│   │   │   │   │   │   ├── flows.csv          ← FlowMonitor por fluxo
│   │   │   │   │   │   ├── flows.flowmonitor  ← XML completo
│   │   │   │   │   │   └── command.txt        ← comando usado
│   │   │   │   │   ├── fundo-100/
│   │   │   │   │   └── fundo-240/
│   │   │   │   └── clientes-45/
│   │   │   │       ├── fundo-30/
│   │   │   │       ├── fundo-100/
│   │   │   │       └── fundo-240/
│   │   │   └── TCP-TLS/
│   │   │       ├── clientes-5/
│   │   │       │   ├── fundo-30/
│   │   │       │   ├── fundo-100/
│   │   │       │   └── fundo-240/
│   │   │       └── clientes-45/
│   │   │           ├── fundo-30/
│   │   │           ├── fundo-100/
│   │   │           └── fundo-240/
│   │   ├── perda-3/
│   │   │   └── ... (mesma estrutura)
│   │   └── perda-15/
│   │       └── ... (mesma estrutura)
│   └── 100Mbps/
│       └── ... (mesma estrutura)
└── WiFi5GHz/
    └── ... (mesma estrutura)
```

## Como Executar

```bash
cd /caminho/para/ns-3.47

# Executar todos os 144 experimentos
python3 scratch/orchestrator.py

# Executar apenas os 5 primeiros (teste rápido)
python3 scratch/orchestrator.py --max-runs=5

# Apenas mostrar o que seria executado (sem rodar)
python3 scratch/orchestrator.py --dry-run

# Retomar de onde parou (pula diretórios com summary.csv existente)
python3 scratch/orchestrator.py --resume

# Começar a partir do índice 50
python3 scratch/orchestrator.py --from-idx=50
```

## Constantes Modificáveis (no topo do `orchestrator.py`)

| Constante | Padrão | Descrição |
|---|---|---|
| `DURACAO` | 30 | Segundos de simulação por experimento |
| `TLS_PAYLOAD` | 3000 | Bytes no payload TLS pós-handshake (0 = desliga fase TLS) |
| `CONN_PER_CLIENT` | 1 | Conexões por cliente. ⚠ **QUIC: manter =1** (ver limitação abaixo) |
| `CONN_INTERVAL` | 0.5 | Intervalo entre conexões (s) |
| `CONN_TIMEOUT` | 15 | Timeout de handshake (s) |
| `UDP_BG_RATE_FRAC` | 0.3 | Fração da banda total usada por tráfego de fundo |
| `SERV_DELAY` | 5ms | Latência do link do servidor |
| `ACCESS_DELAY` | 2ms | Latência dos links de acesso |
| `GARGALO_DELAY` | 20ms | Latência do enlace gargalo |
| `WIFI_STANDARD` | 80211ac | Padrão WiFi |
| `WIFI_RATE` | VhtMcs7 | Taxa PHY WiFi |
| `WIFI_DISTANCE` | 5 | Distância AP → STAs (metros). Menor = sinal mais forte |

## ⚠ Limitação Conhecida: QUIC + múltiplas conexões por nó

O módulo QUIC do **ns-3.47** possui um bug: após a primeira conexão bem-sucedida de
um nó, ele tenta usar **0-RTT** (retomada de sessão TLS) na conexão seguinte.
A implementação do 0-RTT no ns-3.47 é **não funcional**, causando crash com a
mensagem `"0RTT Handshake requested with wrong Initial Version"`.

**Consequência:** `CONN_PER_CLIENT` deve ser **1** para QUIC.
Não use `connPerClient > 1` com `--enableQuic=1`.

**Alternativa para mais amostras:** aumente `numQuicClients` no orquestrador.
Cada nó adicional fará 1 handshake independente. Exemplo:
`numQuicClients=10` = 10 handshakes QUIC, cada um de um nó diferente.

**TCP não tem essa limitação** — pode usar `connPerClient` qualquer.

## O que cada parâmetro significa

| Parâmetro | Descrição |
|---|---|
| **Ethernet** | Clientes conectados ao roteador via P2P cabeado |
| **WiFi5GHz** | Clientes conectados via WiFi 802.11ac (5 GHz) |
| **30 Mbps / 100 Mbps** | Largura de banda do enlace gargalo entre os roteadores |
| **Perda 0% / 3% / 15%** | Taxa de perda de pacotes no enlace gargalo |
| **QUIC** | Clientes usando QUIC (TLS 1.3 embutido no handshake) |
| **TCP-TLS** | Clientes usando TCP com fase TLS simulada (echo de payload) |
| **Clientes ativos** | Número de hosts gerando tráfego TCP ou QUIC (cada um com 1 conexão) |
| **Clientes de fundo** | Nós gerando tráfego UDP de fundo (simulam streaming/ruído) |
| **Distância WiFi** | Distância AP → STAs em metros. Quanto maior, pior o sinal (lei de Friis) |

## Métricas Coletadas (summary.csv)

Cada execução produz um `summary.csv` com **1 linha por protocolo**:

| Coluna (pt) | Significado |
|---|---|
| `Protocolo` | TCP ou QUIC |
| `Sucessos` | Conexões que completaram o handshake |
| `Tentativas` | Total de tentativas de conexão |
| `TaxaSucesso` | Sucessos / Tentativas × 100 |
| `LatenciaMediaMs` | Tempo médio do handshake (ms) |
| `LatenciaMinMs` | Menor handshake observado |
| `LatenciaMaxMs` | Maior handshake observado |
| `GargaloBw` | Banda do gargalo |
| `GargaloDelay` | Latência do gargalo |
| `GargaloErro` | Perda no gargalo |
| `WiFi` | 1 = WiFi, 0 = Ethernet |
| `WiFiPadrao` | Padrão WiFi usado |
| `TraficoFundo` | 1 se tráfego de fundo ativo |
| `NosFundo` | Nós de tráfego de fundo |
| `NumClientes` | Clientes ativos do protocolo |
| `ConexoesPorCliente` | Conexões por cliente |
| `Duracao` | Duração da simulação |

## Pós-processamento com Python

```python
import csv, glob, os
import matplotlib.pyplot as plt
import numpy as np

rows = []
for f in sorted(glob.glob("scratch/RESULTADOS/**/summary.csv", recursive=True)):
    with open(f) as fh:
        for r in csv.DictReader(fh):
            r["TaxaSucesso"] = float(r["TaxaSucesso"])
            r["LatenciaMediaMs"] = float(r["LatenciaMediaMs"])
            r["GargaloErro"] = float(r["GargaloErro"])
            rows.append(r)

# Taxa de sucesso TCP vs QUIC por perda
for erro in ["0.0", "0.03", "0.15"]:
    sub = [r for r in rows if r["GargaloErro"] == float(erro)]
    tcp = [r["TaxaSucesso"] for r in sub if r["Protocolo"] == "TCP"]
    quic = [r["TaxaSucesso"] for r in sub if r["Protocolo"] == "QUIC"]
    print(f"Perda {float(erro)*100:.0f}%: TCP={np.mean(tcp):.1f}%  QUIC={np.mean(quic):.1f}%")

# Latência TCP vs QUIC
tcp_lat = [r["LatenciaMediaMs"] for r in rows if r["Protocolo"] == "TCP" and r["LatenciaMediaMs"] > "0"]
quic_lat = [r["LatenciaMediaMs"] for r in rows if r["Protocolo"] == "QUIC" and r["LatenciaMediaMs"] > "0"]
print(f"\nTCP latência média: {np.mean(tcp_lat):.1f} ms")
print(f"QUIC latência média: {np.mean(quic_lat):.1f} ms")
```

## Interpretação dos Resultados

Consulte o arquivo `CENARIOS.md` para os 13 cenários planejados e suas
hipóteses. O orquestrador cobre sistematicamente todos os parâmetros
desses cenários, permitindo análises cruzadas como:

- **TCP vs QUIC por perda**: qual protocolo é mais resiliente?
- **Ethernet vs WiFi**: quanto o meio sem fio penaliza o handshake?
- **Mais clientes → menos sucesso**: a partir de quantos clientes a taxa cai?
- **Banda de 30 vs 100 Mbps**: a banda afeta o estabelecimento de conexões curtas?
- **Fundo de 30 vs 240 nós**: o ruído de rede impacta mais TCP ou QUIC?

## Dicas

- Experimentos com **240 nós de fundo** são mais lentos de simular.
- Use `--max-runs` para testar primeiro com poucos experimentos.
- `--resume` é seguro: verifica se `summary.csv` já existe antes de executar.
- O arquivo `command.txt` em cada diretório registra o comando exato usado.
- Para variância estatística, execute o mesmo cenário múltiplas vezes com
  `--run={0..N}` e mesmo `--outputDir` — o `summary.csv` faz append.
