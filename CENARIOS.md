# Cenários de Teste — TCP vs UDP vs QUIC

Este documento define **13 cenários** para comparar desempenho de TCP, UDP e QUIC
quanto a **taxa de sucesso de conexão**, **latência de handshake** e **vazão**.

Cada cenário tem **3 níveis de intensidade** (1 = leve, 2 = moderado, 3 = extremo)
para escalar gradualmente as condições adversas.

## Parâmetros principais

| Parâmetro | Padrão | Descrição |
|---|---|---|
| `--numTcpClients` | 2 | Número de nós clientes TCP |
| `--numUdpClients` | 2 | Número de nós clientes UDP |
| `--numQuicClients` | 0 | Número de nós clientes QUIC |
| `--connPerClient` | 5 | Conexões TCP por cliente |
| `--connInterval` | 0.5 | Intervalo entre conexões (s) |
| `--connTimeout` | 10 | Timeout de handshake (s) |
| `--bottleneckBw` | 10Mbps | Largura de banda do gargalo |
| `--bottleneckDelay` | 20ms | Latência do gargalo |
| `--bottleneckError` | 0.0 | Taxa de erro no gargalo (0–1) |
| `--serverBw` | 100Mbps | Banda do link do servidor |
| `--serverDelay` | 5ms | Latência do link do servidor |
| `--accessBw` | 100Mbps | Banda dos links de acesso |
| `--accessDelay` | 2ms | Latência dos links de acesso |
| `--duration` | 60 | Duração da simulação (s) |
| `--tcpVariant` | TcpNewReno | Variante de controle de congestão |
| `--wifiStandard` | 80211ac | Padrão WiFi (a/b/g/n/ac/ax) |
| `--wifiRate` | VhtMcs7 | Taxa PHY WiFi |
| `--enableWifi` | 1 | Habilita WiFi (0 = só cabeado) |
| `--enableQuic` | 0 | Habilita testes QUIC |
| `--background` | 0 | Habilita tráfego de fundo |
| `--numBgNodes` | 2 | Nós de tráfego de fundo |
| `--bgRate` | 2Mbps | Taxa de tráfego de fundo |
| `--queueDisc` | PfifoFastQueueDisc | Disciplina de fila |
| `--outputDir` | auto | Diretório de saída |
| `--run` | 0 | Semente RNG |
| `--verboseConn` | 0 | Log por conexão |

## Formato de saída

Cada execução gera um diretório com:

```
--outputDir/                      # ex: scratch/RESULTADOS/cenario1/
├── summary.csv                   # 1 linha por protocolo (TCP/UDP/QUIC)
│                                 #   com métricas agregadas + parâmetros
├── connections.csv               # 1 linha por conexão individual
├── flows.csv                     # FlowMonitor por fluxo
└── flows.flowmonitor             # XML completo
```

**`summary.csv`** é o arquivo para pós-processamento:

```
Protocol,Successes,Attempts,SuccessRate,AvgLatencyMs,MinLatencyMs,MaxLatencyMs,
BytesRx,PacketsRx,EstPacketsSent,DeliveryRate,0RTTFlows,
TcpVariant,BottleneckBw,BottleneckDelay,BottleneckError,
ServerBw,ServerDelay,AccessBw,AccessDelay,
Wifi,WifiStandard,WifiRate,QueueDisc,Background,BgRate,BgNodes,
NumClients,ConnPerClient,ConnInterval,ConnTimeout,Duration
```

Exemplo de análise com Python:
```python
import csv, glob, numpy as np

rows = []
for f in sorted(glob.glob("scratch/RESULTADOS/**/summary.csv", recursive=True)):
    with open(f) as fh:
        for r in csv.DictReader(fh):
            r["SuccessRate"] = float(r.get("SuccessRate", 0))
            rows.append(r)

tcp = [r["SuccessRate"] for r in rows if r["Protocol"] == "TCP"]
print(f"TCP success rate: {np.mean(tcp):.1f}% ±{np.std(tcp):.1f}%")
```

---

## Cenário 1: Linha de Base (Baseline)

**O que compara:** Desempenho puro de TCP, UDP e QUIC em uma rede limpa, sem perdas
e sem concorrência. Serve como referência para todos os outros cenários.

**Hipótese:** TCP e QUIC devem ter 100% de sucesso. TCP pode ser ligeiramente mais
rápido no handshake (3-way é mais simples que TLS 1.3). UDP sem handshake,
entrega ~100%.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=2 --numUdpClients=1 --numQuicClients=2 --connPerClient=5 --duration=30 --outputDir=baseline/nivel1"` | Sem perdas |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=5 --numUdpClients=3 --numQuicClients=5 --connPerClient=10 --duration=60 --bottleneckBw=100Mbps --outputDir=baseline/nivel2"` | 100 Mbps, 10 conexões/cliente |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=10 --numUdpClients=5 --numQuicClients=10 --connPerClient=20 --duration=120 --bottleneckBw=1Gbps --outputDir=baseline/nivel3"` | 1 Gbps, 20 conexões/cliente |

**Métricas esperadas:**
- Sucesso TCP/QUIC: 100%
- Handshake TCP: ~50–60 ms (WiFi + enlaces P2P)
- Handshake QUIC: ~55–70 ms (TLS 1.3 incluso)
- Entrega UDP: ~99–100%

---

## Cenário 2: Perda de Pacotes (Packet Loss)

**O que compara:** Resiliência dos protocolos a perdas no enlace gargalo.
TCP depende de ACKs; QUIC usa mecanismos similares com menos overhead.
Perdas testam retransmissão no handshake.

**Hipótese:** TCP degrada mais rápido que QUIC sob perda > 5%. UDP não tem
handshake, mas perde pacotes de dados.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --bottleneckError=0.01 --duration=60 --outputDir=packetloss/nivel1"` | 1% de perda |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --bottleneckError=0.05 --duration=60 --outputDir=packetloss/nivel2"` | 5% de perda |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --bottleneckError=0.10 --duration=60 --connTimeout=15 --outputDir=packetloss/nivel3"` | 10% de perda |

**Métricas esperadas:**
- Loss 1%: TCP 100%, QUIC 100%
- Loss 5%: TCP 80–95%, QUIC 90–100%
- Loss 10%: TCP 50–70%, QUIC 70–90%
- Latência de handshake aumenta com perda
- Entrega UDP: ~99% / 95% / 90%

---

## Cenário 3: Alta Latência (Longa Distância)

**O que compara:** Impacto da latência (RTT elevado) no handshake.
Simula links satélite (500+ ms) ou transcontinentais (100–300 ms).

**Hipótese:** TCP 3-way handshake = 1.5 RTT. QUIC handshake = 1 RTT (TLS 1.3 otimizado).
QUIC deve ser mais rápido sob alta latência.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckDelay=50ms --serverDelay=30ms --outputDir=latency/nivel1 --connTimeout=20"` | RTT ~110 ms |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckDelay=150ms --serverDelay=50ms --outputDir=latency/nivel2 --connTimeout=30"` | RTT ~410 ms |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckDelay=300ms --serverDelay=100ms --outputDir=latency/nivel3 --connTimeout=60"` | RTT ~810 ms |

**Métricas esperadas:**
- Handshake TCP ~= 1.5× RTT
- Handshake QUIC ~= 1× RTT
- Sucesso > 95% se timeout configurado adequadamente

---

## Cenário 4: Gargalo de Banda (Bandwidth Bottleneck)

**O que compara:** Como largura de banda limitada afeta múltiplas conexões
concorrentes. TCP com controle de congestionamento vs. UDP sem controle.

**Hipótese:** Em banda baixa, conexões TCP competem pelo slow start.
Conexões curtas sofrem com slow start incompleto.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=2 --numQuicClients=3 --connPerClient=5 --bottleneckBw=50Mbps --duration=30 --outputDir=bottleneck/nivel1"` | 50 Mbps |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=5 --numUdpClients=3 --numQuicClients=5 --connPerClient=10 --bottleneckBw=5Mbps --duration=60 --outputDir=bottleneck/nivel2"` | 5 Mbps |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=10 --numUdpClients=5 --numQuicClients=10 --connPerClient=15 --bottleneckBw=1Mbps --duration=90 --connInterval=1.0 --outputDir=bottleneck/nivel3"` | 1 Mbps |

---

## Cenário 5: Concorrência com Tráfego de Fundo (Background Traffic)

**O que compara:** Degradação dos protocolos quando há tráfego competindo
pelo mesmo gargalo. Simula rede compartilhada realista.

**Hipótese:** TCP se adapta (fairness). UDP não se adapta, causando unfairness.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=2 --numUdpClients=1 --numQuicClients=2 --connPerClient=10 --bottleneckBw=10Mbps --background=1 --bgRate=1Mbps --numBgNodes=2 --outputDir=background/nivel1 --duration=60"` | Fundo 1 Mbps × 2 nós |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=2 --numUdpClients=1 --numQuicClients=2 --connPerClient=10 --bottleneckBw=10Mbps --background=1 --bgRate=5Mbps --numBgNodes=2 --outputDir=background/nivel2 --duration=60"` | Fundo 5 Mbps × 2 = 10 Mbps (satura) |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=2 --numUdpClients=1 --numQuicClients=2 --connPerClient=10 --bottleneckBw=10Mbps --background=1 --bgRate=5Mbps --numBgNodes=3 --outputDir=background/nivel3 --duration=60 --bottleneckDelay=50ms"` | Fundo 15 Mbps + latência |

---

## Cenário 6: WiFi vs Cabeado

**O que compara:** Desempenho via WiFi (sem fio compartilhado) vs. enlace
cabeado P2P dedicado.

**Hipótese:** WiFi adiciona latência variável e perdas por contenção.

| Nível | Comando | Condições |
|---|---|---|
| **WiFi** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=2 --numQuicClients=3 --connPerClient=10 --wifiStandard=80211ac --wifiRate=VhtMcs5 --duration=60 --outputDir=wifi-vs-wired/wifi"` | 802.11ac |
| **Cabeado** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=2 --numQuicClients=3 --connPerClient=10 --enableWifi=0 --duration=60 --outputDir=wifi-vs-wired/wired"` | P2P apenas |
| **WiFi 2.4** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=5 --numUdpClients=3 --numQuicClients=3 --connPerClient=10 --wifiStandard=80211g --wifiRate=ErpOfdmRate54Mbps --duration=60 --outputDir=wifi-vs-wired/wifi-2.4"` | 802.11g (2.4 GHz) |

---

## Cenário 7: Variantes TCP (TcpNewReno vs TcpBbr vs TcpCubic vs TcpVegas)

**O que compara:** Diferentes algoritmos de controle de congestionamento no
handshake e desempenho de conexões curtas.

**Hipótese:** BBR pode ter handshake mais rápido (modelo, não depende de perda).
Vegas sofre com RTT alto.

| Comando | Variante |
|---|---|
| `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=1 --connPerClient=10 --tcpVariant=TcpNewReno --duration=60 --outputDir=tcp-variants/newreno"` | TcpNewReno |
| `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=1 --connPerClient=10 --tcpVariant=TcpBbr --duration=60 --outputDir=tcp-variants/bbr"` | TcpBbr |
| `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=1 --connPerClient=10 --tcpVariant=TcpCubic --duration=60 --outputDir=tcp-variants/cubic"` | TcpCubic |
| `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=1 --connPerClient=10 --tcpVariant=TcpVegas --duration=60 --outputDir=tcp-variants/vegas"` | TcpVegas |

**Teste extra — sob 5% de perda:**
```bash
./ns3 run "connection-success --numTcpClients=3 --connPerClient=10 --tcpVariant=TcpBbr --bottleneckError=0.05 --duration=60 --outputDir=tcp-variants/bbr-loss"
./ns3 run "connection-success --numTcpClients=3 --connPerClient=10 --tcpVariant=TcpNewReno --bottleneckError=0.05 --duration=60 --outputDir=tcp-variants/newreno-loss"
```

---

## Cenário 8: Escalonamento (Número de Conexões)

**O que compara:** Como a taxa de sucesso escala com o número de conexões
concorrentes.

**Hipótese:** A partir de certo número, filas saturam, causando perdas de
SYN/INITIAL e timeouts.

| Nível | Comando | Conexões totais |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=5 --numQuicClients=5 --connPerClient=5 --connInterval=0.1 --bottleneckBw=100Mbps --duration=30 --outputDir=scaling/nivel1"` | 50 |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=20 --numQuicClients=20 --connPerClient=10 --connInterval=0.05 --bottleneckBw=100Mbps --duration=60 --outputDir=scaling/nivel2"` | 400 |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=50 --numQuicClients=50 --connPerClient=20 --connInterval=0.02 --bottleneckBw=1Gbps --duration=120 --connTimeout=20 --outputDir=scaling/nivel3"` | 2000 |

---

## Cenário 9: Disciplinas de Fila (PfifoFast vs CoDel)

**O que compara:** Efeito de AQM (CoDel) vs. drop-tail (PfifoFast).
CoDel controla a fila ativamente para reduzir bufferbloat.

**Hipótese:** CoDel reduz latência de handshake sob congestão. Porém, pode
descartar prematuramente pacotes de handshake.

| Comando | Disciplina |
|---|---|
| `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --bottleneckBw=5Mbps --background=1 --bgRate=3Mbps --numBgNodes=2 --queueDisc=ns3::PfifoFastQueueDisc --duration=60 --outputDir=queue/pfifo"` | PfifoFast |
| `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --bottleneckBw=5Mbps --background=1 --bgRate=3Mbps --numBgNodes=2 --queueDisc=ns3::CoDelQueueDisc --duration=60 --outputDir=queue/codel"` | CoDel |

---

## Cenário 10: Proporção de Conexões (TCP vs UDP vs QUIC)

**O que compara:** Fairness entre protocolos quando diferentes proporções
competem.

**Hipótese:** TCP e QUIC são justos entre si. UDP sem controle ocupa banda
indiscriminadamente.

| Comando | Proporção |
|---|---|
| `./ns3 run "connection-success --enableQuic=1 --numTcpClients=8 --numUdpClients=1 --numQuicClients=8 --connPerClient=10 --bottleneckBw=10Mbps --duration=60 --outputDir=proportion/8-1-8"` | 8:1:8 |
| `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=6 --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --duration=60 --outputDir=proportion/3-6-3"` | 3:6:3 (UDP domina) |
| `./ns3 run "connection-success --enableQuic=1 --numTcpClients=5 --numUdpClients=1 --numQuicClients=5 --connPerClient=10 --bottleneckBw=10Mbps --duration=60 --udpRate=10Mbps --outputDir=proportion/5-1-5-aggr"` | UDP agressivo |

---

## Cenário 11: Handshake 0-RTT do QUIC (⚠ não funcional no ns-3.47)

**O que compara:** QUIC com 0-RTT (retomada de sessão) vs. handshake completo.

**Hipótese:** 0-RTT elimina o RTT de handshake. Vantagem dramática sob latência alta.

**Nota:** O módulo QUIC do ns-3.47 não suporta 0-RTT — causa o erro
`"0RTT Handshake requested with wrong Initial Version"`.
O cenário está documentado para referência, mas não pode ser executado.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numQuicClients=3 --numTcpClients=3 --connPerClient=5 --duration=30 --ns3::QuicL4Protocol::0RTT-Handshake=1 --outputDir=quic-0rtt/nivel1"` | ⚠ Falha no QUIC |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numQuicClients=3 --numTcpClients=3 --connPerClient=5 --duration=30 --bottleneckDelay=100ms --serverDelay=50ms --ns3::QuicL4Protocol::0RTT-Handshake=1 --connTimeout=30 --outputDir=quic-0rtt/nivel2"` | ⚠ Falha no QUIC |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numQuicClients=3 --numTcpClients=3 --connPerClient=5 --duration=60 --bottleneckDelay=100ms --bottleneckError=0.02 --ns3::QuicL4Protocol::0RTT-Handshake=1 --connTimeout=30 --outputDir=quic-0rtt/nivel3"` | ⚠ Falha no QUIC |

---

## Cenário 12: Perda em Rajada (Burst Loss)

**O que compara:** Rajadas de perda (bursts) vs perda uniforme.

**Nota:** O modelo `RateErrorModel` padrão distribui perdas uniformemente (Bernoulli).
Para burst, usar `BurstErrorModel`.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --bottleneckError=0.01 --duration=60 --outputDir=burst/uniform"` | Perda uniforme 1% |
| **2** | mesmo comando + flag de burst error model | Perda em rajada |

---

## Cenário 13: Vários Standards WiFi (802.11n / ac / ax)

**O que compara:** WiFi 4 vs 5 vs 6 em latência de handshake e taxa de sucesso.

**Hipótese:** WiFi mais moderno (ax) tem menor latência e maior capacidade.

| Comando | Padrão |
|---|---|
| `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --wifiStandard=80211n --wifiRate=HtMcs7 --duration=60 --outputDir=wifi-standards/80211n"` | 802.11n (WiFi 4) |
| `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --wifiStandard=80211ac --wifiRate=VhtMcs7 --duration=60 --outputDir=wifi-standards/80211ac"` | 802.11ac (WiFi 5) |
| `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --wifiStandard=80211ax --wifiRate=HeMcs7 --duration=60 --outputDir=wifi-standards/80211ax"` | 802.11ax (WiFi 6) |

---

## Como Executar em Lote

```bash
#!/bin/bash
# Cenário 2: Packet Loss — 3 níveis
BASE="connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --duration=60"

echo "=== Nível 1: 1% loss ==="
./ns3 run "$BASE --bottleneckError=0.01 --outputDir=packetloss/nivel1"

echo "=== Nível 2: 5% loss ==="
./ns3 run "$BASE --bottleneckError=0.05 --outputDir=packetloss/nivel2"

echo "=== Nível 3: 10% loss ==="
./ns3 run "$BASE --bottleneckError=0.10 --connTimeout=15 --outputDir=packetloss/nivel3"
```

**Múltiplas execuções do mesmo cenário** (para variância estatística):

```bash
#!/bin/bash
DIR="packetloss/5pct-10runs"
for r in $(seq 0 9); do
  echo "=== Run $r/10 ==="
  ./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 \
    --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --bottleneckError=0.05 \
    --duration=60 --run=$r --outputDir=$DIR"
done
# Cada run adiciona 1 linha ao $DIR/summary.csv (append automático)
```

---

## Interpretação de Resultados

O `summary.csv` contém todas as métricas. Exemplo real:

```
Protocol,Run,Successes,Attempts,SuccessRate,AvgLatencyMs,MinLatencyMs,MaxLatencyMs,...
TCP,0,28,30,93.33,125.4,54.2,3054.3,...
UDP,0,0,0,0.00,0.0,0.0,0.0,1185800,847,892,95.0,...
QUIC,0,29,30,96.67,98.2,52.1,158.3,...
```

**O que observar entre níveis:**
- Queda na taxa de sucesso → limite do protocolo
- Aumento na latência → retransmissões ou filas cheias
- `0RTTFlows > 0` → QUIC usando caminho rápido
- `MinLatencyMs` vs `MaxLatencyMs` → variabilidade do handshake

**Geração de gráficos:**
```bash
python3 plot-results.py -d scratch/RESULTADOS -o plots/
```
