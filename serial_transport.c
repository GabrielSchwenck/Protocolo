#include "serial_transport.h"
#include <stdio.h>

// Variáveis globais privadas desta camada para gerir a thread
static HANDLE hThread = NULL;
static bool is_running = false;
static HANDLE hSerialGlobal = NULL;
static SerialRxCallback rx_callback = NULL;

// A Thread que corre em pano de fundo
static DWORD WINAPI RxThreadFunc(LPVOID lpParam) {
    uint8_t buffer[1024];
    DWORD bytesRead;

    while (is_running) {
        // ReadFile tenta ler. Fica bloqueado no máximo 50ms (conforme configurado abaixo)
        if (ReadFile(hSerialGlobal, buffer, sizeof(buffer), &bytesRead, NULL)) {
            if (bytesRead > 0 && rx_callback != NULL) {
                // Chegaram dados! Chama a função do main.c enviando os bytes
                rx_callback(buffer, bytesRead);
            }
        } else {
            Sleep(5); // Pausa de segurança em caso de erro na porta
        }
    }
    return 0;
}

HANDLE serial_open(const char *portName, int baudRate) {
    char path[20];
    sprintf(path, "\\\\.\\%s", portName);
    
    HANDLE hSerial = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (hSerial == INVALID_HANDLE_VALUE) return NULL;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(hSerial, &dcb);
    dcb.BaudRate = baudRate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(hSerial, &dcb);

    // CONFIGURAÇÃO DE TIMEOUTS (Muito importante para a thread não encravar para sempre)
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD; 
    timeouts.ReadTotalTimeoutConstant = 50; // A thread espera no máximo 50ms por bytes
    timeouts.ReadTotalTimeoutMultiplier = 0;
    SetCommTimeouts(hSerial, &timeouts);

    return hSerial;
}

bool serial_start_rx_thread(HANDLE hSerial, SerialRxCallback callback) {
    if (hThread != NULL || hSerial == NULL || callback == NULL) return false;

    hSerialGlobal = hSerial;
    rx_callback = callback;
    is_running = true;

    // Cria e arranca a Thread do Windows
    hThread = CreateThread(NULL, 0, RxThreadFunc, NULL, 0, NULL);
    return (hThread != NULL);
}

void serial_stop_rx_thread() {
    if (hThread != NULL) {
        is_running = false; // Sinaliza o 'while' da thread para parar
        WaitForSingleObject(hThread, 1000); // Aguarda até 1 segundo para ela fechar limpa
        CloseHandle(hThread);
        hThread = NULL;
    }
}

int serial_write(HANDLE hSerial, const uint8_t *data, uint32_t length) {
    if (hSerial == NULL || data == NULL || length == 0) return -1;
    DWORD bytesWritten = 0;
    if (WriteFile(hSerial, data, length, &bytesWritten, NULL)) return (int)bytesWritten;
    return -1;
}

void serial_purge(HANDLE hSerial) {
    if (hSerial != NULL) PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

void serial_close(HANDLE hSerial) {
    serial_stop_rx_thread(); // Garante que a thread morre antes de fechar a porta
    if (hSerial != NULL && hSerial != INVALID_HANDLE_VALUE) CloseHandle(hSerial);
}

