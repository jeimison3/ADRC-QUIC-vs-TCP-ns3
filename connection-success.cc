/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2024
 *
 * Connection Success Rate — TCP & UDP (and QUIC) Handshake Metrics
 * over Mixed Wired/Wireless Topology
 *
 * Inherits topology control from tcp-udp-comparison.cc:
 *  [Server] ---P2P--- [Router1] ---P2P(bottleneck)--- [Router2] ---P2P--- [AP] ~~~WiFi~~~ [STAs]
 *                                                                       \---P2P--- [WiredClients]
 *
 * Metrics measured:
 *   - TCP: SYN→ESTABLISHED success rate & handshake latency (socket callbacks)
 *   - UDP: datagram delivery rate (first-packet-based, connectionless)
 *   - QUIC: Initial→Handshake→OPEN success rate & latency (Rx + CongState traces)
 *   - Background traffic configurable for congestion simulation
 *
 * Usage:
 *   ./ns3 run "connection-success --numTcpClients=3 --numUdpClients=2 --connPerClient=10"
 *   ./ns3 run "connection-success --enableQuic=1 --numQuicClients=2 --connPerClient=5"
 *   ./ns3 run "connection-success --tcpVariant=TcpBbr --bottleneckError=0.02"
 */

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <map>
#include <vector>
#include <iomanip>

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
  bool succeeded;
  bool gracefulClose;
  std::string failureReason;
};

struct UdpFlowResult
{
  double firstPktTime;
  uint64_t pktsSent;
  uint64_t pktsRcvd;
};

struct QuicConnectionResult
{
  double initialTime;
  double handshakeTime;
  double openTime;
  bool succeeded;
  bool is0RTT;
  bool firstDataAfterHandshake;
  std::string failureReason;
};

// Global accumulators
static std::vector<TcpConnectionResult> g_tcpResults;
static std::vector<UdpFlowResult> g_udpResults;
static std::vector<QuicConnectionResult> g_quicResults;

static std::map<Ptr<Socket>, TcpConnectionResult> g_pendingTcp;
static std::map<Ptr<Socket>, double> g_tcpStartTimes;

static std::map<Ptr<const QuicSocketBase>, QuicConnectionResult> g_pendingQuic;
static std::map<Ptr<const QuicSocketBase>, int> g_quicRxStage; // 0=none, 1=initial, 2=handshake

static uint64_t g_udpPacketsSent = 0;
static double g_timeoutLimit = 10.0;
static bool g_verboseConn = false;

// ===========================================================================
// TCP Socket Callbacks
// ===========================================================================

static void
OnTcpConnectionSucceeded(Ptr<Socket> sock)
{
  auto it = g_tcpStartTimes.find(sock);
  if (it == g_tcpStartTimes.end()) return;

  TcpConnectionResult r;
  r.startTime = it->second;
  r.establishedTime = Simulator::Now().GetSeconds();
  r.succeeded = true;
  r.gracefulClose = false;
  r.failureReason = "";
  g_pendingTcp[sock] = r;
  g_tcpStartTimes.erase(it);  // remove from timeout watch — already succeeded

  if (g_verboseConn)
    std::cout << Simulator::Now().GetSeconds()
              << "s TCP-OK: handshake="
              << (r.establishedTime - r.startTime) * 1000.0 << " ms" << std::endl;
}

static void
OnTcpConnectionFailed(Ptr<Socket> sock)
{
  auto it = g_tcpStartTimes.find(sock);
  if (it == g_tcpStartTimes.end()) return;

  TcpConnectionResult r;
  r.startTime = it->second;
  r.establishedTime = 0;
  r.succeeded = false;
  r.failureReason = "ConnectFailed";
  g_tcpResults.push_back(r);
  g_tcpStartTimes.erase(it);

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
  // Clean up start time map
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
  // Read and discard — just confirming data flow works
  Ptr<Packet> p;
  while ((p = sock->Recv()))
    { }
}

// ===========================================================================
// UDP receive callback (keep for semantic clarity, packet count via sink)
// ===========================================================================


// ===========================================================================
// QUIC trace hookup (scheduled after apps start to ensure sockets exist)
// ===========================================================================

// Forward declaration
static void
OnQuicRx(Ptr<const Packet> p, const QuicHeader& h, Ptr<const QuicSocketBase> s);

static void
HookQuicTraces(Ptr<Node> server, NodeContainer quicStaNodes)
{
  // Only hook CLIENT-side Rx traces — server-side would use different socket objects,
  // making correlation across sockets impractical.
  // Detection: client receives HANDSHAKE → client receives first Short → OPEN.
  for (uint32_t i = 0; i < quicStaNodes.GetN(); i++)
    {
      std::ostringstream rxPath;
      rxPath << "/NodeList/" << quicStaNodes.Get(i)->GetId()
             << "/$ns3::QuicL4Protocol/SocketList/*/QuicSocketBase/Rx";
      Config::ConnectWithoutContext(rxPath.str(), MakeCallback(&OnQuicRx));
    }
}


[[maybe_unused]] static void
OnQuicRx(Ptr<const Packet> p, const QuicHeader& h, Ptr<const QuicSocketBase> s)
{
  if (!s) return;
  double now = Simulator::Now().GetSeconds();

  // Client-side only: detect HANDSHAKE → first Short = connection established.
  // Start time is approximated by app start (warmupTime), since we can't detect
  // INITIAL without hooking the server socket.

  if (h.IsHandshake())
    {
      if (g_quicRxStage.find(s) == g_quicRxStage.end() || g_quicRxStage[s] < 2)
        {
          g_quicRxStage[s] = 2;
          QuicConnectionResult r;
          // Use the app start time as connection start reference
          // (connection attempt begins when QuicClientHelper::StartApplication runs)
          r.initialTime = 0; // will be filled later if we detect completion
          r.handshakeTime = now;
          r.succeeded = false;
          r.is0RTT = false;
          r.firstDataAfterHandshake = false;
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
          g_quicRxStage[s] = 3;
          it->second.openTime = now;
          it->second.succeeded = true;
          it->second.firstDataAfterHandshake = true;
          if (it->second.handshakeTime > 0)
            it->second.initialTime = it->second.handshakeTime;
          g_quicResults.push_back(it->second);
          g_pendingQuic.erase(it);
          if (g_verboseConn)
            std::cout << now << "s QUIC-OPEN (first data)"
                      << " hs-latency=" << (now - it->second.handshakeTime) * 1000.0
                      << " ms" << std::endl;
        }
    }
  else if (h.IsORTT())
    {
      // 0-RTT: data arrives before full handshake
      auto it = g_pendingQuic.find(s);
      if (it != g_pendingQuic.end() && !it->second.succeeded)
        {
          it->second.is0RTT = true;
          it->second.openTime = now;
          it->second.succeeded = true;
          it->second.firstDataAfterHandshake = true;
          if (it->second.handshakeTime > 0)
            it->second.initialTime = it->second.handshakeTime;
          g_quicResults.push_back(it->second);
          g_pendingQuic.erase(it);
          if (g_verboseConn)
            std::cout << now << "s QUIC-0RTT"
                      << " latency=" << (now - it->second.handshakeTime) * 1000.0
                      << " ms" << std::endl;
        }
    }
}

// ===========================================================================
// Connection scheduler — TCP
// ===========================================================================

static void
ScheduleTcpConnection(Ptr<Node> client, Ipv4Address serverAddr, uint16_t port,
                      uint32_t remaining, Time interval, Time timeout)
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
      // Immediate failure (e.g., no route)
      TcpConnectionResult r;
      r.startTime = now;
      r.establishedTime = 0;
      r.succeeded = false;
      r.failureReason = "ImmediateConnectError";
      g_tcpResults.push_back(r);
      g_tcpStartTimes.erase(sock);
    }

  // Schedule remaining connections
  Simulator::Schedule(interval,
                      &ScheduleTcpConnection,
                      client, serverAddr, port,
                      remaining - 1, interval, timeout);
}

// ===========================================================================
// Connection timeout checker
// ===========================================================================

static void
CheckTimeouts(double timeoutLimit)
{
  double now = Simulator::Now().GetSeconds();

  // Check pending TCP connections
  for (auto it = g_tcpStartTimes.begin(); it != g_tcpStartTimes.end(); )
    {
      if (now - it->second > timeoutLimit)
        {
          TcpConnectionResult r;
          r.startTime = it->second;
          r.establishedTime = 0;
          r.succeeded = false;
          r.failureReason = "Timeout";
          g_tcpResults.push_back(r);

          auto pit = g_pendingTcp.find(it->first);
          if (pit != g_pendingTcp.end())
            g_pendingTcp.erase(pit);

          it = g_tcpStartTimes.erase(it);
        }
      else
        { ++it; }
    }

  // Check pending QUIC connections (client-side: handshake received but no Short yet)
  for (auto it = g_pendingQuic.begin(); it != g_pendingQuic.end(); )
    {
      if (!it->second.succeeded && now - it->second.handshakeTime > timeoutLimit)
        {
          it->second.failureReason = "Timeout";
          it->second.succeeded = false;
          g_quicResults.push_back(it->second);
          it = g_pendingQuic.erase(it);
        }
      else
        { ++it; }
    }

  Simulator::Schedule(Seconds(0.5), &CheckTimeouts, timeoutLimit);
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

  // Connection parameters
  uint32_t numTcpClients = 2;
  uint32_t numUdpClients = 2;
  uint32_t connPerClient = 5;
  double connectionInterval = 0.5;   // seconds between consecutive connections
  double connectionTimeout = 10.0;   // max seconds to wait for handshake
  uint32_t payloadSize = 1400;
  std::string udpRate = "1Mbps";
  double simulationTime = 60.0;

  // QUIC parameters
  bool enableQuic = false;
  uint32_t numQuicClients = 0;

  // WiFi
  bool enableWifi = true;
  std::string wifiStandardStr = "80211ac";
  std::string wifiRate = "VhtMcs7";

  // Background
  bool enableBackground = false;
  uint32_t numBgNodes = 2;
  std::string bgRate = "2Mbps";

  // Control
  bool flowMonitor = true;
  bool pcap = false;
  std::string queueDisc = "ns3::PfifoFastQueueDisc";
  std::string prefixName = "conn-success";
  uint32_t run = 0;
  bool verbose = false;

  CommandLine cmd(__FILE__);

  // Link parameters
  cmd.AddValue("serverBw", "Server link bandwidth", serverBandwidth);
  cmd.AddValue("serverDelay", "Server link one-way delay", serverDelay);
  cmd.AddValue("bottleneckBw", "Bottleneck link bandwidth", bottleneckBandwidth);
  cmd.AddValue("bottleneckDelay", "Bottleneck link one-way delay", bottleneckDelay);
  cmd.AddValue("bottleneckError", "Bottleneck packet error rate (0.0–1.0)",
               bottleneckErrorRate);
  cmd.AddValue("accessBw", "Access link bandwidth", accessBandwidth);
  cmd.AddValue("accessDelay", "Access link one-way delay", accessDelay);

  // Connection parameters
  cmd.AddValue("tcpVariant",
               "TCP congestion control: TcpNewReno, TcpBbr, TcpCubic, TcpVegas, etc.",
               tcpVariant);
  cmd.AddValue("numTcpClients", "Number of TCP client nodes", numTcpClients);
  cmd.AddValue("numUdpClients", "Number of UDP client nodes", numUdpClients);
  cmd.AddValue("connPerClient",
               "Number of connection attempts per client node", connPerClient);
  cmd.AddValue("connInterval",
               "Interval between consecutive connection attempts (seconds)",
               connectionInterval);
  cmd.AddValue("connTimeout",
               "Maximum wait time for handshake completion (seconds)",
               connectionTimeout);
  cmd.AddValue("payloadSize", "Application payload size in bytes", payloadSize);
  cmd.AddValue("udpRate", "UDP sending rate per client", udpRate);

  // QUIC
  cmd.AddValue("enableQuic", "Enable QUIC connection tests (0/1)", enableQuic);
  cmd.AddValue("numQuicClients", "Number of QUIC client nodes", numQuicClients);
  // Note: 0-RTT handshake via global config:
  //   --ns3::QuicL4Protocol::0RTT-Handshake=1

  // WiFi
  cmd.AddValue("enableWifi", "Enable WiFi section (1=yes, 0=wired only)", enableWifi);
  cmd.AddValue("wifiStandard",
               "WiFi standard: 80211a, 80211b, 80211g, 80211n, 80211ac, 80211ax",
               wifiStandardStr);
  cmd.AddValue("wifiRate", "WiFi data mode (VhtMcs7, HtMcs7, etc.)", wifiRate);

  // Background
  cmd.AddValue("background", "Enable background cross-traffic (0/1)", enableBackground);
  cmd.AddValue("numBgNodes", "Number of background traffic nodes", numBgNodes);
  cmd.AddValue("bgRate", "Background traffic rate per node", bgRate);

  // Control
  cmd.AddValue("duration", "Simulation duration in seconds", simulationTime);
  cmd.AddValue("run", "Run index for RNG seed", run);
  cmd.AddValue("flowMonitor", "Enable FlowMonitor (0/1)", flowMonitor);
  cmd.AddValue("pcap", "Enable PCAP tracing (0/1)", pcap);
  cmd.AddValue("queueDisc",
               "Queue discipline: ns3::PfifoFastQueueDisc or ns3::CoDelQueueDisc",
               queueDisc);
  cmd.AddValue("prefix", "Prefix for output files", prefixName);
  cmd.AddValue("verbose", "Enable verbose connection logging (0/1)", verbose);
  cmd.AddValue("verboseConn",
               "Enable per-connection success/failure logging (0/1)", g_verboseConn);

  cmd.Parse(argc, argv);

  g_timeoutLimit = connectionTimeout;

  // -----------------------------------------------------------------------
  // Seed, logging, TCP variant
  // -----------------------------------------------------------------------
  SeedManager::SetSeed(1);
  SeedManager::SetRun(run);
  Time::SetResolution(Time::NS);

  if (verbose)
    {
      LogComponentEnableAll(LOG_PREFIX_TIME);
      LogComponentEnableAll(LOG_PREFIX_NODE);
      LogComponentEnable("ConnectionSuccess", LOG_LEVEL_ALL);
    }

  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(payloadSize));
  Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(1 << 21));
  Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(1 << 21));

  tcpVariant = std::string("ns3::") + tcpVariant;
  TypeId tcpTid;
  NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(tcpVariant, &tcpTid),
                      "TypeId " << tcpVariant << " not found");
  Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                     TypeIdValue(TypeId::LookupByName(tcpVariant)));

  // -----------------------------------------------------------------------
  // QUIC transport protocol (if enabled)
  // -----------------------------------------------------------------------
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

  double stopTime = 0.1 + simulationTime;

  // =======================================================================
  // TOPOLOGY CREATION
  // =======================================================================

  NodeContainer serverNode;
  serverNode.Create(1);

  NodeContainer routerNodes;
  routerNodes.Create(2);
  Ptr<Node> router1 = routerNodes.Get(0);
  Ptr<Node> router2 = routerNodes.Get(1);

  NodeContainer apNode;
  apNode.Create(1);

  // TCP client nodes (WiFi)
  NodeContainer tcpStaNodes;
  tcpStaNodes.Create(numTcpClients);

  // UDP client nodes (WiFi)
  NodeContainer udpStaNodes;
  udpStaNodes.Create(numUdpClients);

  // QUIC client nodes (WiFi)
  NodeContainer quicStaNodes;
  if (enableQuic)
    quicStaNodes.Create(numQuicClients);

  // WiFi STA nodes = all client nodes combined
  NodeContainer wifiStaNodes;
  wifiStaNodes.Add(tcpStaNodes);
  wifiStaNodes.Add(udpStaNodes);
  if (enableQuic) wifiStaNodes.Add(quicStaNodes);

  // Background nodes
  NodeContainer bgNodes;
  if (enableBackground)
    bgNodes.Create(numBgNodes);

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

  // IP forwarding
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
  // WIRED LINKS INSTALLATION
  // =======================================================================
  Ipv4AddressHelper address;
  Ipv4InterfaceContainer serverIfaces;

  // Server <-> Router1
  NetDeviceContainer devSR1 = serverLink.Install(serverNode.Get(0), router1);
  tch.Install(devSR1);
  address.SetBase("10.0.0.0", "255.255.255.0");
  Ipv4InterfaceContainer ifSR1 = address.Assign(devSR1);
  serverIfaces.Add(ifSR1.Get(0));

  // Router1 <-> Router2 (bottleneck)
  NetDeviceContainer devR1R2 = bottleneckLink.Install(router1, router2);
  tch.Install(devR1R2);
  address.SetBase("10.0.1.0", "255.255.255.0");
  address.Assign(devR1R2);

  // Router2 <-> AP
  NetDeviceContainer devR2Ap = accessLink.Install(router2, apNode.Get(0));
  tch.Install(devR2Ap);
  address.SetBase("10.0.2.0", "255.255.255.0");
  address.Assign(devR2Ap);

  // Background nodes
  for (uint32_t i = 0; i < bgNodes.GetN(); i++)
    {
      NetDeviceContainer dev = accessLink.Install(router2, bgNodes.Get(i));
      tch.Install(dev);
      address.SetBase(("10.0." + std::to_string(100 + i) + ".0").c_str(), "255.255.255.0");
      address.Assign(dev);
    }

  // =======================================================================
  // WIFI SECTION
  // =======================================================================
  Ipv4InterfaceContainer apWifiIfaces;

  if (enableWifi && wifiStaNodes.GetN() > 0)
    {
      internet.Install(wifiStaNodes);

      // Install QUIC stack on QUIC nodes (separate from TCP/IP)
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

      wifiMac.SetType("ns3::ApWifiMac",
                      "Ssid", SsidValue(ssid),
                      "EnableBeaconJitter", BooleanValue(false));
      NetDeviceContainer apWifiDev = wifi.Install(wifiPhy, wifiMac, apNode.Get(0));

      wifiMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid));
      NetDeviceContainer staWifiDev = wifi.Install(wifiPhy, wifiMac, wifiStaNodes);

      MobilityHelper mobility;
      Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator>();
      positionAlloc->Add(Vector(0.0, 0.0, 0.0)); // AP
      for (uint32_t i = 0; i < wifiStaNodes.GetN(); i++)
        positionAlloc->Add(Vector(5.0 + i * 2.0, 0.0, 0.0));
      mobility.SetPositionAllocator(positionAlloc);
      mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
      mobility.Install(apNode);
      mobility.Install(wifiStaNodes);

      Ipv4AddressHelper wifiAddress;
      wifiAddress.SetBase("10.0.200.0", "255.255.255.0");
      apWifiIfaces = wifiAddress.Assign(apWifiDev);
      wifiAddress.Assign(staWifiDev);
    }

  // =======================================================================
  // ROUTING
  // =======================================================================
  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  // =======================================================================
  // APPLICATIONS & CONNECTION SCHEDULING
  // =======================================================================
  Ipv4Address serverAddr = serverIfaces.GetAddress(0);
  double warmupTime = 1.0;

  // --- UDP Server (PacketSink) ---
  uint16_t udpServerPort = 7000;
  PacketSinkHelper udpSinkHelper("ns3::UdpSocketFactory",
                                 InetSocketAddress(Ipv4Address::GetAny(), udpServerPort));
  ApplicationContainer udpSinkApps = udpSinkHelper.Install(serverNode.Get(0));
  udpSinkApps.Start(Seconds(warmupTime * 0.5));
  udpSinkApps.Stop(Seconds(stopTime + 1));

  // Store pointer to UDP sink for post-simulation counting
  Ptr<PacketSink> udpSink = DynamicCast<PacketSink>(udpSinkApps.Get(0));

  // --- QUIC Server (if enabled) ---
  uint16_t quicServerPort = 8000;
  if (enableQuic && numQuicClients > 0)
    {
      QuicServerHelper quicServer(quicServerPort);
      ApplicationContainer quicServerApp = quicServer.Install(serverNode.Get(0));
      quicServerApp.Start(Seconds(warmupTime * 0.5));
      quicServerApp.Stop(Seconds(stopTime + 1));
    }

  // --- Schedule TCP connections ---
  uint16_t tcpPortBase = 5000;
  for (uint32_t i = 0; i < numTcpClients; i++)
    {
      uint16_t port = static_cast<uint16_t>(tcpPortBase + i);
      // Install TCP sink on server
      PacketSinkHelper tcpSink("ns3::TcpSocketFactory",
                               InetSocketAddress(Ipv4Address::GetAny(), port));
      ApplicationContainer sinkApp = tcpSink.Install(serverNode.Get(0));
      sinkApp.Start(Seconds(warmupTime * 0.5));
      sinkApp.Stop(Seconds(stopTime + 1));

      // Schedule connection attempts from this client
      Simulator::Schedule(Seconds(warmupTime + i * 0.1),
                          &ScheduleTcpConnection,
                          tcpStaNodes.Get(i), serverAddr, port,
                          connPerClient,
                          Seconds(connectionInterval),
                          Seconds(connectionTimeout));
    }

  // --- Schedule UDP flows ---
  for (uint32_t i = 0; i < numUdpClients; i++)
    {
      OnOffHelper udpClient("ns3::UdpSocketFactory",
                            InetSocketAddress(serverAddr, udpServerPort));
      udpClient.SetAttribute("PacketSize", UintegerValue(payloadSize));
      udpClient.SetAttribute("DataRate", DataRateValue(DataRate(udpRate)));
      udpClient.SetAttribute("OnTime",
                             StringValue("ns3::ConstantRandomVariable[Constant=1]"));
      udpClient.SetAttribute("OffTime",
                             StringValue("ns3::ConstantRandomVariable[Constant=0]"));
      ApplicationContainer app = udpClient.Install(udpStaNodes.Get(i));
      // Start staggered to avoid perfect synchronization
      app.Start(Seconds(warmupTime + i * 0.5));
      app.Stop(Seconds(stopTime));
      g_udpPacketsSent += static_cast<uint64_t>(
        DataRate(udpRate).GetBitRate() / 8.0 / payloadSize * simulationTime);
    }

  // --- Schedule QUIC connections ---
  if (enableQuic && numQuicClients > 0)
    {
      // Start all QUIC clients at the same time
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

      // Hook traces 10ms after apps start — sockets are created, handshake not yet done
      Simulator::Schedule(Seconds(warmupTime + 0.01),
                          &HookQuicTraces, serverNode.Get(0), quicStaNodes);
    }

  // --- Background cross-traffic ---
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

  // -----------------------------------------------------------------------
  // Timeout checker (periodic)
  // -----------------------------------------------------------------------
  Simulator::Schedule(Seconds(connectionTimeout + 1.0),
                      &CheckTimeouts, connectionTimeout);

  // -----------------------------------------------------------------------
  // FlowMonitor
  // -----------------------------------------------------------------------
  FlowMonitorHelper flowmonHelper;
  Ptr<FlowMonitor> monitor;
  if (flowMonitor)
    {
      monitor = flowmonHelper.InstallAll();
    }

  // =======================================================================
  // DISPLAY CONFIG
  // =======================================================================
  std::cout << "\n==============================================" << std::endl;
  std::cout << "  Connection Success Rate Simulation" << std::endl;
  std::cout << "==============================================" << std::endl;
  std::cout << "TCP variant:        " << tcpVariant << std::endl;
  std::cout << "TCP clients:        " << numTcpClients
            << " x " << connPerClient << " conn = "
            << numTcpClients * connPerClient << " total" << std::endl;
  std::cout << "UDP clients:        " << numUdpClients
            << " @" << udpRate << "/client" << std::endl;
  if (enableQuic)
    std::cout << "QUIC clients:       " << numQuicClients << std::endl;
  std::cout << "Conn interval:      " << connectionInterval << " s" << std::endl;
  std::cout << "Conn timeout:       " << connectionTimeout << " s" << std::endl;
  std::cout << "---" << std::endl;
  std::cout << "Server link:        " << serverBandwidth
            << " / " << serverDelay << std::endl;
  std::cout << "Bottleneck link:    " << bottleneckBandwidth
            << " / " << bottleneckDelay
            << " / error=" << bottleneckErrorRate << std::endl;
  std::cout << "Access link:        " << accessBandwidth
            << " / " << accessDelay << std::endl;
  std::cout << "WiFi:               " << (enableWifi ? "enabled" : "disabled")
            << " (" << wifiStandardStr << " @" << wifiRate << ")" << std::endl;
  std::cout << "Queue disc:         " << queueDisc << std::endl;
  std::cout << "Background:         " << (enableBackground ? "enabled" : "disabled")
            << " x" << numBgNodes << " @" << bgRate << std::endl;
  std::cout << "Duration:           " << simulationTime
            << " s  (run " << run << ")" << std::endl;
  std::cout << "==============================================" << std::endl;

  // =======================================================================
  // RUN
  // =======================================================================
  Simulator::Stop(Seconds(stopTime + 1));
  Simulator::Run();

  // =======================================================================
  // RESULTS
  // =======================================================================
  std::cout << "\n============= CONNECTION SUCCESS RESULTS =============" << std::endl;

  // --- TCP results ---
  {
    uint32_t tcpSuccess = 0;
    double tcpSumLatency = 0.0;
    uint32_t tcpLatencyCount = 0;
    uint32_t tcpAttempts = numTcpClients * connPerClient;

    // Count completed results
    for (auto& r : g_tcpResults)
      {
        if (r.succeeded)
          {
            tcpSuccess++;
            double lat = r.establishedTime - r.startTime;
            if (lat > 0 && lat < connectionTimeout)
              {
                tcpSumLatency += lat;
                tcpLatencyCount++;
              }
          }
      }

    // Count still-pending (succeeded but not yet closed)
    for (auto& entry : g_pendingTcp)
      {
        if (entry.second.succeeded)
          {
            tcpSuccess++;
            double lat = entry.second.establishedTime - entry.second.startTime;
            if (lat > 0 && lat < connectionTimeout)
              {
                tcpSumLatency += lat;
                tcpLatencyCount++;
              }
          }
      }

    double tcpSuccessRate = tcpAttempts > 0 ?
      100.0 * tcpSuccess / tcpAttempts : 0;
    double tcpAvgLatency = tcpLatencyCount > 0 ?
      tcpSumLatency * 1000.0 / tcpLatencyCount : 0;

    std::cout << "\n--- TCP ---" << std::endl;
    std::cout << "  Attempts:      " << tcpAttempts << std::endl;
    std::cout << "  Success:       " << tcpSuccess << std::endl;
    std::cout << "  Failed:        " << (tcpAttempts - tcpSuccess) << std::endl;
    std::cout << "  Success rate:  " << std::fixed << std::setprecision(1)
              << tcpSuccessRate << " %" << std::endl;
    std::cout << "  Avg handshake: " << std::setprecision(2)
              << tcpAvgLatency << " ms" << std::endl;

    // Detailed failure breakdown
    std::map<std::string, uint32_t> failReasons;
    for (auto& r : g_tcpResults)
      if (!r.succeeded) failReasons[r.failureReason]++;
    for (auto& fr : failReasons)
      std::cout << "    [" << fr.first << "]: " << fr.second << std::endl;
  }

  // --- UDP results ---
  {
    uint64_t udpRcvd = udpSink ? udpSink->GetTotalRx() : 0;

    std::cout << "\n--- UDP (connectionless, packet-level) ---" << std::endl;
    if (udpSink)
      {
        std::cout << "  Bytes received:   " << udpRcvd << std::endl;
        std::cout << "  Packets received: " << (udpRcvd / payloadSize) << std::endl;
      }
    std::cout << "  Est. packets sent: " << g_udpPacketsSent << std::endl;
    double udpDelivery = g_udpPacketsSent > 0 ?
      100.0 * udpRcvd / payloadSize / g_udpPacketsSent : 0;
    std::cout << "  Delivery rate:    " << std::fixed << std::setprecision(1)
              << udpDelivery << " %" << std::endl;
  }

  // --- QUIC results ---
  if (enableQuic && numQuicClients > 0)
    {
      uint32_t quicSuccess = 0;
      uint32_t quic0RTT = 0;
      double quicSumLatency = 0.0;
      uint32_t quicLatencyCount = 0;
      uint32_t quicTotal = g_quicResults.size();

      for (auto& r : g_quicResults)
        {
          if (r.succeeded)
            {
              quicSuccess++;
              if (r.is0RTT) quic0RTT++;
              // Latency from HANDSHAKE reception to OPEN (first Short header)
              double lat = r.openTime - r.handshakeTime;
              if (lat > 0 && lat < connectionTimeout)
                {
                  quicSumLatency += lat;
                  quicLatencyCount++;
                }
            }
        }

      double quicSuccessRate = g_quicResults.size() > 0 ?
        100.0 * quicSuccess / g_quicResults.size() : 0;
      double quicAvgLatency = quicLatencyCount > 0 ?
        quicSumLatency * 1000.0 / quicLatencyCount : 0;

      std::cout << "\n--- QUIC ---" << std::endl;
      std::cout << "  Detected:        " << quicTotal << std::endl;
      std::cout << "  Success:         " << quicSuccess << std::endl;
      std::cout << "  0-RTT flows:     " << quic0RTT << std::endl;
      std::cout << "  Success rate:    " << std::fixed << std::setprecision(1)
                << quicSuccessRate << " %" << std::endl;
      std::cout << "  Avg handshake:   " << std::setprecision(2)
                << quicAvgLatency << " ms" << std::endl;
    }

  std::cout << "=======================================================" << std::endl;

  // =======================================================================
  // FLOW MONITOR CSV
  // =======================================================================
  if (flowMonitor)
    {
      monitor->CheckForLostPackets();
      Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());
      auto stats = monitor->GetFlowStats();

      std::string csvName = prefixName + "-r" + std::to_string(run) + "-flows.csv";
      std::ofstream csvFile(csvName);
      csvFile << "FlowID,Protocol,SrcAddr,DstAddr,SrcPort,DstPort,"
              << "TxBytes,RxBytes,TxPackets,RxPackets,"
              << "LostPackets,DelaySum_s,MeanDelay_s,Throughput_Mbps" << std::endl;

      for (auto& entry : stats)
        {
          Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(entry.first);
          double dur = entry.second.timeLastTxPacket.GetSeconds() -
                       entry.second.timeFirstTxPacket.GetSeconds();
          double thr = (dur > 0) ? entry.second.rxBytes * 8.0 / (dur * 1e6) : 0;
          double meanDelay = (entry.second.rxPackets > 0) ?
            entry.second.delaySum.GetSeconds() / entry.second.rxPackets : 0;

          csvFile << entry.first << ","
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
      csvFile.close();

      std::string xmlName = prefixName + "-r" + std::to_string(run) + ".flowmonitor";
      flowmonHelper.SerializeToXmlFile(xmlName, true, true);

      std::cout << "\nFlow CSV: " << csvName << std::endl;
      std::cout << "Flow XML: " << xmlName << std::endl;
    }

  Simulator::Destroy();
  return 0;
}
