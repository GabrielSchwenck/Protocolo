#include "face_pass_api.h"
#include "protocol_msg.h"
#include <stdio.h>

void FacePass_InitModule(HANDLE hSerial, uint16_t *seq) {
    protocol_send_msg(hSerial, "/api/module/init", "{}", (*seq)++);
}

void FacePass_CreateFaceGroup(HANDLE hSerial, uint16_t *seq) {
    protocol_send_msg(hSerial, "/api/book/create/group/face", "{}", (*seq)++);
}

void FacePass_SetDeduplication(HANDLE hSerial, int state, uint16_t *seq) {
    char json_body[50];
    sprintf(json_body, "{\"repeat_st\": %d}", state);
    protocol_send_msg(hSerial, "/api/set/face_repeat", json_body, (*seq)++);
}

void FacePass_StartEnroll(HANDLE hSerial, int face_id, int timeout_ms, uint16_t *seq) {
    char json_body[100];
    sprintf(json_body, "{\"face_id\": %d, \"obj_type\": 0, \"time\": %d}", face_id, timeout_ms);
    protocol_send_msg(hSerial, "/api/enroll/frm", json_body, (*seq)++);
}

void FacePass_StartRecog(HANDLE hSerial, uint16_t *seq) {
    protocol_send_msg(hSerial, "/api/module/start/recog", "{}", (*seq)++);
}

void FacePass_Pause(HANDLE hSerial, uint16_t *seq) {
    protocol_send_msg(hSerial, "/api/module/pause", "{}", (*seq)++);
}

void FacePass_DeleteAll(HANDLE hSerial, uint16_t *seq) {
    protocol_send_msg(hSerial, "/api/book/del/user", "{\"group_id\": 0, \"face_id\": 0, \"del_flag\": 2}", (*seq)++);
}