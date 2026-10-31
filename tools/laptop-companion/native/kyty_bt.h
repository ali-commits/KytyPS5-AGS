#ifndef KYTY_COMPANION_BT_H
#define KYTY_COMPANION_BT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
const char* kyty_bt_version(void);
const char* kyty_bt_error(void);
uint64_t kyty_bt_now_us(void);
int kyty_bt_find_controller(const char* serial);
void* kyty_bt_open(uint32_t rate, int speaker, int controller);
uint64_t kyty_bt_queue(void* handle, const float* stereo, uint32_t frames);
uint64_t kyty_bt_queued_us(void* handle);
void kyty_bt_close(void* handle);
void kyty_bt_shutdown(void);
int kyty_bt_set_speaker(int controller, int enabled);
int kyty_bt_controls(int controller, const uint8_t* left11, const uint8_t* right11,
                     uint8_t flags, uint16_t large, uint16_t small,
                     uint32_t duration_ms, uint64_t lease_deadline_us);
int kyty_bt_renew(int controller, uint64_t lease_deadline_us);
uint64_t kyty_bt_accepted_reports(void);
uint64_t kyty_bt_failed_reports(void);
uint64_t kyty_bt_accepted_bytes(void);
const char* kyty_bt_source_sha256(void);
#ifdef __cplusplus
}
#endif
#endif
