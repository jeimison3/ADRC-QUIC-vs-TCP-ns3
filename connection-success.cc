/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2024
 *
 * Taxa de Sucesso de Conexão — TCP vs UDP vs QUIC
 * sobre Topologia Mista Cabeada / Sem-Fio
 *
 * Inherits topology control from tcp-udp-comparison.cc:
 *  [Server] ---P2P--- [Router1] ---P2P(bottleneck)--- [Router2] ---P2P--- [AP] ~~~WiFi~~~ [STAs]
 *                                                                       \---P2P--- [WiredClients]
 *
 * Metrics measured:
 *   - TCP: SYN→ESTABLISHED success rate & handshake latency (socket callbacks)
 *   - UDP: datagram delivery rate (PacketSink-based)
 *   - QUIC: HANDSHAKE→first Short success rate & latency (Rx trace on client side)
 *
 * Output (all saved to --outputDir, default: scratch/RESULTADOS/EXEC-<timestamp>/):
 *   - summary.csv     : one row per protocol per experiment (for plotting)
 *   - connections.csv : per-connection latencies and results
 *   - flows.csv       : per-flow FlowMonitor data
 *   - flows.flowmonitor : FlowMonitor XML
 *
 * Usage:
 *   ./ns3 run "connection-success --numTcpClients=3 --connPerClient=10"
 *   ./ns3 run "connection-success --enableQuic=1 --numQuicClients=2"
 *   ./ns3 run "connection-success --tcpVariant=TcpBbr --bottleneckError=0.02 --outputDir=meus-testes"
 */

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <map>
#include <vector>
#include <iomanip>
#include <ctime>
#include <sys/stat.h>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/error-model.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/traffic-control-module.h"
#include "ns3/mobility-module.h"
#include "ns3/ssid.h"
#include "ns3/yans-wifi-channel.h"
#include "ns3/yans-wifi-helper.h"
#include "ns3/tcp-socket.h"

// QUIC module (optional — set enableQuic=1 to activate)
#include "ns3/quic-module.h"
#include "ns3/quic-header.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ConnectionSuccess");

// ===========================================================================
// Statistics accumulators
// ===========================================================================

struct TcpConnectionResult
{
  double startTime;
  double establishedTime;
  double closeTime;
  double latencyMs;
  bool succeeded;
  bool gracefulClose;
  std::string failureReason;
};

struct QuicConnectionResult
{
  double handshakeTime;
  double openTime;
  double latencyMs;
  bool succeeded;
  bool is0RTT;
  std::string failureReason;
};

// Global accumulators
static std::vector<TcpConnectionResult> g_tcpResults;
static std::vector<QuicConnectionResult> g_quicResults;

static std::map<Ptr<Socket>, TcpConnectionResult> g_pendingTcp;
static std::map<Ptr<Socket>, double> g_tcpStartTimes;

static std::map<Ptr<const QuicSocketBase>, QuicConnectionResult> g_pendingQuic;
static std::map<Ptr<const QuicSocketBase>, int> g_quicRxStage; // 0=none, 2=handshake seen

static double g_timeoutLimit = 10.0;
static bool g_verboseConn = false;
static uint32_t g_tcpCompleted = 0;
static uint32_t g_tcpTotal = 0;

// Output directory
static std::string g_outputDir;

// ===========================================================================
// Directory helpers
// ===========================================================================

static bool
DirectoryExists(const std::string& path)
{
  struct stat st;
  return (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

static void
CreateDirectory(const std::string& path)
{
  if (DirectoryExists(path)) return;
  std::string cmd = "mkdir -p \"" + path + "\"";
  int ret = system(cmd.c_str());
  if (ret != 0)
    std::cerr << "Warning: mkdir failed for " << path << std::endl;
}

static std::string
Timestamp()
{
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d-%H-%M-%S", t);
  return std::string(buf);
}

// ===========================================================================
// TCP Socket Callbacks
// ===========================================================================

static void
OnTcpConnectionSucceeded(Ptr<Socket> sock)
{
  auto it = g_tcpStartTimes.find(sock);
  if (it == g_tcpStartTimes.end()) return;

  double now = Simulator::Now().GetSeconds();
  double latMs = (now - it->second) * 1000.0;

  TcpConnectionResult r;
  r.startTime = it->second;
  r.establishedTime = now;
  r.latencyMs = latMs;
  r.succeeded = true;
  r.gracefulClose = false;
  r.failureReason = "";
  g_pendingTcp[sock] = r;
  g_tcpStartTimes.erase(it);

  g_tcpCompleted++;

  if (g_verboseConn)
    std::cout << now << "s TCP-OK [" << g_tcpCompleted << "/" << g_tcpTotal
              << "] hs=" << latMs << " ms" << std::endl;
}

static void
OnTcpConnectionFailed(Ptr<Socket> sock)
{
  auto it = g_tcpStartTimes.find(sock);
  if (it == g_tcpStartTimes.end()) return;

  TcpConnectionResult r;
  r.startTime = it->second;
  r.establishedTime = 0;
  r.latencyMs = -1;
  r.succeeded = false;
  r.failureReason = "ConnectFailed";
  g_tcpResults.push_back(r);
  g_tcpStartTimes.erase(it);
  g_tcpCompleted++;

  if (g_verboseConn)
    std::cout << Simulator::Now().GetSeconds()
              << "s TCP-FAIL: " << r.failureReason << std::endl;
}

static void
OnTcpNormalClose(Ptr<Socket> sock)
{
  auto it = g_pendingTcp.find(sock);
  if (it != g_pendingTcp.end())
    {
      it->second.closeTime = Simulator::Now().GetSeconds();
      it->second.gracefulClose = true;
      g_tcpResults.push_back(it->second);
      g_pendingTcp.erase(it);
    }
  auto it2 = g_tcpStartTimes.find(sock);
  if (it2 != g_tcpStartTimes.end())
    g_tcpStartTimes.erase(it2);
}

static void
OnTcpErrorClose(Ptr<Socket> sock)
{
  auto it = g_pendingTcp.find(sock);
  if (it != g_pendingTcp.end())
    {
      it->second.closeTime = Simulator::Now().GetSeconds();
      it->second.gracefulClose = false;
      it->second.succeeded = false;
      it->second.failureReason = "ErrorClose";
      g_tcpResults.push_back(it->second);
      g_pendingTcp.erase(it);
    }
  auto it2 = g_tcpStartTimes.find(sock);
  if (it2 != g_tcpStartTimes.end())
    g_tcpStartTimes.erase(it2);
}

static void
OnTcpDataReceived(Ptr<Socket> sock)
{
  Ptr<Packet> p;
  while ((p = sock->Recv()))
    { }
}

// ===========================================================================
// QUIC Rx trace
// ===========================================================================

static void
OnQuicRx(Ptr<const Packet> p, const QuicHeader& h, Ptr<const QuicSocketBase> s);

static void
OnQuicRx(Ptr<const Packet> p, const QuicHeader& h, Ptr<const QuicSocketBase> s)
{
  if (!s) return;
  double now = Simulator::Now().GetSeconds();

  if (h.IsHandshake())
    {
      if (g_quicRxStage.find(s) == g_quicRxStage.end() || g_quicRxStage[s] < 2)
        {
          g_quicRxStage[s] = 2;
          QuicConnectionResult r;
          r.handshakeTime = now;
          r.openTime = 0;
          r.latencyMs = -1;
          r.succeeded = false;
          r.is0RTT = false;
          r.failureReason = "";
          g_pendingQuic[s] = r;
          if (g_verboseConn)
            std::cout << now << "s QUIC-HANDSHAKE on client" << std::endl;
        }
    }
  else if (h.IsShort())
    {
      auto it = g_pendingQuic.find(s);
      if (it != g_pendingQuic.end() && !it->second.succeeded)
        {
          it->second.openTime = now;
          it->second.latencyMs = (now - it->second.handshakeTime) * 1000.0;
          it->second.succeeded = true;
          g_quicResults.push_back(it->second);
          g_pendingQuic.erase(it);
          if (g_verboseConn)
            std::cout << now << "s QUIC-OPEN hs-lat=" << it->second.latencyMs << " ms" << std::endl;
        }
    }
  else if (h.IsORTT())
    {
      auto it = g_pendingQuic.find(s);
      if (it != g_pendingQuic.end() && !it->second.succeeded)
        {
          it->second.is0RTT = true;
          it->second.openTime = now;
          it->second.latencyMs = (now - it->second.handshakeTime) * 1000.0;
          it->second.succeeded = true;
          g_quicResults.push_back(it->second);
          g_pendingQuic.erase(it);
          if (g_verboseConn)
            std::cout << now << "s QUIC-0RTT lat=" << it->second.latencyMs << " ms" << std::endl;
        }
    }
}

// ===========================================================================
// QUIC trace hookup
// ===========================================================================

static void
HookQuicTraces(Ptr<Node> /*server*/, NodeContainer quicStaNodes)
{
  for (uint32_t i = 0; i < quicStaNodes.GetN(); i++)
    {
      std::ostringstream rxPath;
      rxPath << "/NodeList/" << quicStaNodes.Get(i)->GetId()
             << "/$ns3::QuicL4Protocol/SocketList/*/QuicSocketBase/Rx";
      Config::ConnectWithoutContext(rxPath.str(), MakeCallback(&OnQuicRx));
    }
}

// ===========================================================================
// Connection scheduler — TCP
// ===========================================================================

static void
ScheduleTcpConnection(Ptr<Node> client, Ipv4Address serverAddr, uint16_t port,
                      uint32_t remaining, Time interval, Time /*timeout*/)
{
  if (remaining == 0) return;

  Ptr<Socket> sock = Socket::CreateSocket(client, TcpSocketFactory::GetTypeId());
  sock->SetConnectCallback(
    MakeCallback(&OnTcpConnectionSucceeded),
    MakeCallback(&OnTcpConnectionFailed));
  sock->SetCloseCallbacks(
    MakeCallback(&OnTcpNormalClose),
    MakeCallback(&OnTcpErrorClose));
  sock->SetRecvCallback(MakeCallback(&OnTcpDataReceived));

  double now = Simulator::Now().GetSeconds();
  g_tcpStartTimes[sock] = now;

  int ret = sock->Connect(InetSocketAddress(serverAddr, port));
  if (ret != 0)
    {
      TcpConnectionResult r;
      r.startTime = now;
      r.latencyMs = -1;
      r.succeeded = false;
      r.failureReason = "ImmediateConnectError";
      g_tcpResults.push_back(r);
      g_tcpStartTimes.erase(sock);
      g_tcpCompleted++;
    }

  Simulator::Schedule(interval,
                      &ScheduleTcpConnection,
                      client, serverAddr, port,
                      remaining - 1, interval, interval);
}

// ===========================================================================
// Timeout checker
// ===========================================================================

static void
CheckTimeouts(double timeoutLimit)
{
  double now = Simulator::Now().GetSeconds();

  for (auto it = g_tcpStartTimes.begin(); it != g_tcpStartTimes.end(); )
    {
      if (now - it->second > timeoutLimit)
        {
          TcpConnectionResult r;
          r.startTime = it->second;
          r.latencyMs = -1;
          r.succeeded = false;
          r.failureReason = "Timeout";
          g_tcpResults.push_back(r);
          g_tcpCompleted++;
          auto pit = g_pendingTcp.find(it->first);
          if (pit != g_pendingTcp.end()) g_pendingTcp.erase(pit);
          it = g_tcpStartTimes.erase(it);
        }
      else { ++it; }
    }

  for (auto it = g_pendingQuic.begin(); it != g_pendingQuic.end(); )
    {
      if (!it->second.succeeded && now - it->second.handshakeTime > timeoutLimit)
        {
          it->second.failureReason = "Timeout";
          it->second.latencyMs = -1;
          g_quicResults.push_back(it->second);
          it = g_pendingQuic.erase(it);
        }
      else { ++it; }
    }

  Simulator::Schedule(Seconds(0.5), &CheckTimeouts, timeoutLimit);
}

// ===========================================================================
// Write summary CSV (one row per protocol per experiment)
// ===========================================================================

static void
WriteSummaryCsv(const std::string& path,
                uint32_t tcpAttempts, uint32_t tcpSuccess,
                double tcpAvgLatMs, double tcpMinLatMs, double tcpMaxLatMs,
                uint32_t quicSuccess, uint32_t quicTotal, uint32_t quic0RTT,
                double quicAvgLatMs, double quicMinLatMs, double quicMaxLatMs,
                const std::string& tcpVariant,
                uint32_t numTcpClients,
                uint32_t numQuicClients, uint32_t connPerClient,
                double connInterval, double connTimeout,
                const std::string& bottleneckBw, const std::string& bottleneckDelay,
                double bottleneckError,
                const std::string& serverBw, const std::string& serverDelay,
                const std::string& accessBw, const std::string& accessDelay,
                bool enableWifi, const std::string& wifiStdStr,
                const std::string& wifiRate,
                bool enableBackground, uint32_t numBgNodes, const std::string& bgRate,
                const std::string& queueDisc,
                double duration, uint32_t run,
                bool enableQuic)
{
  // Check if file exists to write header
  bool exists = false;
  {
    std::ifstream test(path);
    exists = test.good();
  }

  std::ofstream csv(path, std::ios::app);
  if (!exists)
    {
      csv << "Protocolo,Execucao,Timestamp,"
          << "Sucessos,Tentativas,TaxaSucesso,"
          << "LatenciaMediaMs,LatenciaMinMs,LatenciaMaxMs,"
          << "BytesRx,PacotesRx,PacotesEnviadosEst,TaxaEntrega,"
          << "Fluxos0RTT,"
          << "VarianteTCP,GargaloBw,GargaloDelay,GargaloErro,"
          << "ServidorBw,ServidorDelay,AcessoBw,AcessoDelay,"
          << "WiFi,WiFiPadrao,WiFiTaxa,"
          << "DisciplinaFila,TraficoFundo,TaxaFundo,NosFundo,"
          << "NumClientes,ConexoesPorCliente,IntervaloConexoes,TimeoutHandshake,Duracao"
          << std::endl;
    }

  std::string ts = Timestamp();

  auto writeRow = [&](const std::string& proto,
                       uint32_t success, uint32_t attempts,
                       double avgLat, double minLat, double maxLat,
                       uint64_t bytesRx, uint64_t pktsRx, uint64_t pktsSent,
                       double deliveryRate, uint32_t zeroRtt)
  {
    double rate = attempts > 0 ? 100.0 * success / attempts : 0;
    csv << proto << "," << run << "," << ts << ","
        << success << "," << attempts << "," << std::fixed << std::setprecision(2) << rate << ","
        << avgLat << "," << minLat << "," << maxLat << ","
        << bytesRx << "," << pktsRx << "," << pktsSent << ","
        << std::setprecision(1) << deliveryRate << ","
        << zeroRtt << ","
        << tcpVariant << ","
        << bottleneckBw << "," << bottleneckDelay << "," << bottleneckError << ","
        << serverBw << "," << serverDelay << ","
        << accessBw << "," << accessDelay << ","
        << (enableWifi ? 1 : 0) << "," << wifiStdStr << "," << wifiRate << ","
        << queueDisc << ","
        << (enableBackground ? 1 : 0) << "," << bgRate << "," << numBgNodes << ","
        << (proto == "TCP" ? numTcpClients : numQuicClients)
        << "," << connPerClient << "," << connInterval << "," << connTimeout << "," << duration
        << std::endl;
  };

  // TCP row
  {
    writeRow("TCP", tcpSuccess, tcpAttempts,
             tcpAvgLatMs, tcpMinLatMs, tcpMaxLatMs,
             0, 0, 0, 0, 0);
  }

  // QUIC row (if enabled)
  if (enableQuic && numQuicClients > 0)
    {
      writeRow("QUIC", quicSuccess, quicTotal,
               quicAvgLatMs, quicMinLatMs, quicMaxLatMs,
               0, 0, 0, 0, quic0RTT);
    }

  csv.close();
}

// ===========================================================================
// Write per-connection details CSV
// ===========================================================================

static void
WriteConnectionsCsv(const std::string& path)
{
  std::ofstream csv(path);
  csv << "Protocolo,Inicio,LatenciaMs,Sucesso,MotivoFalha,Is0RTT" << std::endl;
  for (auto& r : g_tcpResults)
    {
      csv << "TCP," << r.startTime << "," << r.latencyMs << ","
          << (r.succeeded ? 1 : 0) << "," << r.failureReason << ",0" << std::endl;
    }
  for (auto& entry : g_pendingTcp)
    {
      csv << "TCP," << entry.second.startTime << "," << entry.second.latencyMs << ","
          << (entry.second.succeeded ? 1 : 0) << ",Pending,0" << std::endl;
    }
  for (auto& r : g_quicResults)
    {
      csv << "QUIC," << r.handshakeTime << "," << r.latencyMs << ","
          << (r.succeeded ? 1 : 0) << "," << r.failureReason << ","
          << (r.is0RTT ? 1 : 0) << std::endl;
    }
  csv.close();
}

// ===========================================================================
// MAIN
// ===========================================================================

int main(int argc, char* argv[])
{
  // -----------------------------------------------------------------------
  // Command-line parameters
  // -----------------------------------------------------------------------
  std::string tcpVariant = "TcpNewReno";
  std::string serverBandwidth = "100Mbps";
  std::string serverDelay = "5ms";
  std::string bottleneckBandwidth = "10Mbps";
  std::string bottleneckDelay = "20ms";
  double bottleneckErrorRate = 0.0;
  std::string accessBandwidth = "100Mbps";
  std::string accessDelay = "2ms";

  uint32_t numTcpClients = 2;
  uint32_t connPerClient = 5;
  double connectionInterval = 0.5;
  double connectionTimeout = 10.0;
  uint32_t payloadSize = 1400;
  double simulationTime = 60.0;

  bool enableQuic = false;
  uint32_t numQuicClients = 0;

  bool enableWifi = true;
  std::string wifiStandardStr = "80211ac";
  std::string wifiRate = "VhtMcs7";

  bool enableBackground = false;
  uint32_t numBgNodes = 2;
  std::string bgRate = "2Mbps";

  bool flowMonitor = true;
  bool pcap = false;
  std::string queueDisc = "ns3::PfifoFastQueueDisc";
  std::string outputDir;
  uint32_t run = 0;
  bool verbose = false;

  CommandLine cmd(__FILE__);

  // Links
  cmd.AddValue("serverBw", "Server link bandwidth", serverBandwidth);
  cmd.AddValue("serverDelay", "Server link delay", serverDelay);
  cmd.AddValue("bottleneckBw", "Bottleneck link bandwidth", bottleneckBandwidth);
  cmd.AddValue("bottleneckDelay", "Bottleneck link delay", bottleneckDelay);
  cmd.AddValue("bottleneckError", "Bottleneck packet error rate (0.0–1.0)",
               bottleneckErrorRate);
  cmd.AddValue("accessBw", "Access link bandwidth", accessBandwidth);
  cmd.AddValue("accessDelay", "Access link delay", accessDelay);

  // Protocols
  cmd.AddValue("tcpVariant",
               "TCP congestion control: TcpNewReno, TcpBbr, TcpCubic, etc.",
               tcpVariant);
  cmd.AddValue("numTcpClients", "Number of TCP client nodes", numTcpClients);
  cmd.AddValue("connPerClient", "TCP connections per client", connPerClient);
  cmd.AddValue("connInterval", "Interval between TCP connections (seconds)",
               connectionInterval);
  cmd.AddValue("connTimeout", "Handshake timeout (seconds)", connectionTimeout);
  cmd.AddValue("payloadSize", "Payload size in bytes", payloadSize);

  // QUIC
  cmd.AddValue("enableQuic", "Enable QUIC tests (0/1)", enableQuic);
  cmd.AddValue("numQuicClients", "Number of QUIC client nodes", numQuicClients);

  // WiFi
  cmd.AddValue("enableWifi", "Enable WiFi (1=yes, 0=wired only)", enableWifi);
  cmd.AddValue("wifiStandard",
               "WiFi standard: 80211a, 80211b, 80211g, 80211n, 80211ac, 80211ax",
               wifiStandardStr);
  cmd.AddValue("wifiRate", "WiFi data mode (VhtMcs7, etc.)", wifiRate);

  // Background
  cmd.AddValue("background", "Enable background cross-traffic (0/1)",
               enableBackground);
  cmd.AddValue("numBgNodes", "Number of background nodes", numBgNodes);
  cmd.AddValue("bgRate", "Background traffic rate per node", bgRate);

  // Control
  cmd.AddValue("duration", "Simulation duration in seconds", simulationTime);
  cmd.AddValue("run", "Run index for RNG seed", run);
  cmd.AddValue("flowMonitor", "Enable FlowMonitor (0/1)", flowMonitor);
  cmd.AddValue("pcap", "Enable PCAP tracing (0/1)", pcap);
  cmd.AddValue("queueDisc",
               "Queue discipline: ns3::PfifoFastQueueDisc or ns3::CoDelQueueDisc",
               queueDisc);
  cmd.AddValue("outputDir",
               "Output directory (default: scratch/RESULTADOS/EXEC-<timestamp>)",
               outputDir);
  cmd.AddValue("verbose", "Enable verbose logging (0/1)", verbose);
  cmd.AddValue("verboseConn", "Enable per-connection logging (0/1)", g_verboseConn);

  cmd.Parse(argc, argv);

  g_timeoutLimit = connectionTimeout;
  g_tcpTotal = numTcpClients * connPerClient;

  // -----------------------------------------------------------------------
  // Output directory
  // -----------------------------------------------------------------------
  if (outputDir.empty())
    outputDir = "scratch/RESULTADOS/EXEC-" + Timestamp();
  CreateDirectory(outputDir);

  // -----------------------------------------------------------------------
  // Seed, logging, TCP variant
  // -----------------------------------------------------------------------
  SeedManager::SetSeed(1);
  SeedManager::SetRun(run);
  Time::SetResolution(Time::NS);

  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(payloadSize));
  Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(1 << 21));
  Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(1 << 21));

  tcpVariant = std::string("ns3::") + tcpVariant;
  TypeId tcpTid;
  NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(tcpVariant, &tcpTid),
                      "TypeId " << tcpVariant << " not found");
  Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                     TypeIdValue(TypeId::LookupByName(tcpVariant)));

  // QUIC transport (if enabled)
  if (enableQuic)
    {
      Config::SetDefault("ns3::QuicL4Protocol::SocketType",
                         TypeIdValue(TypeId::LookupByName(tcpVariant)));
      Config::SetDefault("ns3::QuicSocketBase::SocketRcvBufSize",
                         UintegerValue(1 << 21));
      Config::SetDefault("ns3::QuicSocketBase::SocketSndBufSize",
                         UintegerValue(1 << 21));
      Config::SetDefault("ns3::QuicStreamBase::StreamSndBufSize",
                         UintegerValue(1 << 21));
      Config::SetDefault("ns3::QuicStreamBase::StreamRcvBufSize",
                         UintegerValue(1 << 21));
    }

  // -----------------------------------------------------------------------
  // WiFi standard parsing
  // -----------------------------------------------------------------------
  WifiStandard wifiStd = WIFI_STANDARD_80211ac;
  double wifiFreq = 5e9;
  std::string wifiCtrlRate = "VhtMcs0";

  if (wifiStandardStr == "80211a")
    { wifiStd = WIFI_STANDARD_80211a; wifiFreq = 5e9; wifiCtrlRate = "OfdmRate6Mbps"; }
  else if (wifiStandardStr == "80211b")
    { wifiStd = WIFI_STANDARD_80211b; wifiFreq = 2.4e9; wifiCtrlRate = "DsssRate1Mbps"; }
  else if (wifiStandardStr == "80211g")
    { wifiStd = WIFI_STANDARD_80211g; wifiFreq = 2.4e9; wifiCtrlRate = "ErpOfdmRate6Mbps"; }
  else if (wifiStandardStr == "80211n")
    { wifiStd = WIFI_STANDARD_80211n; wifiFreq = 5e9; wifiCtrlRate = "HtMcs0"; }
  else if (wifiStandardStr == "80211ac")
    { wifiStd = WIFI_STANDARD_80211ac; wifiFreq = 5e9; wifiCtrlRate = "VhtMcs0"; }
  else if (wifiStandardStr == "80211ax")
    { wifiStd = WIFI_STANDARD_80211ax; wifiFreq = 5e9; wifiCtrlRate = "HeMcs0"; }
  else
    { NS_FATAL_ERROR("Unknown WiFi standard: " << wifiStandardStr); }

  double warmupTime = 1.0;
  double stopTime = warmupTime + simulationTime;

  // =======================================================================
  // TOPOLOGY CREATION (same as tcp-udp-comparison.cc)
  // =======================================================================

  NodeContainer serverNode;
  serverNode.Create(1);

  NodeContainer routerNodes;
  routerNodes.Create(2);
  Ptr<Node> router1 = routerNodes.Get(0);
  Ptr<Node> router2 = routerNodes.Get(1);

  NodeContainer apNode;
  apNode.Create(1);

  NodeContainer tcpStaNodes;
  tcpStaNodes.Create(numTcpClients);
  NodeContainer quicStaNodes;
  if (enableQuic) quicStaNodes.Create(numQuicClients);

  NodeContainer wifiStaNodes;
  wifiStaNodes.Add(tcpStaNodes);
  if (enableQuic) wifiStaNodes.Add(quicStaNodes);

  NodeContainer bgNodes;
  if (enableBackground) bgNodes.Create(numBgNodes);

  // -----------------------------------------------------------------------
  // Links
  // -----------------------------------------------------------------------
  Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
  uv->SetStream(50);
  RateErrorModel errorModel;
  errorModel.SetRandomVariable(uv);
  errorModel.SetUnit(RateErrorModel::ERROR_UNIT_PACKET);
  errorModel.SetRate(bottleneckErrorRate);

  PointToPointHelper serverLink;
  serverLink.SetDeviceAttribute("DataRate", StringValue(serverBandwidth));
  serverLink.SetChannelAttribute("Delay", StringValue(serverDelay));

  PointToPointHelper bottleneckLink;
  bottleneckLink.SetDeviceAttribute("DataRate", StringValue(bottleneckBandwidth));
  bottleneckLink.SetChannelAttribute("Delay", StringValue(bottleneckDelay));
  bottleneckLink.SetDeviceAttribute("ReceiveErrorModel", PointerValue(&errorModel));

  PointToPointHelper accessLink;
  accessLink.SetDeviceAttribute("DataRate", StringValue(accessBandwidth));
  accessLink.SetChannelAttribute("Delay", StringValue(accessDelay));

  // -----------------------------------------------------------------------
  // Internet stacks
  // -----------------------------------------------------------------------
  InternetStackHelper internet;
  internet.Install(serverNode);
  internet.Install(routerNodes);
  internet.Install(apNode);
  internet.Install(bgNodes);

  Config::Set("/NodeList/" + std::to_string(router1->GetId()) +
              "/$ns3::Ipv4/IpForward", BooleanValue(true));
  Config::Set("/NodeList/" + std::to_string(router2->GetId()) +
              "/$ns3::Ipv4/IpForward", BooleanValue(true));
  Config::Set("/NodeList/" + std::to_string(apNode.Get(0)->GetId()) +
              "/$ns3::Ipv4/IpForward", BooleanValue(true));

  // -----------------------------------------------------------------------
  // Queue discipline
  // -----------------------------------------------------------------------
  TrafficControlHelper tch;
  if (queueDisc == "ns3::PfifoFastQueueDisc")
    tch.SetRootQueueDisc("ns3::PfifoFastQueueDisc");
  else if (queueDisc == "ns3::CoDelQueueDisc")
    tch.SetRootQueueDisc("ns3::CoDelQueueDisc");
  else
    NS_FATAL_ERROR("Unknown queue disc: " << queueDisc);

  DataRate bottleBw(bottleneckBandwidth);
  Time bottleD(bottleneckDelay);
  uint32_t bdpBytes = static_cast<uint32_t>(
    bottleBw.GetBitRate() / 8.0 * bottleD.GetSeconds() * 2.0);
  Config::SetDefault("ns3::PfifoFastQueueDisc::MaxSize",
                     QueueSizeValue(QueueSize(QueueSizeUnit::PACKETS,
                       std::max<uint32_t>(100, bdpBytes / payloadSize))));
  Config::SetDefault("ns3::CoDelQueueDisc::MaxSize",
                     QueueSizeValue(QueueSize(QueueSizeUnit::BYTES,
                       std::max<uint32_t>(150000, bdpBytes))));

  // =======================================================================
  // WIRED LINKS
  // =======================================================================
  Ipv4AddressHelper address;
  Ipv4InterfaceContainer serverIfaces;

  NetDeviceContainer devSR1 = serverLink.Install(serverNode.Get(0), router1);
  tch.Install(devSR1);
  address.SetBase("10.0.0.0", "255.255.255.0");
  serverIfaces = address.Assign(devSR1);

  NetDeviceContainer devR1R2 = bottleneckLink.Install(router1, router2);
  tch.Install(devR1R2);
  address.SetBase("10.0.1.0", "255.255.255.0");
  address.Assign(devR1R2);

  NetDeviceContainer devR2Ap = accessLink.Install(router2, apNode.Get(0));
  tch.Install(devR2Ap);
  address.SetBase("10.0.2.0", "255.255.255.0");
  address.Assign(devR2Ap);

  for (uint32_t i = 0; i < bgNodes.GetN(); i++)
    {
      NetDeviceContainer dev = accessLink.Install(router2, bgNodes.Get(i));
      tch.Install(dev);
      address.SetBase(("10.0." + std::to_string(100 + i) + ".0").c_str(), "255.255.255.0");
      address.Assign(dev);
    }

  // =======================================================================
  // WIFI
  // =======================================================================
  Ipv4InterfaceContainer apWifiIfaces;

  if (enableWifi && wifiStaNodes.GetN() > 0)
    {
      internet.Install(wifiStaNodes);

      if (enableQuic && numQuicClients > 0)
        {
          QuicHelper quicStack;
          quicStack.InstallQuic(quicStaNodes);
          quicStack.InstallQuic(serverNode);
        }

      YansWifiChannelHelper wifiChannel;
      wifiChannel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
      wifiChannel.AddPropagationLoss("ns3::FriisPropagationLossModel",
                                     "Frequency", DoubleValue(wifiFreq));

      YansWifiPhyHelper wifiPhy;
      wifiPhy.SetChannel(wifiChannel.Create());
      wifiPhy.SetErrorRateModel("ns3::YansErrorRateModel");

      if (wifiStd == WIFI_STANDARD_80211n || wifiStd == WIFI_STANDARD_80211ac ||
          wifiStd == WIFI_STANDARD_80211ax || wifiStd == WIFI_STANDARD_80211be)
        {
          std::string band = (wifiFreq < 3e9) ? "BAND_2_4GHZ" : "BAND_5GHZ";
          wifiPhy.Set("ChannelSettings", StringValue("{0, 20, " + band + ", 0}"));
        }

      WifiHelper wifi;
      wifi.SetStandard(wifiStd);
      wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                   "DataMode", StringValue(wifiRate),
                                   "ControlMode", StringValue(wifiCtrlRate));

      WifiMacHelper wifiMac;
      Ssid ssid = Ssid("conn-success-test");

      wifiMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid),
                      "EnableBeaconJitter", BooleanValue(false));
      NetDeviceContainer apWifiDev = wifi.Install(wifiPhy, wifiMac, apNode.Get(0));

      wifiMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid));
      NetDeviceContainer staWifiDev = wifi.Install(wifiPhy, wifiMac, wifiStaNodes);

      MobilityHelper mobility;
      Ptr<ListPositionAllocator> posAlloc = CreateObject<ListPositionAllocator>();
      posAlloc->Add(Vector(0.0, 0.0, 0.0));
      for (uint32_t i = 0; i < wifiStaNodes.GetN(); i++)
        posAlloc->Add(Vector(5.0 + i * 2.0, 0.0, 0.0));
      mobility.SetPositionAllocator(posAlloc);
      mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
      mobility.Install(apNode);
      mobility.Install(wifiStaNodes);

      Ipv4AddressHelper wifiAddress;
      wifiAddress.SetBase("10.0.200.0", "255.255.255.0");
      wifiAddress.Assign(apWifiDev);
      wifiAddress.Assign(staWifiDev);
    }
  else
    {
      // No WiFi: connect STAs via P2P directly
      internet.Install(tcpStaNodes);
      if (enableQuic) internet.Install(quicStaNodes);

      for (uint32_t i = 0; i < tcpStaNodes.GetN(); i++)
        {
          NetDeviceContainer dev = accessLink.Install(router2, tcpStaNodes.Get(i));
          tch.Install(dev);
          address.SetBase(("10.0." + std::to_string(10 + i) + ".0").c_str(), "255.255.255.0");
          address.Assign(dev);
        }
      for (uint32_t i = 0; i < quicStaNodes.GetN(); i++)
        {
          NetDeviceContainer dev = accessLink.Install(router2, quicStaNodes.Get(i));
          tch.Install(dev);
          address.SetBase(("10.0." + std::to_string(80 + i) + ".0").c_str(), "255.255.255.0");
          address.Assign(dev);
        }
    }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  // =======================================================================
  // APPLICATIONS
  // =======================================================================
  Ipv4Address serverAddr = serverIfaces.GetAddress(0);

  // QUIC Server
  uint16_t quicServerPort = 8000;
  if (enableQuic && numQuicClients > 0)
    {
      QuicServerHelper quicServer(quicServerPort);
      ApplicationContainer quicServerApp = quicServer.Install(serverNode.Get(0));
      quicServerApp.Start(Seconds(warmupTime * 0.5));
      quicServerApp.Stop(Seconds(stopTime + 1));
    }

  // TCP sinks and connection scheduling
  uint16_t tcpPortBase = 5000;
  for (uint32_t i = 0; i < numTcpClients; i++)
    {
      uint16_t port = static_cast<uint16_t>(tcpPortBase + i);
      PacketSinkHelper tcpSink("ns3::TcpSocketFactory",
                               InetSocketAddress(Ipv4Address::GetAny(), port));
      ApplicationContainer sinkApp = tcpSink.Install(serverNode.Get(0));
      sinkApp.Start(Seconds(warmupTime * 0.5));
      sinkApp.Stop(Seconds(stopTime + 1));

      Simulator::Schedule(Seconds(warmupTime + i * 0.1),
                          &ScheduleTcpConnection,
                          tcpStaNodes.Get(i), serverAddr, port,
                          connPerClient,
                          Seconds(connectionInterval),
                          Seconds(connectionTimeout));
    }

  // QUIC clients
  if (enableQuic && numQuicClients > 0)
    {
      for (uint32_t i = 0; i < numQuicClients; i++)
        {
          QuicClientHelper quicClient(serverAddr, quicServerPort);
          quicClient.SetAttribute("Interval", TimeValue(MicroSeconds(10000)));
          quicClient.SetAttribute("PacketSize", UintegerValue(payloadSize));
          quicClient.SetAttribute("MaxPackets", UintegerValue(10000000));
          ApplicationContainer app = quicClient.Install(quicStaNodes.Get(i));
          app.Start(Seconds(warmupTime));
          app.Stop(Seconds(stopTime));
        }
      Simulator::Schedule(Seconds(warmupTime + 0.01),
                          &HookQuicTraces, serverNode.Get(0), quicStaNodes);
    }

  // Background
  if (enableBackground && numBgNodes > 0)
    {
      for (uint32_t i = 0; i < bgNodes.GetN(); i++)
        {
          OnOffHelper bgClient("ns3::UdpSocketFactory",
                               InetSocketAddress(serverAddr, 9999));
          bgClient.SetAttribute("PacketSize", UintegerValue(512));
          bgClient.SetAttribute("DataRate", DataRateValue(DataRate(bgRate)));
          bgClient.SetAttribute("OnTime",
                                StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));
          bgClient.SetAttribute("OffTime",
                                StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));
          ApplicationContainer app = bgClient.Install(bgNodes.Get(i));
          app.Start(Seconds(warmupTime + 0.2));
          app.Stop(Seconds(stopTime));
        }
    }

  // Timeout checker
  Simulator::Schedule(Seconds(connectionTimeout + 1.0),
                      &CheckTimeouts, connectionTimeout);

  // FlowMonitor
  FlowMonitorHelper flowmonHelper;
  Ptr<FlowMonitor> monitor;
  if (flowMonitor)
    monitor = flowmonHelper.InstallAll();

  // =======================================================================
  // DISPLAY CONFIG
  // =======================================================================
  std::cout << "==============================================" << std::endl;
  std::cout << "  Simulação de Taxa de Sucesso de Conexão" << std::endl;
  std::cout << "==============================================" << std::endl;
  std::cout << "Diretório:          " << outputDir << std::endl;
  std::cout << "Variante TCP:       " << tcpVariant << std::endl;
  std::cout << "Clientes TCP:       " << numTcpClients
            << " × " << connPerClient << " conexões = " << g_tcpTotal << std::endl;
  if (enableQuic)
    std::cout << "Clientes QUIC:      " << numQuicClients << std::endl;
  std::cout << "Intervalo conexões: " << connectionInterval << " s" << std::endl;
  std::cout << "Timeout handshake:  " << connectionTimeout << " s" << std::endl;
  std::cout << "---" << std::endl;
  std::cout << "Gargalo (bw/delay/erro): " << bottleneckBandwidth
            << " / " << bottleneckDelay
            << " / " << bottleneckErrorRate << std::endl;
  std::cout << "Link servidor:      " << serverBandwidth
            << " / " << serverDelay << std::endl;
  std::cout << "Link acesso:        " << accessBandwidth
            << " / " << accessDelay << std::endl;
  std::cout << "WiFi:               " << (enableWifi ? "ativado" : "desativado")
            << " (" << wifiStandardStr << " @" << wifiRate << ")" << std::endl;
  std::cout << "Fila:               " << queueDisc << std::endl;
  std::cout << "Tráfego de fundo:   " << (enableBackground ? "ativado" : "desativado")
            << " ×" << numBgNodes << " @" << bgRate << std::endl;
  std::cout << "Duração:            " << simulationTime
            << " s  (execução " << run << ")" << std::endl;
  std::cout << "==============================================" << std::endl;

  // =======================================================================
  // RUN
  // =======================================================================
  Simulator::Stop(Seconds(stopTime + 1));
  Simulator::Run();

  // =======================================================================
  // COMPUTE RESULTS
  // =======================================================================

  // --- TCP ---
  uint32_t tcpSuccess = 0;
  double tcpSumLat = 0, tcpMinLat = 1e9, tcpMaxLat = 0;
  uint32_t tcpLatCount = 0;
  uint32_t tcpAttempts = g_tcpTotal;

  for (auto& r : g_tcpResults)
    {
      if (r.succeeded && r.latencyMs > 0)
        {
          tcpSuccess++;
          tcpSumLat += r.latencyMs;
          tcpLatCount++;
          if (r.latencyMs < tcpMinLat) tcpMinLat = r.latencyMs;
          if (r.latencyMs > tcpMaxLat) tcpMaxLat = r.latencyMs;
        }
    }
  for (auto& entry : g_pendingTcp)
    {
      if (entry.second.succeeded && entry.second.latencyMs > 0)
        {
          tcpSuccess++;
          tcpSumLat += entry.second.latencyMs;
          tcpLatCount++;
          if (entry.second.latencyMs < tcpMinLat) tcpMinLat = entry.second.latencyMs;
          if (entry.second.latencyMs > tcpMaxLat) tcpMaxLat = entry.second.latencyMs;
        }
    }

  double tcpAvgLat = tcpLatCount > 0 ? tcpSumLat / tcpLatCount : 0;
  if (tcpMinLat > 1e8) tcpMinLat = 0;

  std::cout << "\n============= RESULTADOS DE SUCESSO DE CONEXÃO =============" << std::endl;
  std::cout << "\n--- TCP ---" << std::endl;
  std::cout << "  Tentativas:       " << tcpAttempts << std::endl;
  std::cout << "  Sucessos:         " << tcpSuccess << std::endl;
  std::cout << "  Falhas:           " << (tcpAttempts - tcpSuccess) << std::endl;
  std::cout << "  Taxa de sucesso:  " << std::fixed << std::setprecision(1)
            << (tcpAttempts > 0 ? 100.0 * tcpSuccess / tcpAttempts : 0) << " %" << std::endl;
  std::cout << "  Latência média:   " << std::setprecision(2) << tcpAvgLat << " ms" << std::endl;
  std::cout << "  Latência min/máx: " << tcpMinLat << " / " << tcpMaxLat << " ms" << std::endl;

  std::map<std::string, uint32_t> failReasons;
  for (auto& r : g_tcpResults)
    if (!r.succeeded) failReasons[r.failureReason]++;
  for (auto& fr : failReasons)
    std::cout << "    [" << fr.first << "]: " << fr.second << std::endl;

  // --- QUIC ---
  uint32_t quicSuccess = 0, quic0RTT = 0;
  double quicSumLat = 0, quicMinLat = 1e9, quicMaxLat = 0;
  uint32_t quicLatCount = 0;
  uint32_t quicTotal = g_quicResults.size();

  for (auto& r : g_quicResults)
    {
      if (r.succeeded && r.latencyMs > 0)
        {
          quicSuccess++;
          if (r.is0RTT) quic0RTT++;
          quicSumLat += r.latencyMs;
          quicLatCount++;
          if (r.latencyMs < quicMinLat) quicMinLat = r.latencyMs;
          if (r.latencyMs > quicMaxLat) quicMaxLat = r.latencyMs;
        }
    }

  double quicAvgLat = quicLatCount > 0 ? quicSumLat / quicLatCount : 0;
  if (quicMinLat > 1e8) quicMinLat = 0;

  if (enableQuic && numQuicClients > 0)
    {
      std::cout << "\n--- QUIC ---" << std::endl;
      std::cout << "  Detectados:     " << quicTotal << std::endl;
      std::cout << "  Sucessos:       " << quicSuccess << std::endl;
      std::cout << "  Fluxos 0-RTT:   " << quic0RTT << std::endl;
      std::cout << "  Taxa de sucesso:" << std::fixed << std::setprecision(1)
                << (quicTotal > 0 ? 100.0 * quicSuccess / quicTotal : 0) << " %" << std::endl;
      std::cout << "  Latência média: " << std::setprecision(2) << quicAvgLat << " ms" << std::endl;
      std::cout << "  Latência mín/máx:" << quicMinLat << " / " << quicMaxLat << " ms" << std::endl;
    }

  std::cout << "=======================================================" << std::endl;

  // =======================================================================
  // FLOW MONITOR (per-flow CSV)
  // =======================================================================
  if (flowMonitor)
    {
      monitor->CheckForLostPackets();
      Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());
      auto stats = monitor->GetFlowStats();

      std::string flowsPath = outputDir + "/flows.csv";
      std::ofstream flowsFile(flowsPath);
      flowsFile << "FluxoID,Protocolo,Origem,Destino,PortaOrigem,PortaDestino,"
                << "BytesTx,BytesRx,PacotesTx,PacotesRx,"
                << "PacotesPerdidos,AtrasoSoma_s,AtrasoMedio_s,Vazao_Mbps" << std::endl;

      for (auto& entry : stats)
        {
          Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(entry.first);
          double dur = entry.second.timeLastTxPacket.GetSeconds() -
                       entry.second.timeFirstTxPacket.GetSeconds();
          double thr = (dur > 0) ? entry.second.rxBytes * 8.0 / (dur * 1e6) : 0;
          double meanDelay = (entry.second.rxPackets > 0) ?
            entry.second.delaySum.GetSeconds() / entry.second.rxPackets : 0;

          flowsFile << entry.first << ","
                    << (t.protocol == 6 ? "TCP" : (t.protocol == 17 ? "UDP" : "OTHER"))
                    << "," << t.sourceAddress << "," << t.destinationAddress
                    << "," << t.sourcePort << "," << t.destinationPort
                    << "," << entry.second.txBytes
                    << "," << entry.second.rxBytes
                    << "," << entry.second.txPackets
                    << "," << entry.second.rxPackets
                    << "," << entry.second.lostPackets
                    << "," << entry.second.delaySum.GetSeconds()
                    << "," << meanDelay
                    << "," << thr << std::endl;
        }
      flowsFile.close();

      std::string xmlPath = outputDir + "/flows.flowmonitor";
      flowmonHelper.SerializeToXmlFile(xmlPath, true, true);

      std::cout << "\nFluxos CSV:     " << flowsPath << std::endl;
      std::cout << "Fluxos XML:     " << xmlPath << std::endl;
    }

  // =======================================================================
  // WRITE SUMMARY AND CONNECTIONS CSVs
  // =======================================================================
  WriteConnectionsCsv(outputDir + "/connections.csv");
  WriteSummaryCsv(outputDir + "/summary.csv",
                  tcpAttempts, tcpSuccess,
                  tcpAvgLat, tcpMinLat, tcpMaxLat,
                  quicSuccess, quicTotal, quic0RTT,
                  quicAvgLat, quicMinLat, quicMaxLat,
                  tcpVariant,
                  numTcpClients, numQuicClients, connPerClient,
                  connectionInterval, connectionTimeout,
                  bottleneckBandwidth, bottleneckDelay, bottleneckErrorRate,
                  serverBandwidth, serverDelay,
                  accessBandwidth, accessDelay,
                  enableWifi, wifiStandardStr, wifiRate,
                  enableBackground, numBgNodes, bgRate,
                  queueDisc, simulationTime, run,
                  enableQuic);

  std::cout << "Resumo CSV:      " << outputDir << "/summary.csv" << std::endl;
  std::cout << "Conexões CSV:    " << outputDir << "/connections.csv" << std::endl;

  Simulator::Destroy();
  return 0;
}
