# Conclusão — TCP/TLS vs QUIC: Comparação de Taxa de Sucesso de Conexão

## O que este trabalho demonstra

Criamos um ambiente de simulação ns-3 para comparar o **estabelecimento de conexão**
entre **TCP (com TLS simulado)** e **QUIC (com TLS 1.3 nativo)**, sob diferentes
condições de rede: perda de pacotes, latência, gargalo de banda, tráfego de fundo,
WiFi vs cabeado, e escalonamento de conexões.

## A métrica principal: latência de handshake efetiva

| Protocolo | Medição | O que está incluso |
|---|---|---|
| **TCP** | `Connect()` → primeiro dado ecoado de volta | 3-way handshake + TLS ClientHello + ServerHello + HTTP request/response (~2.5 RTT) |
| **QUIC** | `HANDSHAKE` recebido → primeiro `Short` header recebido | Handshake QUIC já inclui TLS 1.3 completo (~1 RTT) |

## Resultados empíricos (rede limpa, WiFi 802.11ac, bottleneck 5 Mbps)

```
TCP:  Latência média: 117.70 ms  (3-way + TLS + HTTP echo)
QUIC: Latência média: 61.08 ms   (handshake com TLS 1.3 incluso)
```

**QUIC é aproximadamente 2× mais rápido que TCP/TLS no estabelecimento de conexão.**

## Por que QUIC é mais rápido?

- **TCP**: o cliente faz o 3-way handshake (SYN → SYN+ACK → ACK, ~1.5 RTT). Depois,
  precisa negociar TLS (ClientHello → ServerHello, mais ~1.5 RTT com certificados e
  chaves). Total: ~2.5–3 RTT até a primeira requisição HTTP poder trafegar.
- **QUIC**: o handshake já incorpora TLS 1.3. Uma única troca de mensagens
  (INITIAL → HANDSHAKE) estabelece a conexão criptografada. Total: ~1 RTT.

## 0-RTT do QUIC (não funcional no ns-3.47)

O QUIC suporta o modo **0-RTT** (retomada de sessão), onde o cliente pode
enviar dados imediatamente na primeira mensagem, sem qualquer RTT de handshake.
Isso representaria uma vantagem dramática sobre TCP/TLS.

**Nota:** a implementação do QUIC no ns-3.47 possui uma limitação —
`"0RTT Handshake requested with wrong Initial Version"` — que impede o uso
do 0-RTT. O erro está no módulo QUIC do simulador (`quic-socket-base.cc:2545`),
não no script de simulação. O cenário 0-RTT está documentado no CENARIOS.md
mas não pode ser executado nesta versão do ns-3.

## Como o TCP/TLS foi simulado

O ns-3 não possui um módulo TLS pronto. Para simular o overhead do TLS sobre TCP,
implementamos um **servidor echo TCP** que:

1. Aceita a conexão TCP (3-way handshake) — métrica base (~55ms)
2. O cliente envia `--tlsPayloadSize` bytes (default 3000, simulando ClientHello)
3. O servidor ecoa os dados de volta (simulando ServerHello + Certificate + Finished)
4. O cliente detecta o eco e registra o tempo total — métrica final (~118ms)

Isso captura o custo adicional de **1 RTT completo** para a troca de dados
TLS/HTTP, que é uma aproximação conservadora (o TLS real pode levar mais RTTs
dependendo do tamanho dos certificados).

## Cenários onde QUIC brilha

| Cenário | Vantagem QUIC | Explicação |
|---|---|---|
| **Latência alta** (satélite, transatlântico) | Maior | QUIC completa em 1 RTT vs 2.5+ RTT do TCP/TLS |
| **Perda de pacotes** (redes sem fio) | Significativa | QUIC recupera perdas por stream (sem HoL blocking) |
| **Múltiplas conexões** | Moderada | Multiplexação de streams evita múltiplos handshakes |
| **0-RTT ativado** | Absoluta | Dados fluem imediatamente, sem qualquer RTT |
| **WiFi congestionado** | Leve | Controle de congestionamento similar ao TCP, mas sem fila HoL |

## Limitações da simulação

1. **TLS simulado, não real**: o echo TCP é uma aproximação. O TLS real tem
   custos computacionais e de tamanho de pacote que não foram modelados.
2. **Conexões curtas**: focamos no handshake, não na transferência de longa
   duração. O QUIC é ainda mais vantajoso em conexões curtas (típicas da web).
3. **QUIC do ns-3**: a implementação do QUIC no ns-3 pode não refletir todas
   as otimizações do QUIC real (como migração de conexão, FEC, etc.).

## Como reproduzir as medições

```bash
# Cenário padrão (TCP vs QUIC, com TLS simulado)
./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numQuicClients=3 \
  --connPerClient=10 --tlsPayloadSize=3000 --duration=60 --bottleneckBw=10Mbps \
  --outputDir=comparativo/padrao"

# Com perda (QUIC deve ser mais resiliente)
./ns3 run "connection-success --enableQuic=1 --numTcpClients=3 --numQuicClients=3 \
  --connPerClient=10 --tlsPayloadSize=3000 --duration=60 --bottleneckBw=10Mbps \
  --bottleneckError=0.05 --outputDir=comparativo/perda-5pct"
```

## Referências

- QUIC RFC 9000: https://www.rfc-editor.org/rfc/rfc9000
- TLS 1.3 RFC 8446: https://www.rfc-editor.org/rfc/rfc8446
- Artigo original Google QUIC: https://dl.acm.org/doi/10.1145/3098822.3098842
