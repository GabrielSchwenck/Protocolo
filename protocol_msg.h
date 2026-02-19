#ifndef PROTOCOL_MSG_H
#define PROTOCOL_MSG_H

#include <stdint.h>
#include <stddef.h>
#include <windows.h>
#include "cJSON.h"

#define SYNC_FLAG_VALUE 0x0079CFEB

#pragma pack(push, 1)
typedef struct {
    uint32_t sync_flag; 
    uint32_t msg_crc32; 
    uint16_t head_crc16; 
    uint8_t  head_len;
    uint8_t  uri_len; 
    uint32_t msg_len; 
    uint16_t serial; 
    uint8_t  type; 
    uint8_t  need_resp;
} ProtocolHeader;
#pragma pack(pop)

// --- NOVA ESTRUTURA PARA PACOTES VALIDADOS ---
typedef struct {
    int is_valid;            // 1 se temos um pacote perfeito, 0 se não
    int bytes_to_consume;    // Quantos bytes o main.c deve apagar do buffer (limpar lixo ou pacote lido)
    char uri[128];           // A rota do comando (ex: "/api/push/recog_result")
    char body[80000];        // O corpo em JSON ou Base64
    uint8_t type;            // 0: Request, 1: Response, 2: Evento (Reconhecimento)
    uint16_t serial;         // ID da mensagem
} ParsedPacket;

// --- FUNÇÕES ---
int protocol_send_msg(HANDLE hSerial, const char* uri, const char* body, uint16_t seq);

// Nova função: Inspeciona o buffer bruto e devolve um pacote validado se existir
ParsedPacket protocol_parse_buffer(const uint8_t *buffer, int current_len);

// (Mantenha as declarações do base64, crc32, extract_int_safe...)
char* base64_encode(const unsigned char *data, size_t input_length, size_t *output_length);
unsigned char* base64_decode(const char *data, size_t input_length, size_t *output_length);
int find_pattern_index(const uint8_t *buffer, int buffer_len, const char *pattern);
int extract_int_safe(const uint8_t *buffer, int len, const char *key);
int FacePass_ExtractData(cJSON *json, int *score_out);

#endif // PROTOCOL_MSG_H