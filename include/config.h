// Configuration shared across the project
#ifndef CONFIG_H
#define CONFIG_H

// mDNS hostname (without .local)
#define MDNS_HOSTNAME "can2wifi"

// CAN pins (edit to match your wiring)
// Default placeholders - set to the GPIO pins you use for CAN TX/RX
#define CAN_TX_PIN 33
#define CAN_RX_PIN 32

// TCP server port for forwarding CAN frames
#define TCP_PORT 15731

#endif // CONFIG_H
