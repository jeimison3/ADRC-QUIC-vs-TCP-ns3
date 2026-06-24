# Cenários de Teste — TCP vs UDP vs QUIC

Este documento define **10+ cenários** para comparar desempenho de TCP, UDP e QUIC
quanto a **taxa de sucesso de conexão**, **latência de handshake** e **vazão**.

Cada cenário tem **3 níveis de intensidade** (1 = leve, 2 = moderado, 3 = extremo)
para escalar gradualmente as condições adversas.

Abreviações usadas nos comandos:

| Flag curta | Flag longa | Padrão |
|---|---|---|
| `-cT` | `--numTcpClients` | 2 |
| `-cU` | `--numUdpClients` | 1 |
| `-cQ` | `--numQuicClients` | 0 |
| `-n` | `--connPerClient` | 5 |
| `-i` | `--connInterval` | 0.5 s |
| `-to` | `--connTimeout` | 10 s |
| `-e` | `--bottleneckError` | 0.0 |
| `-bB` | `--bottleneckBw` | 10 Mbps |
| `-bD` | `--bottleneckDelay` | 20 ms |
| `-sB` | `--serverBw` | 100 Mbps |
| `-sD` | `--serverDelay` | 5 ms |
| `-aB` | `--accessBw` | 100 Mbps |
| `-aD` | `--accessDelay` | 2 ms |
| `-d` | `--duration` | 30 s |
| `-v` | `--tcpVariant` | TcpNewReno |
| `-wS` | `--wifiStandard` | 80211ac |
| `-wR` | `--wifiRate` | VhtMcs7 |
| `-bg` | `--background` | 0 |
| `-bgn` | `--numBgNodes` | 2 |
| `-bgr` | `--bgRate` | 2 Mbps |
| `-q` | `--queueDisc` | PfifoFastQueueDisc |
| `-p` | `--prefix` | conn-success |

---

## Cenário 1: Linha de Base (Baseline)

**O que compara:** Desempenho puro de TCP, UDP e QUIC em uma rede limpa, sem perdas
e sem concorrência. Serve como referência para todos os outros cenários.

**Hipótese:** TCP e QUIC devem ter 100% de sucesso. TCP pode ser ligeiramente mais
rápido no handshake (3-way handshake é mais simples que TLS 1.3). UDP não tem handshake,
então entrega é praticamente 100%.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=2 --numUdpClients=1 --numQuicClients=2 --connPerClient=5 --duration=30 --prefix=base1"` | 5 Mbps bottleneck, sem perdas |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=5 --numUdpClients=3 --numQuicClients=5 --connPerClient=10 --duration=60 --bottleneckBw=100Mbps --prefix=base2"` | 100 Mbps, 10 conexões por cliente |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=10 --numUdpClients=5 --numQuicClients=10 --connPerClient=20 --duration=120 --bottleneckBw=1Gbps --prefix=base3"` | 1 Gbps, 20 conexões por cliente |

**Métricas esperadas:**
- Sucesso TCP/QUIC: 100%
- Handshake TCP: ~50–60 ms (WiFi + enlaces P2P)
- Handshake QUIC: ~55–70 ms (TLS 1.3 incluso no handshake)
- Entrega UDP: ~99–100%

---

## Cenário 2: Perda de Pacotes (Packet Loss)

**O que compara:** Resiliência dos protocolos a perdas no enlace gargalo.
TCP depende de ACKs para estabelecer conexão; QUIC usa mecanismos similares com
menos overhead. Perdas testam retransmissão no handshake.

**Hipótese:** TCP degrada mais rápido que QUIC. Perdas acima de 5% devem começar
a impactar visivelmente a taxa de sucesso. UDP não tem handshake, mas perde pacotes,
reduzindo a taxa de entrega.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --bottleneckError=0.01 --duration=60 --prefix=loss1"` | 1% de perda |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --bottleneckError=0.05 --duration=60 --prefix=loss2"` | 5% de perda |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --bottleneckError=0.10 --duration=60 --connTimeout=15 --prefix=loss3"` | 10% de perda |

**Métricas esperadas:**
- Loss 1%: TCP próximo de 100%, QUIC próximo de 100%
- Loss 5%: TCP cai para 80–95%, QUIC mantém 90–100%
- Loss 10%: TCP cai para 50–70%, QUIC cai para 70–90%
- Latência de handshake aumenta com perda (retransmissões)
- Entrega UDP: 99%/95%/90% aproximadamente

---

## Cenário 3: Alta Latência (Longa Distância)

**O que compara:** Impacto da latência de rede (RTT elevado) no estabelecimento de
conexões. Simula links satélite (500+ ms) ou transcontinentais (100–300 ms).

**Hipótese:** TCP tem handshake de 3 vias (1.5 RTT para SYN → SYN+ACK → ACK);
QUIC tem 1-RTT handshake (TLS 1.3 otimizado). Handshakes QUIC devem ser mais
rápidos em altas latências. UDP continua sem handshake, mas o atraso afeta a
entrega de datagramas.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckDelay=50ms --serverDelay=30ms --prefix=lat1 --connTimeout=20"` | RTT ~110 ms |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckDelay=150ms --serverDelay=50ms --prefix=lat2 --connTimeout=30"` | RTT ~410 ms |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=1 --numQuicClients=3 --connPerClient=10 --bottleneckDelay=300ms --serverDelay=100ms --prefix=lat3 --connTimeout=60"` | RTT ~810 ms |

**Métricas esperadas:**
- Handshake TCP ~= 1.5× RTT (SYN → SYN+ACK → ACK)
- Handshake QUIC ~= 1× RTT (TLS 1.3 handshake completo em 1 RTT)
- Ambos com retransmissões se houver timeouts
- Sucesso deve permanecer alto (>95%) se o timeout for configurado adequadamente

---

## Cenário 4: Gargalo de Banda (Bandwidth Bottleneck)

**O que compara:** Como a largura de banda limitada afeta múltiplas conexões
concorrentes. TCP com controle de congestionamento vs. UDP sem controle vs. QUIC
com controle similar ao TCP.

**Hipótese:** Em banda baixa, múltiplas conexões TCP competem e o slow start
pode causar perdas. Conexões TCP curtas (handshake + dados) podem sofrer com
slow start incompleto. QUIC tem multiplexação de streams que pode ajudar.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=2 --numQuicClients=3 --connPerClient=5 --bottleneckBw=50Mbps --duration=30 --prefix=bw1"` | 50 Mbps gargalo |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=5 --numUdpClients=3 --numQuicClients=5 --connPerClient=10 --bottleneckBw=5Mbps --duration=60 --prefix=bw2"` | 5 Mbps gargalo |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=10 --numUdpClients=5 --numQuicClients=10 --connPerClient=15 --bottleneckBw=1Mbps --duration=90 --connInterval=1.0 --prefix=bw3` | 1 Mbps gargalo, 1s entre tentativas |

**Métricas esperadas:**
- Em 1 Mbps: fila enche, perdas por overflow. TCP tem backoff excessivo.
- QUIC pode ter melhor desempenho com multiplexação.
- UDP sature a fila e afeta todos os fluxos.
- Sucesso pode cair drasticamente no nível 3 (congestionamento extremo).

---

## Cenário 5: Concorrência com Tráfego de Fundo (Background Traffic)

**O que compara:** Degradação dos protocolos quando há tráfego cross-traffic
competindo pelo mesmo gargalo. Simula cenário realista de rede compartilhada.

**Hipótese:** TCP se adapta via controle de congestionamento (fairness). UDP
não se adapta, causando unfairness e potencial colapso de congestionamento.
QUIC tem controle similar ao TCP.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=2 --numUdpClients=1 --numQuicClients=2 --connPerClient=10 --bottleneckBw=10Mbps --background=1 --bgRate=1Mbps --numBgNodes=2 --prefix=bg1 --duration=60` | Fundo 1 Mbps × 2 nós = 2 Mbps |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=2 --numUdpClients=1 --numQuicClients=2 --connPerClient=10 --bottleneckBw=10Mbps --background=1 --bgRate=5Mbps --numBgNodes=2 --prefix=bg2 --duration=60` | Fundo 5 Mbps × 2 = 10 Mbps (satura) |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=2 --numUdpClients=1 --numQuicClients=2 --connPerClient=10 --bottleneckBw=10Mbps --background=1 --bgRate=5Mbps --numBgNodes=3 --prefix=bg3 --duration=60 --bottleneckDelay=50ms` | Fundo 5 Mbps × 3 = 15 Mbps + latência |

**Métricas esperadas:**
- Nível 1: queda pequena no sucesso (~95%+)
- Nível 2 (saturação): perdas por overflow de fila, queda significativa no
  sucesso TCP/QUIC. UDP perde menos conexões (não tem handshake) mas perde
  pacotes de dados.
- Nível 3: cenário extremo, poucas conexões TCP se estabelecem.
- Identificar qual protocolo é mais resiliente sob congestão.

---

## Cenário 6: WiFi vs Cabeado

**O que compara:** Desempenho de conexões via WiFi (acesso sem fio compartilhado)
vs. enlace cabeado P2P dedicado. Simula clientes reais com acesso wireless.

**Hipótese:** WiFi adiciona latência variável e perdas por contenção. Conexões
curtas sofrem mais com overhead do WiFi (beacon, backoff, contenção).

**Comando base (WiFi, usar com --prefix=wifi):**
```bash
./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --numQuicClients=3 --connPerClient=10 --enableWifi=1 --wifiStandard=80211ac --wifiRate=VhtMcs5 --duration=60 --enableQuic=1 --prefix=wifi"
```

**Comando base (Cabeado, usar com --prefix=wired):**
```bash
./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --enableWifi=0 --duration=60 --prefix=wired"
```

| Nível | Condições |
|---|---|
| **1** | 1 cliente TCP + 1 UDP, taxa de acesso 100 Mbps |
| **2** | 5 clientes TCP + 2 UDP, taxa de acesso 50 Mbps |
| **3** | 10 clientes TCP + 5 UDP, taxa de acesso 10 Mbps, WiFi 80211g |

Nível 3 802.11g é particularmente lento comparado a 802.11ac (VHT).
Para testar o impacto do padrão WiFi:
```bash
./ns3 run "connection-success --numTcpClients=5 --numUdpClients=3 --connPerClient=10 --enableWifi=1 --wifiStandard=80211g --wifiRate=ErpOfdmRate54Mbps --prefix=wifi-2.4 --duration=60 --enableQuic=1 --numQuicClients=3"
```

**Métricas esperadas:**
- WiFi adiciona 5–20 ms extra no handshake vs cabeado.
- Sucesso WiFi < cabeado sob congestão (controle de acesso ao meio).
- 802.11g (2.4 GHz) pior que 802.11ac (5 GHz).

---

## Cenário 7: Variantes TCP (TcpNewReno vs TcpBbr vs TcpCubic vs TcpVegas)

**O que compara:** Diferentes algoritmos de controle de congestionamento TCP
no estabelecimento e throughput de conexões curtas. BBR é baseado em modelo,
Cubic/NewReno em perda, Vegas em atraso.

**Hipótese:** BBR pode ter handshake mais rápido por não precisar de perdas para
crescimento de cWnd. Vegas pode sofrer em handshakes longos (perda de sensibilidade
RTT). Cubic e NewReno similares.

| Nível | Comando | Variante |
|---|---|---|
| **1** | `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=1 --connPerClient=10 --tcpVariant=TcpNewReno --prefix=tcp-newreno --duration=60"` | TcpNewReno |
| **2** | `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=1 --connPerClient=10 --tcpVariant=TcpBbr --prefix=tcp-bbr --duration=60"` | TcpBbr |
| **3** | `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=1 --connPerClient=10 --tcpVariant=TcpVegas --prefix=tcp-vegas --duration=60"` | TcpVegas |

Testar com 5% de perda para ver diferenças:
```bash
# BBR vs NewReno sob perda
./ns3 run "connection-success --numTcpClients=3 --connPerClient=10 --tcpVariant=TcpBbr --bottleneckError=0.05 --prefix=bbr-loss --duration=60"
./ns3 run "connection-success --numTcpClients=3 --connPerClient=10 --tcpVariant=TcpNewReno --bottleneckError=0.05 --prefix=newreno-loss --duration=60"
```

**Métricas esperadas:**
- TcpNewReno: padrão, justo, mas conservador sob perda.
- TcpBbr: melhor throughput sob perda (não reage linearmente a perdas).
- TcpVegas: handshakes lentos com RTT alto (depende de RTT estável).

---

## Cenário 8: Escalonamento (Número de Conexões)

**O que compara:** Como a taxa de sucesso escala com o número de conexões
concorrentes. Testa capacidade de lidar com muitos handshakes simultâneos.

**Hipótese:** A partir de certo número, filas e buffers no roteador saturam,
causando perdas de SYN/INITIAL e timeouts.

| Nível | Comando | Conexões totais |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=5 --numQuicClients=5 --connPerClient=5 --connInterval=0.1 --bottleneckBw=100Mbps --duration=30 --prefix=scale1"` | 50 |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=20 --numQuicClients=20 --connPerClient=10 --connInterval=0.05 --bottleneckBw=100Mbps --duration=60 --prefix=scale2` | 400 |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=50 --numQuicClients=50 --connPerClient=20 --connInterval=0.02 --bottleneckBw=1Gbps --duration=120 --connTimeout=20 --prefix=scale3` | 2000 |

**Métricas esperadas:**
- Nível 1: 100% sucesso.
- Nível 2: pequena degradação (alguns timeouts).
- Nível 3: degradação perceptível, especialmente para QUIC (mais overhead
  por conexão em termos de estado). Taxa de sucesso TCP pode cair para 70–90%.
- O gargalo não é apenas banda, mas também processamento de pacotes (espera
  na fila do roteador).

---

## Cenário 9: Disciplinas de Fila (PfifoFast vs CoDel)

**O que compara:** Efeito do gerenciamento ativo de filas (AQM) vs. drop-tail
simples. CoDel controla o tamanho da fila ativamente para reduzir bufferbloat.

**Hipótese:** CoDel deve reduzir latência de handshake quando há congestão,
mantendo filas mais curtas. PfifoFast permite que filas cresçam, aumentando
latência e potencialmente causando timeouts.

| Nível | Comando | Disciplina |
|---|---|---|
| **1** | `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --bottleneckBw=5Mbps --background=1 --bgRate=3Mbps --numBgNodes=2 --queueDisc=ns3::PfifoFastQueueDisc --prefix=pfifo1 --duration=60"` | PfifoFast |
| **2** | `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --bottleneckBw=5Mbps --background=1 --bgRate=3Mbps --numBgNodes=2 --queueDisc=ns3::CoDelQueueDisc --prefix=codel1 --duration=60"` | CoDel |

**Comparação direta:**
| Nível | PfifoFast | CoDel |
|---|---|---|
| **1** | `./ns3 run "connection-success ... --prefix=pfifo --queueDisc=ns3::PfifoFastQueueDisc ..."` | `./ns3 run "connection-success ... --prefix=codel --queueDisc=ns3::CoDelQueueDisc ..."` |
| **2** (com congestão) | igual nível 1 + `--background=1 --bgRate=5Mbps` | igual nível 1 + mesma congestão |
| **3** (congestão + latência) | igual nível 2 + `--bottleneckDelay=50ms` | igual nível 2 + mesma latência |

**Métricas esperadas:**
- PfifoFast: fila cresce, latência de handshake alta sob congestão.
- CoDel: fila controlada, latência de handshake menor. Pode ter mais perdas
  no curto prazo (dropping precoce) mas recuperação mais rápida.
- Para conexões curtas (handshake + dados), CoDel pode ser pior que PfifoFast
  (descarta prematuramente pacotes de handshake). O cenário testa exatamente isso.

---

## Cenário 10: Rácio da Proporção de Conexões (TCP vs UDP vs QUIC)

**O que compara:** Fairness entre protocolos quando diferentes proporções de
TCP/UDP/QUIC competem. Simula cenários realistas com diferentes tipos de tráfego.

**Hipótese:** TCP e QUIC se comportam similarmente em termos de controle de
congestionamento. UDP sem controle ocupa banda indiscriminadamente, afetando
TCP e QUIC.

| Nível | Comando | Proporção |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=8 --numUdpClients=1 --numQuicClients=8 --connPerClient=10 --bottleneckBw=10Mbps --duration=60 --prefix=prop-8-1-8"` | TCP:UDP:QUIC = 8:1:8 |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numUdpClients=6 --numQuicClients=3 --connPerClient=10 --bottleneckBw=10Mbps --duration=60 --prefix=prop-3-6-3"` | 3:6:3 (UDP domina) |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=5 --numUdpClients=1 --numQuicClients=5 --connPerClient=10 --bottleneckBw=10Mbps --duration=60 --udpRate=10Mbps --prefix=prop-5-1-5"` | TCP/QUIC + UDP agressivo |

**Métricas esperadas:**
- Nível 1: TCP e QUIC dividem banda justamente. UDP baixo.
- Nível 2: UDP sature o link (6 × 1 Mbps = 6 Mbps em 10 Mbps), deixando
  pouco para TCP e QUIC. Sucesso TCP/QUIC cai.
- Nível 3: UDP com taxa igual ao bottleneck (10 Mbps) devora toda a banda.
  TCP/QUIC praticamente não conseguem estabelecer conexões.

---

## Cenário 11: Handshake 0-RTT do QUIC

**O que compara:** QUIC com 0-RTT (retomada de sessão) vs. QUIC full handshake
vs. TCP. 0-RTT permite enviar dados imediatamente, sem esperar handshake.

**Hipótese:** 0-RTT elimina o RTT de handshake, permitindo envio de dados
quase instantâneo. Deve ser especialmente vantajoso em redes com alta latência.

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numQuicClients=3 --numTcpClients=3 --numUdpClients=1 --connPerClient=5 --duration=30 --ns3::QuicL4Protocol::0RTT-Handshake=1 --prefix=quic-0rtt1"` | 0-RTT ativado |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numQuicClients=3 --numTcpClients=3 --numUdpClients=1 --connPerClient=5 --duration=30 --bottleneckDelay=100ms --serverDelay=50ms --ns3::QuicL4Protocol::0RTT-Handshake=1 --prefix=quic-0rtt2 --connTimeout=30"` | 0-RTT + latência alta |
| **3** | `./ns3 run "connection-success --enableQuic=1 --numQuicClients=3 --numTcpClients=3 --numUdpClients=1 --connPerClient=5 --duration=60 --bottleneckDelay=100ms --bottleneckError=0.02 --ns3::QuicL4Protocol::0RTT-Handshake=1 --prefix=quic-0rtt3 --connTimeout=30"` | 0-RTT + perda + latência |

**Métricas esperadas:**
- QUIC 0-RTT deve ter latência de primeira transmissão próxima de 0.
- Sob latência alta (nível 2 e 3), 0-RTT é dramaticamente mais rápido que TCP
  e QUIC sem 0-RTT.
- Sob perda (nível 3), 0-RTT pode falhar (dados rejeitados se as chaves de
  sessão não forem aceitas), caindo para handshake completo.

---

## Cenário 12: Perda Assimétrica (Burst Loss)

**O que compara:** Efeito de rajadas de perda (bursts) vs perda uniforme.
Rajadas simulam fading em WiFi ou congestionamento intermitente.

**Hipótese:** Perda em rajada é mais prejudicial para TCP (múltiplos segmentos
perdidos em uma janela, causando RTO) do que para QUIC (que pode ter melhor
recuperação com FEC ou retransmissão seletiva).

**Nota:** O modelo `RateErrorModel` com `ERROR_UNIT_PACKET` distribui perdas
uniformemente (Bernoulli). Para burst loss, seria necessário usar
`BurstErrorModel`. Exemplo de burst:

| Nível | Comando | Condições |
|---|---|---|
| **1** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --connPerClient=10 --bottleneckBw=10Mbps --bottleneckError=0.01 --duration=60 --prefix=uniform1"` | Perda uniforme 1% |
| **2** | `./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --connPerClient=10 --bottleneckBw=10Mbps --bottleneckError=0.01 --duration=60 --prefix=uniform2 --ns3::RateErrorModel::ErrorUnit=BURST"` | Perda em rajada |
| **3** | adicionar `--bottleneckDelay=30ms` aos comandos acima para amplificar | |

---

## Cenário 13: Vários Standards WiFi

**O que compara:** 802.11n vs 802.11ac vs 802.11ax (WiFi 4/5/6) em termos de
latência de handshake e taxa de sucesso de conexão.

**Hipótese:** WiFi mais moderno (ax/HE) tem menor latência e maior capacidade,
resultando em melhor taxa de sucesso e handshakes mais rápidos.

| Nível | Comando | Padrão / Taxa |
|---|---|---|
| **1** | `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --enableWifi=1 --wifiStandard=80211n --wifiRate=HtMcs7 --duration=60 --prefix=wifi-n"` | 802.11n HT MCS7 |
| **2** | `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --enableWifi=1 --wifiStandard=80211ac --wifiRate=VhtMcs7 --duration=60 --prefix=wifi-ac"` | 802.11ac VHT MCS7 |
| **3** | `./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10 --enableWifi=1 --wifiStandard=80211ax --wifiRate=HeMcs7 --duration=60 --prefix=wifi-ax"` | 802.11ax HE MCS7 |

**Métricas esperadas:**
- 802.11ax (WiFi 6) deve ter melhor desempenho com múltiplas estações (OFDMA,
  melhor controle de acesso ao meio).
- 802.11ac (WiFi 5) bom para poucos clientes.
- 802.11n (WiFi 4) degrada com múltiplos clientes concorrentes.
- Diferenças devem se amplificar com `--numTcpClients=10`.

---

## Como Executar em Lote

Para automatizar a execução múltipla, salve os comandos de um cenário em
um script shell:

```bash
#!/bin/bash
# Cenário 2: Packet Loss (níveis 1, 2, 3)
NAMES=("loss1" "loss2" "loss3")
ERRORS=("0.01" "0.05" "0.10")
for i in 0 1 2; do
  echo "=== Executando ${NAMES[$i]} (loss ${ERRORS[$i]}) ==="
  ./ns3 run "connection-success \
    --enableQuic=1 \
    --numTcpClients=3 \
    --numUdpClients=1 \
    --numQuicClients=3 \
    --connPerClient=10 \
    --bottleneckBw=10Mbps \
    --bottleneckError=${ERRORS[$i]} \
    --duration=60 \
    --prefix=${NAMES[$i]}"
done
echo "=== Todos concluídos ==="
```

---

## Interpretação de Resultados

Após executar cada cenário, analise:

1. **`<prefix>-r0-flows.csv`** — FlowMonitor por fluxo:
   - `LostPackets` / `TxPackets` = perda real
   - `MeanDelay_s` = atraso médio
   - `Throughput_Mbps` = vazão real

2. **Saída no terminal** — Taxa de sucesso e latência média:
   ```
   TCP   Attempts: 30, Success: 28 (93.3%), Avg handshake: 125.4 ms
   UDP   Delivery rate: 87.3%
   QUIC  Detected: 30, Success: 29 (96.7%), Avg handshake: 98.2 ms
   ```

3. **Comparações entre níveis** — Como as métricas mudam de nível 1 para 3:
   - Queda na taxa de sucesso sugere limites do protocolo
   - Aumento na latência sugere retransmissões ou filas cheias
