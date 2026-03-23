#ifndef SMS_COMMAND_H
#define SMS_COMMAND_H

#include <stdint.h>
#include "nvs.h"

#ifndef ENABLE_SMS_COMMANDS
#define ENABLE_SMS_COMMANDS 0
#endif

#define SMS_NVS_KEY_HMAC "sms_hmac"
#define SMS_NVS_CTR "sms_ctr"
#define SMS_NVS_FROM "sms_from"
#define SMS_NVS_EN "sms_en"

#if ENABLE_SMS_COMMANDS
#include "../../libraries/FreematicsPlus/FreematicsNetwork.h"
void smsCommandPoll(CellSIMCOM* modem, int cellularRegistered, const char* devid, nvs_handle_t nvs_h);
#else
class CellSIMCOM;
static inline void smsCommandPoll(CellSIMCOM*, int, const char*, nvs_handle_t) {}
#endif

#endif
