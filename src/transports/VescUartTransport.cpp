#include "transports/VescUartTransport.h"
#include <Arduino.h>

#ifndef WOKWI_SIMULATION
#include <VescUart.h>
VescUart UART;
#endif

namespace Esk8OS {
namespace Transports {

void beginVescUart() {
#ifndef WOKWI_SIMULATION
    Serial1.begin(115200, SERIAL_8N1, 18, 17); // GPIO 18 RX, 17 TX
    UART.setSerialPort(&Serial1);
#endif
}

}
}
