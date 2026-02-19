#include <windows.h>
#include <stdio.h>
#include <conio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Inclusão das nossas camadas arquiteturais
#include "serial_transport.h"
#include "protocol_msg.h"
#include "face_pass_api.h"
#include "cJSON.h"

// --- CONFIGURAÇÕES ---
#define SERIAL_PORT "COM14" 
#define BAUD_RATE 115200
#define TIMEOUT_MS 20000 

// --- VARIÁVEIS GLOBAIS PARTILHADAS ---
uint8_t acc_buffer[100000]; 
int acc_idx = 0;

// O "Cadeado" para proteger o buffer entre a thread de leitura e o programa principal
CRITICAL_SECTION buffer_lock; 

// --- FUNÇÃO CALLBACK (Chamada pela Thread da Camada 1) ---
// Esta função é executada em PANO DE FUNDO sempre que o módulo envia bytes
void on_serial_data_received(const uint8_t *data, uint32_t length) {
    // TRANCA O CADEADO: O main.c não pode ler o buffer enquanto estivermos a escrever nele
    EnterCriticalSection(&buffer_lock); 
    
    // Proteção contra overflow (estouro de memória)
    if (acc_idx + length < sizeof(acc_buffer)) {
        memcpy(acc_buffer + acc_idx, data, length);
        acc_idx += length;
    }
    
    // DESTRANCA O CADEADO
    LeaveCriticalSection(&buffer_lock); 
}

int main() {
    // 1. Inicializa o mecanismo de proteção (cadeado) antes de tudo
    InitializeCriticalSection(&buffer_lock); 

    // 2. Abre a porta serial (Camada 1)
    HANDLE hSerial = serial_open(SERIAL_PORT, BAUD_RATE);
    if (!hSerial) { 
        printf("ERRO: Porta %s ocupada ou inexistente.\n", SERIAL_PORT); 
        DeleteCriticalSection(&buffer_lock);
        return 1; 
    }

    // 3. ARRANCAMOS A THREAD DE BACKGROUND AQUI:
    // A partir desta linha, o Windows fica a escutar a porta COM14 silenciosamente
    if (!serial_start_rx_thread(hSerial, on_serial_data_received)) {
        printf("ERRO: Falha ao iniciar a thread de leitura.\n");
        serial_close(hSerial);
        DeleteCriticalSection(&buffer_lock);
        return 1;
    }

    uint16_t seq = 0;
    char filename[50];
    int ch;

    // 4. Inicialização limpa do módulo (Camada 3)
    FacePass_InitModule(hSerial, &seq);         Sleep(500);
    FacePass_CreateFaceGroup(hSerial, &seq);    Sleep(500);
    FacePass_SetDeduplication(hSerial, 1, &seq);Sleep(500); 
    
    int next_global_id = 1;

    // 5. Loop Principal
    while(1) {
        printf("\n=== CONTROLO DE ACESSOS ASSINCRONO ===\n");
        printf(" [C] Cadastrar\n", next_global_id);
        printf(" [R] Autenticar\n");      
        printf(" [D] Limpar Tudo\n");      
        printf(" [S] Sair\n");
        printf(">> ");

        ch = _getch(); ch = toupper(ch); printf("%c\n", ch);
        if (ch == 'S') break;

        // --- CADASTRAR ---
        if (ch == 'C') {
            printf("\n>> CADASTRO: Olhe para a camara.\n");
            
            // 1. Limpa o buffer de forma segura antes de começar
            EnterCriticalSection(&buffer_lock);
            acc_idx = 0; 
            memset(acc_buffer, 0, sizeof(acc_buffer));
            LeaveCriticalSection(&buffer_lock);

            // 2. Envia o comando para o módulo (Camada 3)
            FacePass_StartEnroll(hSerial, next_global_id, TIMEOUT_MS, &seq);

            int success = 0; 
            int fail_duplicate = 0;
            DWORD start_time = GetTickCount();

            // 3. Fica à espera da resposta sem travar a porta serial
            while ((GetTickCount() - start_time) < TIMEOUT_MS) {
                if(_kbhit()) { _getch(); break; } // Cancela se premir uma tecla

                ParsedPacket pkt;
                pkt.is_valid = 0;

                // TRANCA O CADEADO: Tenta extrair um pacote do buffer
                EnterCriticalSection(&buffer_lock);
                if (acc_idx > 0) {
                    pkt = protocol_parse_buffer(acc_buffer, acc_idx);
                    
                    // Se o parser disser para apagar bytes (lixo ou pacote lido), avançamos o buffer
                    if (pkt.bytes_to_consume > 0) {
                        int remaining = acc_idx - pkt.bytes_to_consume;
                        memmove(acc_buffer, acc_buffer + pkt.bytes_to_consume, remaining);
                        acc_idx = remaining;
                    }
                }
                LeaveCriticalSection(&buffer_lock); // DESTRANCA

                // 4. Se encontrou um pacote matematicamente perfeito:
                if (pkt.is_valid) {
                    
                    // Transforma o texto num Objeto JSON manipulável
                    cJSON *json = cJSON_Parse(pkt.body);
                    
                    if (json != NULL) {
                        cJSON *err_info = cJSON_GetObjectItemCaseSensitive(json, "err_info");
                        cJSON *id_existed = cJSON_GetObjectItemCaseSensitive(json, "id_existed");
                        cJSON *ft_string = cJSON_GetObjectItemCaseSensitive(json, "ft");

                        int err_val = (err_info != NULL) ? err_info->valueint : -1;
                        
                        // Verifica se deu erro de face duplicada
                        if (err_val > 0) {
                            int id_exist = (id_existed != NULL) ? id_existed->valueint : 0;
                            if (id_exist == 1 || err_val == 36) { 
                                printf("\n[ERRO] FACE DUPLICADA!\n"); 
                                fail_duplicate = 1; 
                            }
                        }

                        // Se não há erro e existe a string Base64 ('ft')
                        if (err_val == 0 && !fail_duplicate && cJSON_IsString(ft_string) && (ft_string->valuestring != NULL)) {
                            printf("\n[SUCESSO] Capturado!\n");

                            const char *b64_temp = ft_string->valuestring;
                            size_t b64_len = strlen(b64_temp);
                            size_t raw_len;
                            
                            // Descodifica o Base64 para binário
                            unsigned char *raw_data = base64_decode(b64_temp, b64_len, &raw_len);

                            if (raw_data) {
                                // Guarda o ficheiro .bin no disco
                                sprintf(filename, "face_%d.bin", next_global_id);
                                FILE *fp = fopen(filename, "wb");
                                if (fp) {
                                    UserHeader header;
                                    header.face_id = next_global_id;
                                    header.feature_len = (uint16_t)raw_len;
                                    fwrite(&header, sizeof(UserHeader), 1, fp);
                                    fwrite(raw_data, 1, raw_len, fp);
                                    fclose(fp);
                                    printf("-> Ficheiro guardado: %s\n", filename);
                                }
                                free(raw_data);
                            }
                            next_global_id++; 
                            success = 1; 
                        }
                        
                        cJSON_Delete(json); // Liberta a memória do objeto JSON
                    }
                }

                if (success || fail_duplicate) break; // Sai do while se terminou
                
                Sleep(10); // Pausa de 10ms para não usar 100% do CPU
            }
            if (!success && !fail_duplicate) printf("\n[FALHA] Timeout ou erro.\n");
        }
        
     // --- RECONHECER ---
        else if (ch == 'R') {
            printf("\n>> AUTENTICACAO: Olhe para a camara.\n");
            printf("   [Pressione qualquer tecla para parar...]\n");
            
            serial_purge(hSerial); 

            EnterCriticalSection(&buffer_lock);
            acc_idx = 0; 
            memset(acc_buffer, 0, sizeof(acc_buffer));
            LeaveCriticalSection(&buffer_lock);

            FacePass_StartRecog(hSerial, &seq);
            
            int id_found_flag = 0; 

            while (1) {
                if(_kbhit()) { 
                    _getch(); 
                    printf("\n\n[PARADO] Interrupcao manual pelo utilizador.\n");
                    break; 
                }

                ParsedPacket pkt;
                pkt.is_valid = 0;

                EnterCriticalSection(&buffer_lock);
                if (acc_idx > 0) {
                    pkt = protocol_parse_buffer(acc_buffer, acc_idx);
                    
                    if (pkt.bytes_to_consume > 0) {
                        int remaining = acc_idx - pkt.bytes_to_consume;
                        memmove(acc_buffer, acc_buffer + pkt.bytes_to_consume, remaining);
                        acc_idx = remaining;
                    }
                }
                LeaveCriticalSection(&buffer_lock);

                // 4. Se encontrou um pacote 100% válido
                if (pkt.is_valid) {
                    cJSON *json = cJSON_Parse(pkt.body);
                    
                    if (json != NULL) {
                        int id_val = 0;
                        int score_val = 0;

                        // CHAMA A FUNÇÃO ROBUSTA: Ela já mergulha no iden_info internamente
                        id_val = FacePass_ExtractData(json, &score_val);

                        if (id_val > 0) {
                            printf("\n\n[!!!] RECONHECIDO: ID %d (Score: %d%%)\n-> Acesso Permitido.\n", id_val, score_val);
                            id_found_flag = 1; 
                        } else if (id_val == 0) {
                            printf("\n[?] Pessoa desconhecida detetada.\n");
                        }
                        
                        cJSON_Delete(json);
                    }
                    
                    if (strstr(pkt.uri, "/api/push/recog_result") != NULL) {
                        printf(".");
                    }
                }
                
                Sleep(10);
            }

            FacePass_Pause(hSerial, &seq);
            Sleep(200); 
            serial_purge(hSerial);
            
            if (!id_found_flag && !acc_idx) printf("\n[FIM] Reconhecimento encerrado.\n");
            Sleep(1000);
        }
        
        // --- LIMPAR TUDO ---
        else if (ch == 'D') {
            FacePass_DeleteAll(hSerial, &seq);
            system("del face_*.bin"); 
            Sleep(500); next_global_id = 1; 
            printf("\n[RESET] Limpo.\n");
        }
    }
    
    // 6. Encerramento seguro
    serial_close(hSerial); // O serial_close já desliga a thread internamente
    DeleteCriticalSection(&buffer_lock); // Destrói o cadeado
    
    return 0;
}