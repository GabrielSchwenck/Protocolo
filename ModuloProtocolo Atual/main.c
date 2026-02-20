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

// --- VARIÁVEIS GLOBAIS PARTILHADAS (FIFO) ---
#define RB_CAPACITY 200000 // 200KB (Espaço seguro para fotos grandes em Base64)
uint8_t rb_memory[RB_CAPACITY];
RingBuffer rx_fifo;

// O "Cadeado" para proteger o buffer entre a thread de leitura e o programa principal
CRITICAL_SECTION buffer_lock; 

// --- FUNÇÃO CALLBACK (Chamada pela Thread da Camada 1) ---
// Executada em PANO DE FUNDO sempre que o módulo envia bytes
void on_serial_data_received(const uint8_t *data, uint32_t length) {
    // TRANCA O CADEADO: O main.c não pode ler o buffer enquanto escrevemos nele
    EnterCriticalSection(&buffer_lock); 
    
    // Injeta os dados novos na "cabeça" do Buffer Circular
    rb_put(&rx_fifo, data, length);
    
    // DESTRANCA O CADEADO
    LeaveCriticalSection(&buffer_lock); 
}

int main() {

    int aux=0;

    // 1. Inicializa o mecanismo de proteção (cadeado) e o Buffer Circular
    InitializeCriticalSection(&buffer_lock); 
    rb_init(&rx_fifo, rb_memory, RB_CAPACITY);

    // 2. Abre a porta serial (Camada 1)
    HANDLE hSerial = serial_open(SERIAL_PORT, BAUD_RATE);
    if (!hSerial) { 
        printf("[ERRO]\n"); 
        DeleteCriticalSection(&buffer_lock);
        return 1; 
    }

    // 3. ARRANCAMOS A THREAD DE BACKGROUND AQUI:
    if (!serial_start_rx_thread(hSerial, on_serial_data_received)) {
        printf("[ERRO]\n");
        serial_close(hSerial);
        DeleteCriticalSection(&buffer_lock);
        return 1;
    }

    uint16_t seq = 0;
    char filename[50];
    int ch;

    // 4. Inicialização limpa do módulo (Camada 3)
    FacePass_InitModule(hSerial, &seq);         Sleep(330);
    FacePass_CreateFaceGroup(hSerial, &seq);    Sleep(330);
    FacePass_SetDeduplication(hSerial, 1, &seq);Sleep(330); 
    
    int next_global_id = 1;

    // 5. Loop Principal
    while(1) {
        printf("\n=== CONTROLE DE ACESSOS ===\n");
        printf(" [C] Cadastrar\n");
        printf(" [R] Reconhecer\n");      
        printf(" [D] Deletar\n");      
        printf(" [S] Sair\n");
        printf("\n>> ");

        ch = _getch(); ch = toupper(ch); printf("%c\n", ch);
        if (ch == 'S') break;

        // ==========================================================
        // --- MODO: CADASTRAR ---
        // ==========================================================
        if (ch == 'C') {
            printf("\n>> Modo de Cadastro: Olhe para a camera.\n");
            
            // 1. Limpa o FIFO de forma segura antes de começar
            EnterCriticalSection(&buffer_lock);
            rb_init(&rx_fifo, rb_memory, RB_CAPACITY); 
            LeaveCriticalSection(&buffer_lock);

            // 2. Envia o comando para o módulo (Camada 3)
            FacePass_StartEnroll(hSerial, next_global_id, TIMEOUT_MS, &seq);

            int success = 0; 
            int fail_duplicate = 0;
            DWORD start_time = GetTickCount();

            // 3. Fica à espera da resposta (Timeout de segurança)
            while ((GetTickCount() - start_time) < TIMEOUT_MS) {
                if(_kbhit()) { _getch(); break; } // Cancela se premir uma tecla

                ParsedPacket pkt;
                pkt.is_valid = 0;

                // TRANCA O CADEADO: Tenta extrair um pacote do FIFO
                EnterCriticalSection(&buffer_lock);
                int available = rx_fifo.size;
                if (available > 0) {
                    static uint8_t flat_buffer[RB_CAPACITY];
                    rb_peek(&rx_fifo, flat_buffer, available);

                    pkt = protocol_parse_buffer(flat_buffer, available);
                    
                    if (pkt.bytes_to_consume > 0) {
                        rb_consume(&rx_fifo, pkt.bytes_to_consume);
                    }
                }
                LeaveCriticalSection(&buffer_lock); // DESTRANCA

          // 4. Se encontrou um pacote matematicamente perfeito:
                if (pkt.is_valid) {
                    cJSON *json = cJSON_Parse(pkt.body);
                    if (json != NULL) {
                        cJSON *err_info = cJSON_GetObjectItemCaseSensitive(json, "err_info");
                        cJSON *id_existed = cJSON_GetObjectItemCaseSensitive(json, "id_existed");
                        cJSON *ft_string = cJSON_GetObjectItemCaseSensitive(json, "ft");

                        int err_val = (err_info != NULL) ? err_info->valueint : -1;
                        int should_break = 0; // Bandeira para abortar o loop em caso de erro sem vazar memória
                        
                        // 1. Tratamento de Erros enviados pelo módulo
                        if (err_val > 0) {
                            int id_exist = (id_existed != NULL) ? id_existed->valueint : 0;
                            if (id_exist == 1 || err_val == 36) { 
                                printf("\n[ERRO: FACE DUPLICADA]\n"); 
                                fail_duplicate = 1; 
                            } else {
                                // Se for erro de Timeout (ex: 13) ou outro sem ser duplicado
                                printf("\n[ERRO]\n", err_val);
                                should_break = 1; 
                            }
                        }

                        // 2. Se não há erro e existe a string Base64 ('ft')
                        else if (err_val == 0 && !fail_duplicate && cJSON_IsString(ft_string) && (ft_string->valuestring != NULL)) {
                            const char *b64_temp = ft_string->valuestring;
                            
                            // --- A NOVA PROTEÇÃO ---
                            // Ignora se o módulo enviar a palavra "null" ou uma string curta demais
                            if (strcmp(b64_temp, "null") == 0 || strlen(b64_temp) < 100) {
                                aux++;
                                if(aux==3) should_break = 1;
                            } else {
                                // É um Base64 autêntico e volumoso!
                                printf("\n[SUCESSO]\n");

                                size_t b64_len = strlen(b64_temp);
                                size_t raw_len;
                                
                                unsigned char *raw_data = base64_decode(b64_temp, b64_len, &raw_len);

                                if (raw_data) {
                                    sprintf(filename, "face_%d.bin", next_global_id);
                                    FILE *fp = fopen(filename, "wb");
                                    if (fp) {
                                        UserHeader header;
                                        header.face_id = next_global_id;
                                        header.feature_len = (uint16_t)raw_len;
                                        fwrite(&header, sizeof(UserHeader), 1, fp);
                                        fwrite(raw_data, 1, raw_len, fp);
                                        fclose(fp);
                                        printf("-> Arquivo Salvo: %s\n", filename);
                                    }
                                    free(raw_data);
                                }
                                next_global_id++; 
                                success = 1; 
                            }
                        }
                        
                        cJSON_Delete(json); // Limpeza de memória obrigatória
                        
                        // Se houve falha grave (como ausência de rosto), paramos de esperar
                        if (should_break){
                            aux=0;
                            break;
                        }  
                    }
                }

                if (success || fail_duplicate) break; 
                Sleep(10); 
            }
            if (!success && !fail_duplicate) printf("\n[FALHA]\n");
        }
        
        // ==========================================================
        // --- MODO: RECONHECER ---
        // ==========================================================
        else if (ch == 'R') {
            printf("\n>> Modo de Reconhecimento: Olhe para a camera.\n");
            
            serial_purge(hSerial); 

            // Limpa o FIFO antes de iniciar
            EnterCriticalSection(&buffer_lock);
            rb_init(&rx_fifo, rb_memory, RB_CAPACITY);
            LeaveCriticalSection(&buffer_lock);

            FacePass_StartRecog(hSerial, &seq);
            
            int id_found_flag = 0; 

            while (1) {
                // Bloqueio de UI: Espera até uma tecla ser pressionada para sair do modo
                if(_kbhit()) { 
                    _getch(); 
                    printf("\n\nReconhecimento Interrompido.\n");
                    break; 
                }

                ParsedPacket pkt;
                pkt.is_valid = 0;

                // TRANCA O CADEADO: Lê do FIFO
                EnterCriticalSection(&buffer_lock);
                int available = rx_fifo.size;
                if (available > 0) {
                    static uint8_t flat_buffer[RB_CAPACITY];
                    rb_peek(&rx_fifo, flat_buffer, available);

                    pkt = protocol_parse_buffer(flat_buffer, available);
                    
                    if (pkt.bytes_to_consume > 0) {
                        rb_consume(&rx_fifo, pkt.bytes_to_consume);
                    }
                }
                LeaveCriticalSection(&buffer_lock); // DESTRANCA

                // Processa o pacote se for válido
                if (pkt.is_valid) {
                    cJSON *json = cJSON_Parse(pkt.body);
                    if (json != NULL) {
                        int id_val = 0;
                        int score_val = 0;

                        // A função robusta mapeia o iden_info internamente
                        id_val = FacePass_ExtractData(json, &score_val);

                        if (id_val > 0) {
                            printf("\n\nID Reconhecido: %d (Score: %d%%).\nAcesso Permitido.\n", id_val, score_val);
                            id_found_flag = 1; 
                            
                        } else if ((id_val == 0)||((strstr(pkt.uri, "recog") != NULL) || (strstr(pkt.uri, "push") != NULL))) {
                            // Rosto detectado mas não cadastrado
                            printf(".");
                        }
                        
                        cJSON_Delete(json);
                    }
                }
                Sleep(10);
            }

            // Garante que o hardware para de enviar pacotes de câmara antes de voltar ao menu
            FacePass_Pause(hSerial, &seq);
            Sleep(200); 
            serial_purge(hSerial);
        }
        
        // ==========================================================
        // --- MODO: LIMPAR TUDO ---
        // ==========================================================
        else if (ch == 'D') {
            FacePass_DeleteAll(hSerial, &seq);
            system("del face_*.bin"); 
            Sleep(500); 
            next_global_id = 1; 
            printf("\nTodos os Dados Foram DELETADOS.\n");
        }
    }
    
    // 6. Encerramento seguro
    serial_close(hSerial); // Já desliga a thread internamente de forma segura
    DeleteCriticalSection(&buffer_lock); // Destrói o cadeado
    
    return 0;
}