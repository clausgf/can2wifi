#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "secrets.h"
#include "config.h"
#include <WebServer.h>
#include <ESPmDNS.h>
#include <vector>
#include <driver/twai.h>
#include <ArduinoWebsockets.h>

using namespace websockets;


// ***************************************************************************
// CAN bus setup and helpers
// ***************************************************************************

volatile uint32_t can_tx_count = 0;
volatile uint32_t can_rx_count = 0;

/**
 * Initialize TWAI (ESP32 CAN)
 */
bool can_init() {
  can_tx_count = 0;
  can_rx_count = 0;

  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NORMAL);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
    Serial.println("Failed to install TWAI driver CAN_RX_PIN=" + String(CAN_RX_PIN) + " CAN_TX_PIN=" + String(CAN_TX_PIN));
    return false;
  }
  if (twai_start() != ESP_OK) {
    Serial.println("Failed to start TWAI CAN_RX_PIN=" + String(CAN_RX_PIN) + " CAN_TX_PIN=" + String(CAN_TX_PIN));
    return false;
  }
  Serial.println("TWAI (CAN) started CAN_RX_PIN=" + String(CAN_RX_PIN) + " CAN_TX_PIN=" + String(CAN_TX_PIN));
  return true;
}

/**
 * Transmit CAN frame (extended ID, up to 8 bytes)
 */
bool can_transmit(uint32_t id, uint8_t data_len, const uint8_t* data) {
  if (data_len > 8) {
    Serial.println("can_transmit: DLC > 8");
    return false;
  }
  twai_message_t msg;
  msg.identifier = id;
  msg.data_length_code = data_len;
  msg.extd = 1; // Use extended ID (29 bit)
  msg.rtr = 0;
  for (uint8_t i=0; i<8; i++) {
    msg.data[i] = (i<data_len) ? data[i] : 0;
  }
  esp_err_t res = twai_transmit(&msg, pdMS_TO_TICKS(100));
  if (res != ESP_OK) {
    Serial.println("can_transmit: transmit error " + String(res));
    return false;
  }
  can_tx_count++;
  return true;
}

/**
 * Receive CAN frame (extended ID, up to 8 bytes)
 * This is a non-blocking call.
 */
bool can_receive(uint32_t &out_id, uint8_t &out_data_len, uint8_t* out_data) {
  twai_message_t msg;
  if (twai_receive(&msg, pdMS_TO_TICKS(0)) == ESP_OK) {
    out_id = msg.identifier;
    out_data_len = msg.data_length_code;
    if (out_data_len > 8) {
      Serial.println("can_receive: DLC > 8: " + String(out_data_len));
      return false;
    };
    for (uint8_t i=0; i<8; i++) {
      out_data[i] = (i<out_data_len) ? msg.data[i] : 0;
    }
    can_rx_count++;
    return true;
  }
  return false;
}


// ***************************************************************************
// TCP server/client setup and helpers
// ***************************************************************************

volatile uint32_t tcp_tx_count = 0;
volatile uint32_t tcp_rx_count = 0;

WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;

/**
 * Initialize TCP server
 */
bool tcp_init() {
  tcp_tx_count = 0;
  tcp_rx_count = 0;

  tcpServer.begin();
  tcpServer.setNoDelay(true);
  Serial.printf("TCP server listening on port %d\n", TCP_PORT);
  return true;
}

/**
 * Poll TCP server and accept new client
 */
void tcp_accept() {
  if (!tcpClient || !tcpClient.connected()) {
    if (tcpServer.hasClient()) {
      if (tcpClient) {
        tcpClient.stop();
      }
      tcpClient = tcpServer.available();
      Serial.println("TCP client connected");
    }
  }
}

/**
 * Transmit CAN frame over TCP.
 * TCP->CAN framing: [4 bytes big-endian ID][1 byte DLC][8 bytes payload (always 8 bytes)]
 * @return true if frame sent.
 */
bool tcp_transmit(WiFiClient &tcpClient, uint32_t id, uint8_t data_len, const uint8_t* data) {
  // Check client connection
  if (!tcpClient || !tcpClient.connected()) {
    return false;
  }
  if (data_len > 8) {
    Serial.println("tcp_transmit: DLC > 8: " + String(data_len));
    return false;
  }

  // Frame the CAN ID in big-endian
  uint8_t buf[5 + 8];
  buf[0] = (uint8_t)((id >> 24) & 0xFF);
  buf[1] = (uint8_t)((id >> 16) & 0xFF);
  buf[2] = (uint8_t)((id >> 8) & 0xFF);
  buf[3] = (uint8_t)(id & 0xFF);

  // Frame the DLC and data
  buf[4] = data_len;
  for (uint8_t i=0; i<8; i++) {
    buf[5 + i] = (i<data_len) ? data[i] : 0;
  }

  // Always write full 13 bytes (5 + 8)
  size_t written = tcpClient.write(buf, 5 + 8);
  if (written != 5 + 8) {
    Serial.println("tcp_transmit: write error, written=" + String(written));
    return false;
  }
  tcp_tx_count++;
  return true;
}

/**
 * Receive CAN frame over TCP.
 * TCP->CAN framing: [4 bytes big-endian ID][1 byte DLC][8 bytes payload (always 8 bytes)]
 * This is a non-blocking call; it checks if enough data is available.
 * @return true if frame received.
 */
bool tcp_receive(WiFiClient &tcpClient, uint32_t &out_id, uint8_t &out_data_len, uint8_t* out_data) {
  // Check client connection
  if (!tcpClient || !tcpClient.connected()) {
    return false;
  }

  // Check if enough data is available
  if (tcpClient.available() >= 5 + 8) {
    // Read the full frame
    uint8_t buf[5 + 8];
    tcpClient.read(buf, 5 + 8);
    // Parse ID, DLC, data
    out_id = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3]);
    out_data_len = buf[4];
    if (out_data_len > 8) {
      Serial.println("tcp_receive: DLC > 8: " + String(out_data_len));
      return false;
    }
    for (uint8_t i=0; i<8; i++) {
      out_data[i] = (i<out_data_len) ? buf[5 + i] : 0;
    }
    tcp_rx_count++;
    return true;
  }
  return false;
}


// ***************************************************************************
// Logging helpers
// ***************************************************************************

String hex_dump(const uint8_t data_len, const uint8_t* data) {
  String out = "";
  for (uint8_t i=0; i<8; i++) {
    char buf[4];
    sprintf(buf, "%02X", (i < data_len) ? data[i] : 0);
    out += buf;
    if (i < data_len-1) out += ' ';
  }
  return out;
}

String decode_can(const uint32_t id, const uint8_t data_len, const uint8_t* data) {
  uint8_t prio = (id >> 25) & 0x0f;
  uint8_t cmd = (id >> 17) & 0xff;
  uint8_t resp = (id >> 16) & 0x01;
  uint16_t addr = id & 0xffff;
  char buf[128];
  sprintf(buf, "P=%X ADDR=0x%04X R=%X CMD=%02X DLC=%u DATA=%s", prio, addr, resp, cmd, data_len, 
    hex_dump(data_len, data).c_str());
  return String(buf);
}

String log_frame(const String &direction, const uint32_t id, const uint8_t data_len, const uint8_t* data) {
  char buf[128];
  sprintf(buf, "%s ID=0x%08X DLC=%u DATA=%s | %s", 
    direction.c_str(), 
    id, 
    data_len, 
    hex_dump(data_len, data).c_str(), 
    decode_can(id, data_len, data).c_str());
  return String(buf);
}


// ***************************************************************************
// Web server setup and handlers
// ***************************************************************************

// Web server
WebServer webServer(80);

// WebSocket server
WebsocketsServer wsServer;
std::vector<WebsocketsClient> wsClients;

// Status broadcast timer
unsigned long lastStatusBroadcast = 0;

/**
 * Handle root web page
 */
void web_handle_root() {
  const char* page = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>can2wifi</title>
<style>
  html,body{height:100%;margin:0;font-family:Arial}
  .container{display:flex;flex-direction:column;height:100vh;padding:12px;box-sizing:border-box}
  .header{margin-bottom:8px}
  #status{margin-top:6px}
  #monitor{flex:1;overflow:auto;border:1px solid #ddd;padding:6px;background:#fafafa;white-space:pre;font-family:monospace}
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <h2 style="margin:0">can2wifi</h2>
    <div id="status">Loading...</div>
  </div>
  <h3 style="margin:4px 0">Frame Monitor</h3>
  <div id="monitor"></div>
</div>
<script>
(() => {
  const monitor = document.getElementById('monitor');
  const status = document.getElementById('status');
  let ws;
  function appendLine(s){
    const d = document.createElement('div'); d.textContent = s; monitor.appendChild(d);
    // autoscroll
    monitor.scrollTop = monitor.scrollHeight;
  }

  function connect(){
    ws = new WebSocket('ws://' + location.hostname + ':81');
    ws.onopen = () => { status.textContent = 'WS connected'; };
    ws.onclose = () => { status.textContent = 'WS disconnected, reconnecting...'; setTimeout(connect, 1500); };
    ws.onerror = () => { status.textContent = 'WS error'; ws.close(); };
    ws.onmessage = (ev) => {
      try {
        const j = JSON.parse(ev.data);
        if (j.type === 'frame') {
          const dir = j.dir || 'rx';
          const prio = j.prio;
          const cmd = j.cmd;
          const resp = j.resp;
          const addr = j.addr;
          const dlc = j.dlc;
          const data = (j.data||[]).map(x=>('0'+x.toString(16)).slice(-2).toUpperCase()).join(' ');
          appendLine(`${dir.toUpperCase()} P=${prio} ADDR=0x${addr.toString(16).toUpperCase().padStart(4,'0')} R=${resp} CMD=${cmd.toString(16).toUpperCase().padStart(2,'0')} DLC=${dlc} DATA=${data}`);
        } else if (j.type === 'status') {
          status.textContent = `IP: ${j.ip} | TCP client: ${j.tcp_client} | Sent: ${j.sent} | Recv: ${j.received}`;
        }
      } catch(e){ console.error('ws parse', e); }
    };
  }
  connect();
})();
</script>
</body>
</html>
)rawliteral";
  webServer.send(200, "text/html", page);
  Serial.println("Served root page to " + webServer.client().remoteIP().toString());
}

/**
 * Initialize web server
 */
bool web_init() {
  webServer.on("/", web_handle_root);
  webServer.begin();
  Serial.println("Web server started on port 80");

  wsServer.listen(81); // gilmaimon/ArduinoWebsockets connection handling in loop()
  Serial.println("WebSocket server started on port 81");

  return true;
}

bool ws_handle() {
  // accept new websocket clients
  while (wsServer.poll()) {
    WebsocketsClient client = wsServer.accept();
    if (!client.available()) {
      Serial.println("WebSocket client connection failed");
      continue;
    }
    wsClients.push_back(client);
    Serial.println("WebSocket client connected, total clients: " + String(wsClients.size()));
  }

  // poll existing clients and remove disconnected ones
  for (auto it = wsClients.begin(); it != wsClients.end(); ) {
    WebsocketsClient &client = *it;
    if (!client.available()) {
      // Client disconnected
      it = wsClients.erase(it);
      Serial.println("WebSocket client disconnected, total clients: " + String(wsClients.size()));
    } else {
      client.poll();
      ++it;
    }
  }

  // periodic status broadcast
  if (millis() - lastStatusBroadcast > 2000) {
    lastStatusBroadcast = millis();
    String status = String("{\"type\":\"status\",\"ip\":\"") + WiFi.localIP().toString() + 
      String("\",\"tcp_client\":") + String((tcpClient && tcpClient.connected())?"true":"false") + 
      String(",\"sent\":") + String(can_tx_count) + 
      String(",\"received\":") + 
      String(can_rx_count) + 
      String("}");
    for (int i = (int)wsClients.size() - 1; i >= 0; --i) {
      if (!wsClients[i].available()) { wsClients.erase(wsClients.begin() + i); continue; }
      wsClients[i].send(status.c_str());
    }
  }
  return true;
}

/**
 * Broadcast CAN frame event to all WebSocket clients
 */
void ws_broadcast_frame(const String &direction, const uint32_t id, const uint8_t data_len, const uint8_t* data) {
  // decode id into fields
  uint8_t prio = (id >> 25) & 0x0f;
  uint8_t cmd = (id >> 17) & 0xff;
  uint8_t resp = (id >> 16) & 0x01;
  uint16_t addr = id & 0xffff;

  String j = String("{\"type\":\"frame\"") +
    String(",\"dir\":\"") + String(direction) +
    String("\",\"prio\":") + String(prio) +
    String(",\"cmd\":") + String(cmd) +
    String(",\"resp\":") + String(resp) +
    String(",\"addr\":") + String(addr) +
    String(",\"dlc\":") + String(data_len) + String(",\"data\":[");

  for (uint8_t i=0; i<data_len; i++) {
    j += String(data[i]);
    if (i+1<data_len) j += ",";
  }
  j += String("],\"ts\":") + String(millis()) + String("}");

  for (int i = (int)wsClients.size() - 1; i >= 0; --i) {
    if (!wsClients[i].available()) { wsClients.erase(wsClients.begin() + i); continue; }
    wsClients[i].send(j.c_str());
  }
}


// ***************************************************************************
// Arduino setup and loop
// ***************************************************************************

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("Starting CAN2WiFi - initializing WiFi...");

  // Connect to WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(500);
    if (millis() - start > 20000) {
      Serial.println("\nWiFi connect timeout, restarting...");
      delay(2000);
      ESP.restart();
    }
  }
  Serial.println();
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());

  // Setup OTA
  ArduinoOTA.setHostname("can2wifi");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("Start OTA");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd OTA");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  // Start mDNS responder to publish the device hostname
  if (MDNS.begin(MDNS_HOSTNAME)) {
    Serial.printf("mDNS responder started as %s.local\n", MDNS_HOSTNAME);
    MDNS.addService("_http", "_tcp", 80);
    MDNS.addService("_can2wifi", "_tcp", TCP_PORT);
  } else {
    Serial.println("Error setting up mDNS responder");
  }

  // Initialize services
  if (!can_init()) {
    Serial.println("CAN init failed — continuing without CAN");
  }
  if (!tcp_init()) {
    Serial.println("TCP init failed — continuing without TCP server");
  }
  if (!web_init()) {
    Serial.println("Web server init failed — continuing without web server");
  }
}

// ***************************************************************************

void loop() {
  ArduinoOTA.handle();
  webServer.handleClient();
  wsServer.poll();
  ws_handle();
  tcp_accept();

  uint32_t id;
  uint8_t payload_len;
  uint8_t payload[8];

  // Read TCP and forward to CAN
  bool tcp_received = tcp_receive(tcpClient, id, payload_len, payload);
  if (tcp_received) {
    bool can_transmitted = can_transmit(id, payload_len, payload);
    String log_msg = log_frame(can_transmitted ? "TCP->CAN " : "TCP->None", id, payload_len, payload);
    Serial.println(log_msg);
    ws_broadcast_frame("tcp-can", id, payload_len, payload);
  }

  // Read CAN and forward to TCP client
  bool can_received = can_receive(id, payload_len, payload);
  if (can_received) {
    bool tcp_transmitted = tcp_transmit(tcpClient, id, payload_len, payload);

    String log_msg = log_frame(tcp_transmitted ? "CAN->TCP " : "CAN->None", id, payload_len, payload);
    Serial.println(log_msg);
    ws_broadcast_frame("can-tcp", id, payload_len, payload);
  }

  delay(5);
}
