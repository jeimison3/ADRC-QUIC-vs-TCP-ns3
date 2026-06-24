# scratch — ns-3 User Simulation Scripts

Este diretório contém scripts de simulação personalizados para o ns-3.47.
Cada arquivo `.cc` com `main()` é compilado automaticamente como executável independente.

## Principais scripts

### `tcp-udp-comparison.cc`

Comparação de desempenho entre **TCP e UDP** em uma topologia mista cabeada/sem-fio com
2 roteadores intermediários e um Access Point.

**Topologia:**
```
[Server] --- P2P --- [Router1] --- P2P (gargalo) --- [Router2] --- P2P --- [AP_Node] ~~~~ WiFi ~~~~ [WiFi_STAs]
                                                              \+--- P2P --- [WiredClients]
```

**Características:**
- Servidor final com latência configurável
- 2 roteadores intermediários com IP forwarding
- Access Point WiFi + clientes STA
- Clientes cabeados opcionais para comparação
- Tráfego de fundo (background) configurável
- Controle de: perda de pacotes, latência, largura de banda, congestionamento
- Queue disciplines: PfifoFast ou CoDel
- FlowMonitor: saída CSV e XML por fluxo
- Relatório de throughput periódico
- Todas as opções via linha de comando

**Exemplos de uso:**
```bash
# Ajuda com todas as opções
./ns3 run "tcp-udp-comparison --PrintHelp"

# Simples: 1 TCP + 1 UDP via WiFi, gargalo 10 Mbps, 30s
./ns3 run "tcp-udp-comparison --numTcpSta=1 --numUdpSta=1"

# Apenas rede cabeada
./ns3 run "tcp-udp-comparison --enableWifi=0 --numWiredTcp=2 --numWiredUdp=2"

# Com perda de pacotes (2%) e tráfego de fundo
./ns3 run "tcp-udp-comparison --bottleneckError=0.02 --background=1 --numBgNodes=3 --bgRate=4Mbps"

# Testar TCP BBR vs UDP
./ns3 run "tcp-udp-comparison --tcpVariant=TcpBbr --numTcpSta=2 --numUdpSta=2"

# Enlace de longa distância (alta latência)
./ns3 run "tcp-udp-comparison --bottleneckBw=20Mbps --bottleneckDelay=100ms --duration=60"

# Com AQM CoDel
./ns3 run "tcp-udp-comparison --queueDisc=ns3::CoDelQueueDisc --duration=60"
```

## Parâmetros principais de `tcp-udp-comparison`

| Parâmetro | Padrão | Descrição |
|---|---|---|
| `tcpVariant` | `TcpNewReno` | Variante TCP (Bbr, Cubic, Vegas, Westwood, etc.) |
| `numTcpSta` | `1` | Número de clientes TCP WiFi |
| `numUdpSta` | `1` | Número de clientes UDP WiFi |
| `numWiredTcp` | `0` | Clientes TCP cabeados |
| `numWiredUdp` | `0` | Clientes UDP cabeados |
| `udpRate` | `5Mbps` | Taxa de envio UDP por cliente |
| `payloadSize` | `1400` | Tamanho do payload (bytes) |
| `serverBw` | `100Mbps` | Largura de banda do link do servidor |
| `serverDelay` | `5ms` | Latência do link do servidor |
| `bottleneckBw` | `10Mbps` | Largura de banda do gargalo |
| `bottleneckDelay` | `20ms` | Latência do gargalo |
| `bottleneckError` | `0.0` | Taxa de perda no gargalo (0.0—1.0) |
| `accessBw` | `100Mbps` | Largura de banda dos links de acesso |
| `accessDelay` | `2ms` | Latência dos links de acesso |
| `enableWifi` | `1` | Habilita seção WiFi (0 = apenas cabeado) |
| `wifiStandard` | `80211ac` | Padrão WiFi (a, b, g, n, ac, ax) |
| `wifiRate` | `VhtMcs7` | Taxa PHY WiFi |
| `background` | `0` | Habilita tráfego de fundo |
| `numBgNodes` | `2` | Nós de tráfego de fundo |
| `bgRate` | `2Mbps` | Taxa de tráfego de fundo por nó |
| `duration` | `30` | Duração da simulação (segundos) |
| `run` | `0` | Índice da execução (semente RNG) |
| `flowMonitor` | `1` | Gera CSV e XML do FlowMonitor |
| `queueDisc` | `PfifoFastQueueDisc` | Disciplina de fila |
| `periodicReport` | `1` | Relatório periódico de throughput |

## Saídas geradas

- **Terminal**: throughput agregado TCP/UDP e por fluxo individual
- **CSV**: `<prefix>-r<N>-flows.csv` — estatísticas por fluxo (bytes, pacotes, perdas, atraso)
- **XML**: `<prefix>-r<N>.flowmonitor` — dados completos do FlowMonitor

## Compilar e executar

```bash
# Compilar tudo no scratch/
cd /home/jeimison/Documentos/Tools/ns-allinone-3.47/ns-3.47
./ns3 build

# Compilar apenas um script
./ns3 build scratch/tcp-udp-comparison.cc

# Executar
./ns3 run "tcp-udp-comparison --duration=30"
```

## Estrutura do diretório

```
scratch/
├── AGENTS.md                  # Instruções para agentes de IA
├── README.md                  # Este arquivo
├── CMakeLists.txt             # Configuração de build automática
├── tcp-udp-comparison.cc      # Script principal TCP vs UDP
├── quic-wifi.cc               # Exemplo QUIC + WiFi
├── scratch-simulator.cc       # Exemplo padrão ns-3
├── subdir/                    # Exemplos em subdiretório
├── nested-subdir/             # Exemplos aninhados
└── tcpudp/                    # Dados de saída de simulações
```

## Variantes TCP disponíveis (ns-3.47)

`TcpNewReno`, `TcpBbr`, `TcpCubic`, `TcpVegas`, `TcpWestwood`,
`TcpWestwoodPlus`, `TcpLedbat`, `TcpHighSpeed`, `TcpBic`, `TcpYeah`,
`TcpIllinois`, `TcpScalable`, `TcpVeno`, `TcpHtcp`
