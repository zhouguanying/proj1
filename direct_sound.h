#ifndef _DIRECT_SOUND_H_
#define _DIRECT_SOUND_H_

#include "utility.h"
#include <stdlib.h>
#include <windows.h>

typedef int (*sound_data_handler_t)(void *userp, void *data, size_t data_len);
int init_aec(HWND hwnd, char *faddr, unsigned short fport, unsigned short lport);
#define _DS_STOP 0
#define _DS_CAPTURE 1
#define _DS_PLAYBACK (1 << 1)

void lock_buffer(void);
void unlock_buffer(void);
int init_direct_sound(HWND hwnd);
void set_handler_capture(void *userp, sound_data_handler_t handler);
void set_handler_playback(void *userp, sound_data_handler_t handler);
int direct_sound_ctrl(unsigned int flags); //参数是位设置
#endif
