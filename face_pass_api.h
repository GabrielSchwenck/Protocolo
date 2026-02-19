#ifndef FACE_PASS_API_H
#define FACE_PASS_API_H

#include <windows.h>
#include <stdint.h>

// Estrutura para o cabeçalho do ficheiro .bin guardado localmente
#pragma pack(push, 1)
typedef struct {
    uint16_t face_id;      
    uint16_t feature_len;  
} UserHeader;
#pragma pack(pop)

// Inicializa o módulo
void FacePass_InitModule(HANDLE hSerial, uint16_t *seq);

// Cria o grupo de faces base
void FacePass_CreateFaceGroup(HANDLE hSerial, uint16_t *seq);

// Configura a verificação de duplicidade (1 para ativar, 0 para desativar)
void FacePass_SetDeduplication(HANDLE hSerial, int state, uint16_t *seq);

// Inicia o processo de registo (cadastro) de uma nova face
void FacePass_StartEnroll(HANDLE hSerial, int face_id, int timeout_ms, uint16_t *seq);

// Inicia o modo de reconhecimento contínuo
void FacePass_StartRecog(HANDLE hSerial, uint16_t *seq);

// Pausa o reconhecimento
void FacePass_Pause(HANDLE hSerial, uint16_t *seq);

// Apaga todas as faces guardadas no módulo
void FacePass_DeleteAll(HANDLE hSerial, uint16_t *seq);

#endif // FACE_PASS_API_H