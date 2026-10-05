/* A stand-in for libnx's <switch.h>, declaring only what the switch/ sources use, with
 * the signatures that code assumes. It exists so a PC compiler can syntax- and
 * type-check the console frontend (make -f Makefile.host check-switch).
 *
 * It proves nothing about the real libnx: the names and prototypes here are
 * this project's reading of it. The real check is the build in the devkitPro
 * container (.github/workflows/opensave-switch.yml). */
#ifndef STUB_SWITCH_H
#define STUB_SWITCH_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef u32 Result;
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)

/* console */
void *consoleInit(void *);
void consoleUpdate(void *);
void consoleExit(void *);
void consoleClear(void);

/* framebuffer */
typedef struct NWindow NWindow;
typedef struct { int dummy; } Framebuffer;
#define PIXEL_FORMAT_RGBA_8888 1
NWindow *nwindowGetDefault(void);
Result framebufferCreate(Framebuffer *fb, NWindow *win, u32 w, u32 h, u32 format, u32 num_fbs);
void framebufferMakeLinear(Framebuffer *fb);
void *framebufferBegin(Framebuffer *fb, u32 *stride);
void framebufferEnd(Framebuffer *fb);
void framebufferClose(Framebuffer *fb);

/* input */
typedef struct { u64 buttons; } PadState;
enum {
    HidNpadButton_A = 1 << 0, HidNpadButton_B = 1 << 1, HidNpadButton_X = 1 << 2, HidNpadButton_Y = 1 << 3,
    HidNpadButton_L = 1 << 6, HidNpadButton_R = 1 << 7, HidNpadButton_ZL = 1 << 8, HidNpadButton_ZR = 1 << 9,
    HidNpadButton_Plus = 1 << 10, HidNpadButton_Left = 1 << 12, HidNpadButton_Up = 1 << 13,
    HidNpadButton_Right = 1 << 14, HidNpadButton_Down = 1 << 15
};
#define HidNpadStyleSet_NpadStandard 0
void padConfigureInput(u32 max_players, u32 style_set);
void padInitializeDefault(PadState *pad);
void padUpdate(PadState *pad);
u64 padGetButtons(const PadState *pad);
u64 padGetButtonsDown(const PadState *pad);

/* applet, sockets, random */
int appletMainLoop(void);
Result socketInitializeDefault(void);
void socketExit(void);
void randomGet(void *buf, size_t len);

/* accounts */
typedef struct { u64 uid[2]; } AccountUid;
typedef struct { int dummy; } AccountProfile;
typedef struct { char nickname[0x20]; } AccountProfileBase;
typedef struct { u8 pad[0x80]; } AccountUserData;
#define AccountServiceType_Application 0
#define ACC_USER_LIST_SIZE 8
Result accountInitialize(int type);
void accountExit(void);
Result accountListAllUsers(AccountUid *uids, s32 count, s32 *total);
Result accountGetProfile(AccountProfile *p, AccountUid uid);
Result accountProfileGet(AccountProfile *p, AccountUserData *ud, AccountProfileBase *base);
void accountProfileClose(AccountProfile *p);
Result accountGetPreselectedUser(AccountUid *out);

/* titles */
typedef struct { u64 application_id; u8 rest[24]; } NsApplicationRecord;
typedef struct { char name[0x200]; char author[0x100]; } NacpLanguageEntry;
typedef struct { u64 user_account_save_data_size, device_save_data_size; u8 pad[0x4000]; } NacpStruct;
typedef struct { NacpStruct nacp; u8 icon[0x20000]; } NsApplicationControlData;
#define NsApplicationControlSource_Storage 2
Result nsInitialize(void);
void nsExit(void);
Result nsListApplicationRecord(NsApplicationRecord *r, s32 count, s32 offset, s32 *out);
Result nsGetApplicationControlData(int source, u64 id, NsApplicationControlData *buf, size_t size, u64 *actual);
Result nacpGetLanguageEntry(NacpStruct *nacp, NacpLanguageEntry **out);

/* file systems */
typedef struct { void *p; } FsFileSystem;
typedef struct { u64 application_id; AccountUid uid; u8 save_data_type; } FsSaveDataAttribute;
#define FsSaveDataType_Account 1
#define FsSaveDataType_Device 2
#define FsSaveDataSpaceId_User 1
Result fsOpenSaveDataFileSystem(FsFileSystem *out, int space, const FsSaveDataAttribute *attr);
void fsFsClose(FsFileSystem *fs);
int fsdevMountDevice(const char *name, FsFileSystem fs);
int fsdevUnmountDevice(const char *name);
Result fsdevCommitDevice(const char *name);

/* ssl */
typedef struct { void *p; } SslContext;
typedef struct { void *p; } SslConnection;
#define SslVersion_Auto 0
#define SslVerifyOption_PeerCa 1
#define SslVerifyOption_HostName 2
#define SslIoMode_Blocking 0
Result sslInitialize(u32 num_sessions);
void sslExit(void);
Result sslCreateContext(SslContext *c, int version);
void sslContextClose(SslContext *c);
Result sslContextCreateConnection(SslContext *c, SslConnection *out);
Result sslConnectionSetSocketDescriptor(SslConnection *c, int sockfd, int *out_sockfd); /* as in libnx */
Result sslConnectionSetHostName(SslConnection *c, const char *name, u32 len);
Result sslConnectionSetVerifyOption(SslConnection *c, int opt);
Result sslConnectionSetIoMode(SslConnection *c, int mode);
Result sslConnectionDoHandshake(SslConnection *c, u32 *out_size, u32 *total_certs, void *buf, u32 bufsize);
Result sslConnectionRead(SslConnection *c, void *buffer, u32 size, u32 *out_size);
Result sslConnectionWrite(SslConnection *c, const void *buffer, u32 size, u32 *out_size);
void sslConnectionClose(SslConnection *c);

#endif
