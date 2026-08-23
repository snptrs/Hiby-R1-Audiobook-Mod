/* Minimal host-only shim so ui.c can be typechecked on macOS. Values match
 * Linux uapi; nothing here ships. */
#ifndef _SHIM_LINUX_INPUT_H
#define _SHIM_LINUX_INPUT_H
#include <stdint.h>
#include <sys/time.h>
struct input_event { struct timeval time; uint16_t type; uint16_t code; int32_t value; };
#define EV_KEY 0x01
#define EV_ABS 0x03
#define ABS_PRESSURE 0x18
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_X 0x00
#define ABS_Y 0x01
#define BTN_TOUCH 0x14a
#define KEY_ESC 1
#define KEY_POWER 116
#define KEY_VOLUMEDOWN 114
#define KEY_VOLUMEUP 115
#define KEY_PLAYPAUSE 164
#define KEY_PREVIOUS 412
#define KEY_NEXT 407
#define KEY_BACK 158
#define KEY_PLAYCD 200
#define KEY_PAUSECD 201
#define KEY_PLAY 207
#define KEY_REWIND 168
#define KEY_FASTFORWARD 208
#define EVIOCGRAB 0x40044590
#define EVIOCGNAME(len) (0x80004506U | (((unsigned)(len)) << 16))
#endif
