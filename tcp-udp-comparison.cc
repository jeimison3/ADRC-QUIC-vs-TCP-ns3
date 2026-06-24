/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2024
 *
 * TCP vs UDP Performance Comparison over Mixed Wired/Wireless Topology
 *
 * Network topology:
 *
 *  [Server]                                                      [WiFi_STAs]
 *     |                                                              |
 *     | serverLink (P2P)                                             | WiFi (802.11ac)
 *     |                                                              |
 *  [Router1]                                                     [AP_Node]
 *     |                                                              |
 *     | bottleneckLink (P2P, bottle_bw/delay/error)                  | accessRightLink (P2P)
 *     |                                                              |
 *  [Router2] ----accessLeftLink(P2P)---- [WiredClients]              |
 *     |                                                              |
 *     +------------------accessRightLink(P2P)------------------------+
 *
 *  - Server: PacketSink for TCP and UDP receivers
 *  - Router1, Router2: intermediate routers (IP forwarding enabled)
 *  - AP_Node: WiFi Access Point + P2P interface (IP forwarding enabled)
 *  - WiFi_STAs: stations sending TCP (BulkSend) / UDP (OnOff) traffic
 *  - WiredClients: comparison baseline on wired access
 *  - Background: configurable cross-traffic via dedicated nodes
 *
 * Usage examples:
 *   # Simple: 1 TCP STA + 1 UDP STA, 10 Mbps bottleneck, 30s
 *   ./ns3 run "tcp-udp-comparison --numTcpSta=1 --numUdpSta=1 --bottleneckBw=10Mbps --duration=30"
 *
 *   # Compare wired vs WiFi:
 *   ./ns3 run "tcp-udp-comparison --numTcpSta=1 --numWiredTcp=1 --numUdpSta=1 --numWiredUdp=1"
 *
 *   # With packet loss, background traffic, and congestion:
 *   ./ns3 run "tcp-udp-comparison --bottleneckError=0.01 --background=1 --numBgNodes=2"
 *
 *   # Wired-only comparison (disable WiFi):
 *   ./ns3 run "tcp-udp-comparison --enableWifi=0 --numWiredTcp=1 --numWiredUdp=1"
 *
 *   # Different TCP variants:
 *   ./ns3 run "tcp-udp-comparison --tcpVariant=TcpBbr --numTcpSta=2 --numUdpSta=2"
 */

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <map>
#include <vector>

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

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TcpUdpComparison");

// ---------------------------------------------------------------------------
// Global data for throughput calculation (PacketSink-based)
// ---------------------------------------------------------------------------
std::map<uint32_t, Ptr<PacketSink>> g_tcpSinks;
std::map<uint32_t, Ptr<PacketSink>> g_udpSinks;
std::map<uint32_t, uint64_t> g_lastRx;
uint32_t g_flowCounter = 0;

// ---------------------------------------------------------------------------
// Periodic throughput reporter
// ---------------------------------------------------------------------------
void
ReportThroughput(double intervalMs)
{
  Time now = Simulator::Now();
  for (auto& entry : g_tcpSinks)
    {
      uint64_t current = entry.second->GetTotalRx();
      double thr = static_cast<double>(current - g_lastRx[entry.first]) * 8.0 /
                   (intervalMs * 1e3);
      std::cout << now.GetSeconds() << "s TCP-Sink-" << entry.first
                << " thr=" << thr << " Mbit/s  rx=" << current << " B" << std::endl;
      g_lastRx[entry.first] = current;
    }
  for (auto& entry : g_udpSinks)
    {
      uint64_t current = entry.second->GetTotalRx();
      double thr = static_cast<double>(current - g_lastRx[entry.first]) * 8.0 /
                   (intervalMs * 1e3);
      std::cout << now.GetSeconds() << "s UDP-Sink-" << entry.first
                << " thr=" << thr << " Mbit/s  rx=" << current << " B" << std::endl;
      g_lastRx[entry.first] = current;
    }
  Simulator::Schedule(MilliSeconds(static_cast<uint64_t>(intervalMs)),
                      &ReportThroughput, intervalMs);
}

// ===========================================================================
int main(int argc, char* argv[])
{
  // -----------------------------------------------------------------------
  // Command-line parameters (all configurable)
  // -----------------------------------------------------------------------
  std::string tcpVariant = "TcpNewReno";
  std::string serverBandwidth = "100Mbps";
  std::string serverDelay = "5ms";
  std::string bottleneckBandwidth = "10Mbps";
  std::string bottleneckDelay = "20ms";
  double bottleneckErrorRate = 0.0;
  std::string accessBandwidth = "100Mbps";
  std::string accessDelay = "2ms";
  uint32_t numTcpSta = 1;
  uint32_t numUdpSta = 1;
  uint32_t numWiredTcp = 0;
  uint32_t numWiredUdp = 0;
  uint32_t payloadSize = 1400;
  std::string udpRate = "5Mbps";
  double simulationTime = 30.0;
  bool enableWifi = true;
  std::string wifiStandardStr = "80211ac";
  std::string wifiRate = "VhtMcs7";
  bool enableBackground = false;
  uint32_t numBgNodes = 2;
  std::string bgRate = "2Mbps";
  bool flowMonitor = true;
  bool pcap = false;
  bool periodicReport = true;
  std::string queueDisc = "ns3::PfifoFastQueueDisc";
  std::string prefixName = "tcp-udp-comp";
  uint32_t run = 0;
  bool verbose = false;

  CommandLine cmd(__FILE__);

  // TCP/UDP
  cmd.AddValue("tcpVariant",
               "TCP congestion control: TcpNewReno, TcpBbr, TcpCubic, TcpVegas, "
               "TcpWestwood, TcpLedbat, TcpHighSpeed, TcpBic, etc.",
               tcpVariant);
  cmd.AddValue("numTcpSta", "Number of WiFi TCP client stations", numTcpSta);
  cmd.AddValue("numUdpSta", "Number of WiFi UDP client stations", numUdpSta);
  cmd.AddValue("numWiredTcp", "Number of wired TCP clients", numWiredTcp);
  cmd.AddValue("numWiredUdp", "Number of wired UDP clients", numWiredUdp);
  cmd.AddValue("udpRate", "UDP sending rate per client (e.g. 5Mbps)", udpRate);
  cmd.AddValue("payloadSize", "Application payload size in bytes", payloadSize);

  // Links
  cmd.AddValue("serverBw", "Server link bandwidth", serverBandwidth);
  cmd.AddValue("serverDelay", "Server link one-way delay", serverDelay);
  cmd.AddValue("bottleneckBw", "Bottleneck link bandwidth", bottleneckBandwidth);
  cmd.AddValue("bottleneckDelay", "Bottleneck link one-way delay", bottleneckDelay);
  cmd.AddValue("bottleneckError", "Bottleneck packet error rate (0.0 to 1.0)",
               bottleneckErrorRate);
  cmd.AddValue("accessBw", "Access link bandwidth", accessBandwidth);
  cmd.AddValue("accessDelay", "Access link one-way delay", accessDelay);

  // WiFi
  cmd.AddValue("enableWifi", "Enable WiFi section (1=yes, 0=wired only)", enableWifi);
  cmd.AddValue("wifiStandard",
               "WiFi standard: 80211a, 80211b, 80211g, 80211n, 80211ac, 80211ax",
               wifiStandardStr);
  cmd.AddValue("wifiRate",
               "WiFi data mode (VhtMcs7, HtMcs7, OfdmRate54Mbps, etc.)", wifiRate);

  // Background
  cmd.AddValue("background", "Enable background cross-traffic (0/1)", enableBackground);
  cmd.AddValue("numBgNodes", "Number of background traffic nodes", numBgNodes);
  cmd.AddValue("bgRate", "Background traffic rate per node", bgRate);

  // Simulation
  cmd.AddValue("duration", "Simulation duration in seconds", simulationTime);
  cmd.AddValue("run", "Run index for RNG seed", run);
  cmd.AddValue("flowMonitor", "Enable FlowMonitor (0/1)", flowMonitor);
  cmd.AddValue("pcap", "Enable PCAP tracing (0/1)", pcap);
  cmd.AddValue("periodicReport", "Enable periodic throughput report (0/1)", periodicReport);
  cmd.AddValue("queueDisc",
               "Queue discipline: ns3::PfifoFastQueueDisc or ns3::CoDelQueueDisc",
               queueDisc);
  cmd.AddValue("prefix", "Prefix for output files", prefixName);
  cmd.AddValue("verbose", "Enable verbose logging (0/1)", verbose);

  cmd.Parse(argc, argv);

  // -----------------------------------------------------------------------
  // Seed and logging
  // -----------------------------------------------------------------------
  SeedManager::SetSeed(1);
  SeedManager::SetRun(run);
  Time::SetResolution(Time::NS);

  if (verbose)
    {
      LogComponentEnableAll(LOG_PREFIX_TIME);
      LogComponentEnableAll(LOG_PREFIX_NODE);
      LogComponentEnable("TcpUdpComparison", LOG_LEVEL_ALL);
    }

  // -----------------------------------------------------------------------
  // Transport protocol configuration
  // -----------------------------------------------------------------------
  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(payloadSize));
  Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(1 << 21));
  Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(1 << 21));

  // Select TCP variant
  tcpVariant = std::string("ns3::") + tcpVariant;
  TypeId tcpTid;
  NS_ABORT_MSG_UNLESS(TypeId::LookupByNameFailSafe(tcpVariant, &tcpTid),
                      "TypeId " << tcpVariant << " not found");
  Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                     TypeIdValue(TypeId::LookupByName(tcpVariant)));

  // -----------------------------------------------------------------------
  // Parse WiFi standard string -> WifiStandard enum
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
    { NS_FATAL_ERROR("Unknown WiFi standard: " << wifiStandardStr
                     << ". Use: 80211a, 80211b, 80211g, 80211n, 80211ac, 80211ax"); }

  // -----------------------------------------------------------------------
  // Simulation time calculation
  // -----------------------------------------------------------------------
  double stopTime = 0.1 + simulationTime;

  // =======================================================================
  // TOPOLOGY CREATION
  // =======================================================================

  // Create node containers
  NodeContainer serverNode;
  serverNode.Create(1);

  NodeContainer routerNodes;
  routerNodes.Create(2);
  Ptr<Node> router1 = routerNodes.Get(0);
  Ptr<Node> router2 = routerNodes.Get(1);

  NodeContainer apNode;
  apNode.Create(1);

  // WiFi STA nodes: first numTcpSta are TCP, rest are UDP
  NodeContainer wifiStaNodes;
  wifiStaNodes.Create(numTcpSta + numUdpSta);
  NodeContainer tcpStaNodes;
  NodeContainer udpStaNodes;
  for (uint32_t i = 0; i < numTcpSta; i++)
    tcpStaNodes.Add(wifiStaNodes.Get(i));
  for (uint32_t i = 0; i < numUdpSta; i++)
    udpStaNodes.Add(wifiStaNodes.Get(numTcpSta + i));

  // Wired client nodes
  NodeContainer wiredTcpNodes;
  wiredTcpNodes.Create(numWiredTcp);
  NodeContainer wiredUdpNodes;
  wiredUdpNodes.Create(numWiredUdp);

  // Background traffic nodes
  NodeContainer bgNodes;
  if (enableBackground)
    bgNodes.Create(numBgNodes);

  // -----------------------------------------------------------------------
  // Link helpers
  // -----------------------------------------------------------------------
  // Error model for bottleneck
  Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
  uv->SetStream(50);
  RateErrorModel errorModel;
  errorModel.SetRandomVariable(uv);
  errorModel.SetUnit(RateErrorModel::ERROR_UNIT_PACKET);
  errorModel.SetRate(bottleneckErrorRate);

  // Server link
  PointToPointHelper serverLink;
  serverLink.SetDeviceAttribute("DataRate", StringValue(serverBandwidth));
  serverLink.SetChannelAttribute("Delay", StringValue(serverDelay));

  // Bottleneck link
  PointToPointHelper bottleneckLink;
  bottleneckLink.SetDeviceAttribute("DataRate", StringValue(bottleneckBandwidth));
  bottleneckLink.SetChannelAttribute("Delay", StringValue(bottleneckDelay));
  bottleneckLink.SetDeviceAttribute("ReceiveErrorModel", PointerValue(&errorModel));

  // Access links
  PointToPointHelper accessLink;
  accessLink.SetDeviceAttribute("DataRate", StringValue(accessBandwidth));
  accessLink.SetChannelAttribute("Delay", StringValue(accessDelay));

  // -----------------------------------------------------------------------
  // Install Internet stack on all wired-side nodes
  // -----------------------------------------------------------------------
  InternetStackHelper internet;
  internet.Install(serverNode);
  internet.Install(routerNodes);
  internet.Install(apNode);
  internet.Install(wiredTcpNodes);
  internet.Install(wiredUdpNodes);
  internet.Install(bgNodes);

  // Enable IP forwarding on routers and AP (multi-homed node)
  Config::Set("/NodeList/" + std::to_string(router1->GetId()) +
              "/$ns3::Ipv4/IpForward", BooleanValue(true));
  Config::Set("/NodeList/" + std::to_string(router2->GetId()) +
              "/$ns3::Ipv4/IpForward", BooleanValue(true));
  Config::Set("/NodeList/" + std::to_string(apNode.Get(0)->GetId()) +
              "/$ns3::Ipv4/IpForward", BooleanValue(true));

  // -----------------------------------------------------------------------
  // Queue disciplines
  // -----------------------------------------------------------------------
  TrafficControlHelper tch;
  if (queueDisc == "ns3::PfifoFastQueueDisc")
    tch.SetRootQueueDisc("ns3::PfifoFastQueueDisc");
  else if (queueDisc == "ns3::CoDelQueueDisc")
    tch.SetRootQueueDisc("ns3::CoDelQueueDisc");
  else
    NS_FATAL_ERROR("Unknown queue disc: " << queueDisc);

  // Dynamic queue size based on bottleneck BDP
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

  // --- Server <-> Router1 ---
  NetDeviceContainer devServerRouter1 = serverLink.Install(serverNode.Get(0), router1);
  tch.Install(devServerRouter1);
  address.SetBase("10.0.0.0", "255.255.255.0");
  Ipv4InterfaceContainer ifServerRouter1 = address.Assign(devServerRouter1);
  serverIfaces.Add(ifServerRouter1.Get(0)); // server IP

  // --- Router1 <-> Router2 (bottleneck) ---
  NetDeviceContainer devRouter1Router2 = bottleneckLink.Install(router1, router2);
  tch.Install(devRouter1Router2);
  address.SetBase("10.0.1.0", "255.255.255.0");
  address.Assign(devRouter1Router2);

  // --- Router2 <-> AP_Node ---
  NetDeviceContainer devRouter2Ap = accessLink.Install(router2, apNode.Get(0));
  tch.Install(devRouter2Ap);
  address.SetBase("10.0.2.0", "255.255.255.0");
  address.Assign(devRouter2Ap);

  // --- Router2 <-> Wired TCP clients (individual PtP links for isolation) ---
  for (uint32_t i = 0; i < wiredTcpNodes.GetN(); i++)
    {
      NetDeviceContainer dev = accessLink.Install(router2, wiredTcpNodes.Get(i));
      tch.Install(dev);
      address.SetBase(("10.0." + std::to_string(10 + i) + ".0").c_str(), "255.255.255.0");
      address.Assign(dev);
    }

  // --- Router2 <-> Wired UDP clients ---
  for (uint32_t i = 0; i < wiredUdpNodes.GetN(); i++)
    {
      NetDeviceContainer dev = accessLink.Install(router2, wiredUdpNodes.Get(i));
      tch.Install(dev);
      address.SetBase(("10.0." + std::to_string(50 + i) + ".0").c_str(), "255.255.255.0");
      address.Assign(dev);
    }

  // --- Router2 <-> Background nodes (cross-traffic) ---
  for (uint32_t i = 0; i < bgNodes.GetN(); i++)
    {
      NetDeviceContainer dev = accessLink.Install(router2, bgNodes.Get(i));
      tch.Install(dev);
      address.SetBase(("10.0." + std::to_string(010 + i) + ".0").c_str(), "255.255.255.0");
      address.Assign(dev);
    }

  // =======================================================================
  // WIFI SECTION
  // =======================================================================
  Ipv4InterfaceContainer apWifiIfaces;
  Ipv4InterfaceContainer staWifiIfaces;

  if (enableWifi && (numTcpSta + numUdpSta) > 0)
    {
      // Internet stack on WiFi STA nodes
      internet.Install(wifiStaNodes);

      // WiFi channel + PHY
      YansWifiChannelHelper wifiChannel;
      wifiChannel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
      wifiChannel.AddPropagationLoss("ns3::FriisPropagationLossModel",
                                     "Frequency", DoubleValue(wifiFreq));

      YansWifiPhyHelper wifiPhy;
      wifiPhy.SetChannel(wifiChannel.Create());
      wifiPhy.SetErrorRateModel("ns3::YansErrorRateModel");

      // Set channel settings (frequency band) for HT/VHT/HE standards
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

      // MAC
      WifiMacHelper wifiMac;
      Ssid ssid = Ssid("tcp-udp-test");

      // AP interface
      wifiMac.SetType("ns3::ApWifiMac",
                      "Ssid", SsidValue(ssid),
                      "EnableBeaconJitter", BooleanValue(false));
      NetDeviceContainer apWifiDev = wifi.Install(wifiPhy, wifiMac, apNode.Get(0));

      // STA interfaces
      wifiMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid));
      NetDeviceContainer staWifiDev = wifi.Install(wifiPhy, wifiMac, wifiStaNodes);

      // Mobility
      MobilityHelper mobility;
      Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator>();
      positionAlloc->Add(Vector(0.0, 0.0, 0.0));  // AP
      for (uint32_t i = 0; i < wifiStaNodes.GetN(); i++)
        positionAlloc->Add(Vector(5.0 + i * 2.0, 0.0, 0.0));
      mobility.SetPositionAllocator(positionAlloc);
      mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
      mobility.Install(apNode);
      mobility.Install(wifiStaNodes);

      // WiFi IP addressing
      Ipv4AddressHelper wifiAddress;
      wifiAddress.SetBase("10.0.200.0", "255.255.255.0");
      apWifiIfaces = wifiAddress.Assign(apWifiDev);
      staWifiIfaces = wifiAddress.Assign(staWifiDev);
    }

  // =======================================================================
  // GLOBAL ROUTING
  // =======================================================================
  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  // =======================================================================
  // APPLICATIONS
  // =======================================================================
  double warmupTime = 1.0;
  uint16_t tcpPortBase = 5000;
  uint16_t udpPortBase = 6000;

  ApplicationContainer clientApps;
  ApplicationContainer serverApps;

  Ipv4Address serverAddr = serverIfaces.GetAddress(0);

  // --- Server: TCP PacketSinks (one per TCP flow) ---
  for (uint32_t i = 0; i < numTcpSta + numWiredTcp; i++)
    {
      uint16_t port = static_cast<uint16_t>(tcpPortBase + i);
      PacketSinkHelper sink("ns3::TcpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(), port));
      ApplicationContainer app = sink.Install(serverNode.Get(0));
      serverApps.Add(app);
      uint32_t id = g_flowCounter++;
      g_tcpSinks[id] = DynamicCast<PacketSink>(app.Get(0));
      g_lastRx[id] = 0;
    }

  // --- Server: UDP PacketSinks (one per UDP flow) ---
  for (uint32_t i = 0; i < numUdpSta + numWiredUdp; i++)
    {
      uint16_t port = static_cast<uint16_t>(udpPortBase + i);
      PacketSinkHelper sink("ns3::UdpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(), port));
      ApplicationContainer app = sink.Install(serverNode.Get(0));
      serverApps.Add(app);
      uint32_t id = g_flowCounter++;
      g_udpSinks[id] = DynamicCast<PacketSink>(app.Get(0));
      g_lastRx[id] = 0;
    }

  // --- WiFi TCP clients (BulkSend, saturates pipe) ---
  for (uint32_t i = 0; i < numTcpSta; i++)
    {
      uint16_t port = static_cast<uint16_t>(tcpPortBase + i);
      BulkSendHelper tcpClient("ns3::TcpSocketFactory",
                               InetSocketAddress(serverAddr, port));
      tcpClient.SetAttribute("SendSize", UintegerValue(payloadSize));
      clientApps.Add(tcpClient.Install(tcpStaNodes.Get(i)));
    }

  // --- WiFi UDP clients (OnOff at controlled rate) ---
  for (uint32_t i = 0; i < numUdpSta; i++)
    {
      uint16_t port = static_cast<uint16_t>(udpPortBase + i);
      OnOffHelper udpClient("ns3::UdpSocketFactory",
                            InetSocketAddress(serverAddr, port));
      udpClient.SetAttribute("PacketSize", UintegerValue(payloadSize));
      udpClient.SetAttribute("DataRate", DataRateValue(DataRate(udpRate)));
      udpClient.SetAttribute("OnTime",
                             StringValue("ns3::ConstantRandomVariable[Constant=1]"));
      udpClient.SetAttribute("OffTime",
                             StringValue("ns3::ConstantRandomVariable[Constant=0]"));
      clientApps.Add(udpClient.Install(udpStaNodes.Get(i)));
    }

  // --- Wired TCP clients ---
  for (uint32_t i = 0; i < numWiredTcp; i++)
    {
      uint16_t port = static_cast<uint16_t>(tcpPortBase + numTcpSta + i);
      BulkSendHelper tcpClient("ns3::TcpSocketFactory",
                               InetSocketAddress(serverAddr, port));
      tcpClient.SetAttribute("SendSize", UintegerValue(payloadSize));
      clientApps.Add(tcpClient.Install(wiredTcpNodes.Get(i)));
    }

  // --- Wired UDP clients ---
  for (uint32_t i = 0; i < numWiredUdp; i++)
    {
      uint16_t port = static_cast<uint16_t>(udpPortBase + numUdpSta + i);
      OnOffHelper udpClient("ns3::UdpSocketFactory",
                            InetSocketAddress(serverAddr, port));
      udpClient.SetAttribute("PacketSize", UintegerValue(payloadSize));
      udpClient.SetAttribute("DataRate", DataRateValue(DataRate(udpRate)));
      udpClient.SetAttribute("OnTime",
                             StringValue("ns3::ConstantRandomVariable[Constant=1]"));
      udpClient.SetAttribute("OffTime",
                             StringValue("ns3::ConstantRandomVariable[Constant=0]"));
      clientApps.Add(udpClient.Install(wiredUdpNodes.Get(i)));
    }

  // --- Background cross-traffic (UDP, bursty OnOff pattern) ---
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
          clientApps.Add(bgClient.Install(bgNodes.Get(i)));
        }
    }

  // --- Start/Stop times ---
  serverApps.Start(Seconds(warmupTime * 0.5));
  serverApps.Stop(Seconds(stopTime + 1));
  clientApps.Start(Seconds(warmupTime));
  clientApps.Stop(Seconds(stopTime));

  // =======================================================================
  // TRACING
  // =======================================================================
  if (pcap)
    {
      serverLink.EnablePcapAll(prefixName + "-server", false);
      bottleneckLink.EnablePcapAll(prefixName + "-bottleneck", false);
      accessLink.EnablePcapAll(prefixName + "-access", false);
    }

  if (periodicReport)
    {
      Simulator::Schedule(Seconds(warmupTime + 0.5),
                          &ReportThroughput, 500.0); // report every 500 ms
    }

  // =======================================================================
  // FLOW MONITOR
  // =======================================================================
  FlowMonitorHelper flowmonHelper;
  Ptr<FlowMonitor> monitor;
  if (flowMonitor)
    {
      monitor = flowmonHelper.InstallAll();
    }

  // =======================================================================
  // DISPLAY CONFIGURATION
  // =======================================================================
  std::cout << "\n==============================================" << std::endl;
  std::cout << "  TCP vs UDP Comparison Simulation" << std::endl;
  std::cout << "==============================================" << std::endl;
  std::cout << "TCP variant:       " << tcpVariant << std::endl;
  std::cout << "TCP flows (WiFi):  " << numTcpSta << std::endl;
  std::cout << "UDP flows (WiFi):  " << numUdpSta << " @" << udpRate << "/client" << std::endl;
  std::cout << "TCP flows (Wired): " << numWiredTcp << std::endl;
  std::cout << "UDP flows (Wired): " << numWiredUdp << " @" << udpRate << "/client" << std::endl;
  std::cout << "---" << std::endl;
  std::cout << "Server link:       " << serverBandwidth << " / " << serverDelay << std::endl;
  std::cout << "Bottleneck link:   " << bottleneckBandwidth << " / " << bottleneckDelay
            << " / error=" << bottleneckErrorRate << std::endl;
  std::cout << "Access link:       " << accessBandwidth << " / " << accessDelay << std::endl;
  std::cout << "WiFi:              " << (enableWifi ? "enabled" : "disabled")
            << " (" << wifiStandardStr << " @" << wifiRate << ")" << std::endl;
  std::cout << "Queue disc:        " << queueDisc << std::endl;
  std::cout << "Background:        " << (enableBackground ? "enabled" : "disabled")
            << " x" << numBgNodes << " @" << bgRate << std::endl;
  std::cout << "Duration:          " << simulationTime << " s  (run " << run << ")" << std::endl;
  std::cout << "Payload:           " << payloadSize << " B" << std::endl;
  std::cout << "==============================================" << std::endl;

  // =======================================================================
  // RUN
  // =======================================================================
  Simulator::Stop(Seconds(stopTime + 1));
  Simulator::Run();

  // =======================================================================
  // RESULTS
  // =======================================================================
  std::cout << "\n================ RESULTS ================" << std::endl;

  double totalTcpRx = 0;
  double totalUdpRx = 0;

  for (auto& entry : g_tcpSinks)
    {
      double rx = entry.second->GetTotalRx();
      totalTcpRx += rx;
      double thr = rx * 8.0 / (simulationTime * 1e6);
      std::cout << "TCP-Sink-" << entry.first << ": "
                << rx << " B  (" << thr << " Mbps)" << std::endl;
    }

  for (auto& entry : g_udpSinks)
    {
      double rx = entry.second->GetTotalRx();
      totalUdpRx += rx;
      double thr = rx * 8.0 / (simulationTime * 1e6);
      std::cout << "UDP-Sink-" << entry.first << ": "
                << rx << " B  (" << thr << " Mbps)" << std::endl;
    }

  std::cout << "------------------------------------------" << std::endl;
  std::cout << "TCP total: " << totalTcpRx << " B  ("
            << (totalTcpRx * 8.0 / (simulationTime * 1e6)) << " Mbps)" << std::endl;
  std::cout << "UDP total: " << totalUdpRx << " B  ("
            << (totalUdpRx * 8.0 / (simulationTime * 1e6)) << " Mbps)" << std::endl;
  std::cout << "==========================================" << std::endl;

  // =======================================================================
  // FLOW MONITOR OUTPUT (CSV + XML)
  // =======================================================================
  if (flowMonitor)
    {
      monitor->CheckForLostPackets();
      Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());
      auto stats = monitor->GetFlowStats();

      // CSV output
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

      // XML output
      std::string xmlName = prefixName + "-r" + std::to_string(run) + ".flowmonitor";
      flowmonHelper.SerializeToXmlFile(xmlName, true, true);

      std::cout << "Flow CSV:  " << csvName << std::endl;
      std::cout << "Flow XML:  " << xmlName << std::endl;
    }

  Simulator::Destroy();
  return 0;
}
