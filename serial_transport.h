#ifndef SERIAL_TRANSPORT_H
#define SERIAL_TRANSPORT_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

// Tipo de função para o Callback. 
// Será invocada automaticamente sempre que chegarem dados novos.
typedef void (*SerialRxCallback)(const uint8_t *data, uint32_t length);

HANDLE serial_open(const char *portName, int baudRate);
int serial_write(HANDLE hSerial, const uint8_t *data, uint32_t length);
void serial_purge(HANDLE hSerial);
void serial_close(HANDLE hSerial);

// --- NOVAS FUNÇÕES DA THREAD ---
bool serial_start_rx_thread(HANDLE hSerial, SerialRxCallback callback);
void serial_stop_rx_thread();

#endif // SERIAL_TRANSPORT_H