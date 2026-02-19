#include "protocol_msg.h"
#include "serial_transport.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// --- TABELA BASE64 ---
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const int mod_table[] = {0, 2, 1};

// --- FUNÇÕES DE CODIFICAÇÃO (BASE64) ---

char *base64_encode(const unsigned char *data, size_t input_length, size_t *output_length) {
    *output_length = 4 * ((input_length + 2) / 3);
    char *encoded_data = (char *)malloc(*output_length + 1);
    if (encoded_data == NULL) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = b64_table[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = b64_table[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = b64_table[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = b64_table[(triple >> 0 * 6) & 0x3F];
    }
    for (int i = 0; i < mod_table[input_length % 3]; i++)
        encoded_data[*output_length - 1 - i] = '=';

    encoded_data[*output_length] = '\0';
    return encoded_data;
}

unsigned char *base64_decode(const char *data, size_t input_length, size_t *output_length) {
    if (input_length % 4 != 0) return NULL;
    *output_length = input_length / 4 * 3;
    if (data[input_length - 1] == '=') (*output_length)--;
    if (data[input_length - 2] == '=') (*output_length)--;

    unsigned char *decoded_data = (unsigned char *)malloc(*output_length);
    if (decoded_data == NULL) return NULL;

    int decoding_table[256];
    for (int i = 0; i < 256; i++) decoding_table[i] = -1;
    for (int i = 0; i < 64; i++) decoding_table[(unsigned char)b64_table[i]] = i;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t sextet_a = data[i] == '=' ? 0 & i++ : decoding_table[(unsigned char)data[i++]];
        uint32_t sextet_b = data[i] == '=' ? 0 & i++ : decoding_table[(unsigned char)data[i++]];
        uint32_t sextet_c = data[i] == '=' ? 0 & i++ : decoding_table[(unsigned char)data[i++]];
        uint32_t sextet_d = data[i] == '=' ? 0 & i++ : decoding_table[(unsigned char)data[i++]];
        uint32_t triple = (sextet_a << 3 * 6) + (sextet_b << 2 * 6) + (sextet_c << 1 * 6) + (sextet_d << 0 * 6);

        if (j < *output_length) decoded_data[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 0 * 8) & 0xFF;
    }
    return decoded_data;
}

// --- FUNÇÕES DE CRC ---

uint16_t calc_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) { 
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021; 
            else crc >>= 1; 
        }
    }
    return crc;
}

uint32_t calc_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) { 
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320; 
            else crc >>= 1; 
        }
    }
    return ~crc;
}

// --- FUNÇÕES DE PARSING ---

int find_pattern_index(const uint8_t *buffer, int buffer_len, const char *pattern) {
    int pattern_len = strlen(pattern);
    if (buffer_len < pattern_len) return -1;
    for (int i = 0; i <= buffer_len - pattern_len; i++) {
        if (buffer[i] == pattern[0]) { 
            if (memcmp(buffer + i, pattern, pattern_len) == 0) return i;
        }
    }
    return -1; 
}

int extract_int_safe(const uint8_t *buffer, int len, const char *key) {
    int idx = find_pattern_index(buffer, len, key);
    if (idx >= 0) return atoi((char*)&buffer[idx + strlen(key)]);
    return -1;
}

// --- NÚCLEO DO PROTOCOLO: ENVIO DE MENSAGEM ---

int protocol_send_msg(HANDLE hSerial, const char* uri, const char* body, uint16_t seq) {
    if (!hSerial || !uri) return -1;

    static uint8_t tx_buffer[20480]; 
    ProtocolHeader *h = (ProtocolHeader*)tx_buffer;
    
    uint32_t uri_len = strlen(uri) + 1; 
    uint32_t body_len = body ? strlen(body) : 0;
    
    // Monta os campos principais
    h->sync_flag = SYNC_FLAG_VALUE; 
    h->head_len = sizeof(ProtocolHeader); // 20 bytes
    h->uri_len = (uint8_t)uri_len;
    h->msg_len = h->head_len + uri_len + body_len; 
    h->serial = seq; 
    h->type = 0; 
    h->need_resp = 1;
    
    // Copia URI e BODY para o buffer sequencialmente
    memcpy(tx_buffer + h->head_len, uri, uri_len); 
    if(body_len > 0) {
        memcpy(tx_buffer + h->head_len + uri_len, body, body_len);
    }
    
    // Calcula CRCs
    uint8_t *header_bytes = (uint8_t*)h; 
    h->head_crc16 = calc_crc16(header_bytes + 12, 8); 
    h->msg_crc32 = calc_crc32(tx_buffer + 8, h->msg_len - 8);
    
    // Chama a Camada 1 para fazer o envio real para o Hardware
    return serial_write(hSerial, tx_buffer, h->msg_len);
}

ParsedPacket protocol_parse_buffer(const uint8_t *buffer, int current_len) {
    ParsedPacket pkt;
    memset(&pkt, 0, sizeof(ParsedPacket));

    // 1. Procura o início do pacote (SYNC_FLAG)
    int sync_idx = -1;
    for (int i = 0; i <= current_len - 4; i++) {
        uint32_t *flag = (uint32_t*)(buffer + i);
        if (*flag == SYNC_FLAG_VALUE) {
            sync_idx = i;
            break;
        }
    }

    // Se não encontrou o início, não há nada a fazer.
    // Se o buffer estiver muito cheio de lixo, mandamos limpar.
    if (sync_idx == -1) {
        if (current_len > 4000) pkt.bytes_to_consume = current_len; 
        return pkt;
    }

    // Se o pacote não começa na posição 0, significa que há "lixo" antes dele.
    // Pedimos para o main.c consumir esse lixo primeiro e tentar na próxima vez.
    if (sync_idx > 0) {
        pkt.bytes_to_consume = sync_idx;
        return pkt;
    }

    // O pacote começa no índice 0. Já recebemos o cabeçalho completo (20 bytes)?
    if (current_len < sizeof(ProtocolHeader)) {
        return pkt; // Faltam dados, espera mais um bocado
    }

    ProtocolHeader *h = (ProtocolHeader*)buffer;
    
    // O tamanho do pacote declarado no cabeçalho faz sentido?
    if (h->msg_len < sizeof(ProtocolHeader) || h->msg_len > 400000) {
        pkt.bytes_to_consume = 1; // Cabeçalho corrompido, salta um byte para procurar novo pacote
        return pkt;
    }

    // Já recebemos o pacote inteiro?
    if (current_len < h->msg_len) {
        return pkt; // Ainda a descarregar, espera!
    }

    // TEMOS O PACOTE INTEIRO! VALIDAÇÃO CRC32.
    // O CRC32 ignora os primeiros 8 bytes (sync_flag e o próprio msg_crc32)
    uint32_t calc_crc = calc_crc32(buffer + 8, h->msg_len - 8);
    if (h->msg_crc32 != calc_crc) {
        // CRC FALHOU! Ocorreu ruído elétrico e os bytes corromperam.
        // Rejeitamos o pacote saltando 1 byte para obrigar a recomeçar a busca.
        pkt.bytes_to_consume = 1;
        return pkt;
    }

    // PACOTE VALIDADO E PERFEITO! 
    pkt.is_valid = 1;
    pkt.bytes_to_consume = h->msg_len; // O main.c vai apagar este pacote do buffer
    pkt.type = h->type;
    pkt.serial = h->serial;

    // Extrai a URI de forma segura
    if (h->uri_len > 0 && h->uri_len < sizeof(pkt.uri)) {
        memcpy(pkt.uri, buffer + h->head_len, h->uri_len);
    }

    // Extrai o BODY (JSON)
    int body_len = h->msg_len - h->head_len - h->uri_len;
    if (body_len > 0 && body_len < sizeof(pkt.body)) {
        memcpy(pkt.body, buffer + h->head_len + h->uri_len, body_len);
        pkt.body[body_len] = '\0'; // Garante que a string tem fim
    }

    return pkt;
}

int FacePass_ExtractData(cJSON *json, int *score_out) {
    if (!json) return -1;
    
    int id_found = -1;
    if (score_out) *score_out = 0;

    // 1. Tenta encontrar o sub-objeto 'iden_info'
    cJSON *info = cJSON_GetObjectItemCaseSensitive(json, "iden_info");
    // Se iden_info existir, o alvo é ele. Caso contrário, é a raiz do json.
    cJSON *target = info ? info : json;

    // 2. Busca o ID (top1_id, face_id, etc.) no alvo definido
    const char *id_keys[] = {"top1_id", "face_id", "user_id", "id"};
    for (int i = 0; i < 4; i++) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(target, id_keys[i]);
        if (item) {
            if (cJSON_IsNumber(item)) id_found = item->valueint;
            else if (cJSON_IsString(item) && item->valuestring) id_found = atoi(item->valuestring);
            if (id_found != -1) break;
        }
    }

    // 3. Busca o Score no alvo definido
    cJSON *s = cJSON_GetObjectItemCaseSensitive(target, "iden_score");
    if (!s) s = cJSON_GetObjectItemCaseSensitive(target, "score");
    if (s && cJSON_IsNumber(s) && score_out) {
        *score_out = s->valueint;
    }

    return id_found;
}