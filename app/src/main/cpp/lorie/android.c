#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma ide diagnostic ignored "bugprone-reserved-identifier"
#pragma ide diagnostic ignored "OCUnusedMacroInspection"
#define __USE_GNU
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <libgen.h>
#include <globals.h>
#include <xkbsrv.h>
#include <errno.h>
#include <wchar.h>
#include <inpututils.h>
#include <randrstr.h>
#include "renderer.h"
#include "lorie.h"

#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)

static int argc = 0;
static char** argv = NULL;
static int conn_fd = -1;
extern char *__progname; // NOLINT(bugprone-reserved-identifier)
extern DeviceIntPtr lorieMouse, lorieMouseRelative, lorieTouch, lorieKeyboard;
extern ScreenPtr pScreenPtr;
extern int ucs2keysym(long ucs);
void lorieKeysymKeyboardEvent(KeySym keysym, int down);

char *xtrans_unix_path_x11 = NULL;
char *xtrans_unix_dir_x11 = NULL;

typedef enum {
    EVENT_SCREEN_SIZE, EVENT_TOUCH, EVENT_MOUSE, EVENT_KEY, EVENT_UNICODE, EVENT_CLIPBOARD_SYNC
} eventType;
typedef union {
    uint8_t type;
    struct {
        uint8_t t;
        uint16_t width, height, framerate;
    } screenSize;
    struct {
        uint8_t t;
        uint16_t type, id, x, y;
    } touch;
    struct {
        uint8_t t;
        float x, y;
        uint8_t detail, down, relative;
    } mouse;
    struct {
        uint8_t t;
        uint16_t key;
        uint8_t state;
    } key;
    struct {
        uint8_t t;
        uint32_t code;
    } unicode;
    struct {
        uint8_t t;
        uint8_t enable;
    } clipboardSync;
} lorieEvent;

static void* startServer(unused void* cookie) {
    lorieSetVM((JavaVM*) cookie);
    char* envp[] = { NULL };
    exit(dix_main(argc, (char**) argv, envp));
}

JNIEXPORT jboolean JNICALL
Java_com_termux_x11_CmdEntryPoint_start(JNIEnv *env, unused jclass cls, jobjectArray args) {
    pthread_t t;
    JavaVM* vm = NULL;
    // execv's argv array is a bit incompatible with Java's String[], so we do some converting here...
    argc = (*env)->GetArrayLength(env, args) + 1; // Leading executable path
    argv = (char**) calloc(argc, sizeof(char*));

    argv[0] = (char*) "Xlorie";
    for(int i=1; i<argc; i++) {
        jstring js = (jstring)((*env)->GetObjectArrayElement(env, args, i - 1));
        const char *pjc = (*env)->GetStringUTFChars(env, js, JNI_FALSE);
        argv[i] = (char *) calloc(strlen(pjc) + 1, sizeof(char)); //Extra char for the terminating NULL
        strcpy((char *) argv[i], pjc);
        (*env)->ReleaseStringUTFChars(env, js, pjc);
    }

    {
        cpu_set_t mask;
        long num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

        for (int i = num_cpus/2; i < num_cpus; i++)
            CPU_SET(i, &mask);

        if (sched_setaffinity(0, sizeof(cpu_set_t), &mask) == -1)
            log(ERROR, "Failed to set process affinity: %s", strerror(errno));
    }

    if (getenv("TERMUX_X11_DEBUG") && !fork()) {
        // Printing logs of local logcat.
        char pid[32] = {0};
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        sprintf(pid, "%d", getppid());
        execlp("logcat", "logcat", "--pid", pid, NULL);
    }

    // adb sets TMPDIR to /data/local/tmp which is pretty useless.
    if (!strcmp("/data/local/tmp", getenv("TMPDIR") ?: ""))
        unsetenv("TMPDIR");

    if (!getenv("TMPDIR")) {
        if (access("/tmp", F_OK) == 0)
            setenv("TMPDIR", "/tmp", 1);
        else if (access("/data/data/com.termux/files/usr/tmp", F_OK) == 0)
            setenv("TMPDIR", "/data/data/com.termux/files/usr/tmp", 1);
    }

    if (!getenv("TMPDIR")) {
        char* error = (char*) "$TMPDIR is not set. Normally it is pointing to /tmp of a container.";
        log(ERROR, "%s", error);
        dprintf(2, "%s\n", error);
        return JNI_FALSE;
    }

    {
        char* tmp = getenv("TMPDIR");
        char cwd[1024] = {0};

        if (!getcwd(cwd, sizeof(cwd)) || access(cwd, F_OK) != 0)
            chdir(tmp);
        asprintf(&xtrans_unix_path_x11, "%s/.X11-unix/X", tmp);
        asprintf(&xtrans_unix_dir_x11, "%s/.X11-unix/", tmp);
    }

    log(VERBOSE, "Using TMPDIR=\"%s\"", getenv("TMPDIR"));

    {
        const char *root_dir = dirname(getenv("TMPDIR"));
        const char* pathes[] = {
                "/etc/X11/fonts", "/usr/share/fonts/X11", "/share/fonts", NULL
        };
        for (int i=0; pathes[i]; i++) {
            char current_path[1024] = {0};
            snprintf(current_path, sizeof(current_path), "%s%s", root_dir, pathes[i]);
            if (access(current_path, F_OK) == 0) {
                char default_font_path[4096] = {0};
                snprintf(default_font_path, sizeof(default_font_path),
                         "%s/misc,%s/TTF,%s/OTF,%s/Type1,%s/100dpi,%s/75dpi",
                         current_path, current_path, current_path, current_path, current_path, current_path);
                defaultFontPath = strdup(default_font_path);
                break;
            }
        }
    }

    if (!getenv("XKB_CONFIG_ROOT")) {
        // chroot case
        const char *root_dir = dirname(getenv("TMPDIR"));
        char current_path[1024] = {0};
        snprintf(current_path, sizeof(current_path), "%s/usr/share/X11/xkb", root_dir);
        if (access(current_path, F_OK) == 0)
            setenv("XKB_CONFIG_ROOT", current_path, 1);
    }
    if (!getenv("XKB_CONFIG_ROOT")) {
        // proot case
        if (access("/usr/share/X11/xkb", F_OK) == 0)
            setenv("XKB_CONFIG_ROOT", "/usr/share/X11/xkb", 1);
        // Termux case
        else if (access("/data/data/com.termux/files/usr/share/X11/xkb", F_OK) == 0)
            setenv("XKB_CONFIG_ROOT", "/data/data/com.termux/files/usr/share/X11/xkb", 1);
    }

    if (!getenv("XKB_CONFIG_ROOT")) {
        char* error = (char*) "$XKB_CONFIG_ROOT is not set. Normally it is pointing to /usr/share/X11/xkb of a container.";
        log(ERROR, "%s", error);
        dprintf(2, "%s\n", error);
        return JNI_FALSE;
    }

    XkbBaseDirectory = getenv("XKB_CONFIG_ROOT");
    if (access(XkbBaseDirectory, F_OK) != 0) {
        log(ERROR, "%s is unaccessible: %s\n", XkbBaseDirectory, strerror(errno));
        printf("%s is unaccessible: %s\n", XkbBaseDirectory, strerror(errno));
        return JNI_FALSE;
    }

    (*env)->GetJavaVM(env, &vm);

    pthread_create(&t, NULL, startServer, vm);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_termux_x11_CmdEntryPoint_windowChanged(JNIEnv *env, unused jobject cls, jobject surface) {
    QueueWorkProc(lorieChangeWindow, NULL, surface ? (*env)->NewGlobalRef(env, surface) : NULL);
}

void handleLorieEvents(int fd, maybe_unused int ready, maybe_unused void *data) {
    ValuatorMask mask;
    lorieEvent e = {0};
    valuator_mask_zero(&mask);

    if (ready & X_NOTIFY_ERROR) {
//        RemoveNotifyFd(fd);
        InputThreadUnregisterDev(fd);
        close(fd);
        conn_fd = -1;
        lorieEnableClipboardSync(FALSE);
        return;
    }

    if (read(fd, &e, sizeof(e)) == sizeof(e)) {
        switch(e.type) {
            case EVENT_SCREEN_SIZE:
                __android_log_print(ANDROID_LOG_ERROR, "tx11-request", "window changed: %d %d", e.screenSize.width, e.screenSize.height);
                lorieConfigureNotify(e.screenSize.width, e.screenSize.height, e.screenSize.framerate);
                break;
            case EVENT_TOUCH: {
                double x, y;
                DDXTouchPointInfoPtr touch = TouchFindByDDXID(lorieTouch, e.touch.id, FALSE);

                x = (float) e.touch.x * 0xFFFF / (float) pScreenPtr->GetScreenPixmap(pScreenPtr)->drawable.width;
                y = (float) e.touch.y * 0xFFFF / (float) pScreenPtr->GetScreenPixmap(pScreenPtr)->drawable.height;

                // Avoid duplicating events
                if (touch && touch->active) {
                    double oldx, oldy;
                    if (e.touch.type == XI_TouchUpdate &&
                        valuator_mask_fetch_double(touch->valuators, 0, &oldx) &&
                        valuator_mask_fetch_double(touch->valuators, 1, &oldy) &&
                        oldx == x && oldy == y)
                        break;
                }

                // Sometimes activity part does not send XI_TouchBegin and sends only XI_TouchUpdate.
                if (e.touch.type == XI_TouchUpdate && (!touch || !touch->active))
                    e.touch.type = XI_TouchBegin;

                if (e.touch.type == XI_TouchEnd && (!touch || !touch->active))
                    break;

                __android_log_print(ANDROID_LOG_ERROR, "tx11-request", "touch event: %d %d %d %d", e.touch.type, e.touch.id, e.touch.x, e.touch.y);
                valuator_mask_set_double(&mask, 0, x);
                valuator_mask_set_double(&mask, 1, y);
                QueueTouchEvents(lorieTouch, e.touch.type, e.touch.id, 0, &mask);
                break;
            }
            case EVENT_MOUSE: {
                int flags;
                switch(e.mouse.detail) {
                    case 0: // BUTTON_UNDEFINED
                        if (e.mouse.relative) {
                            valuator_mask_set_double(&mask, 0, (double) e.mouse.x);
                            valuator_mask_set_double(&mask, 1, (double) e.mouse.y);
                            QueuePointerEvents(lorieMouseRelative, MotionNotify, 0, POINTER_RELATIVE | POINTER_ACCELERATE, &mask);
                        } else {
                            flags = POINTER_ABSOLUTE | POINTER_SCREEN | POINTER_NORAW;
                            valuator_mask_set_double(&mask, 0, (double) e.mouse.x);
                            valuator_mask_set_double(&mask, 1, (double) e.mouse.y);
                            QueuePointerEvents(lorieMouse, MotionNotify, 0, flags, &mask);
                        }
                        break;
                    case 1: // BUTTON_LEFT
                    case 2: // BUTTON_MIDDLE
                    case 3: // BUTTON_RIGHT
                        QueuePointerEvents(e.mouse.relative ? lorieMouseRelative : lorieMouse, e.mouse.down ? ButtonPress : ButtonRelease, e.mouse.detail, 0, &mask);
                        break;
                    case 4: // BUTTON_SCROLL
                        if (e.mouse.x) {
                            valuator_mask_zero(&mask);
                            valuator_mask_set_double(&mask, 2, (double) e.mouse.x / 120);
                            QueuePointerEvents(lorieMouseRelative, MotionNotify, 0, POINTER_RELATIVE, &mask);
                        }
                        if (e.mouse.y) {
                            valuator_mask_zero(&mask);
                            valuator_mask_set_double(&mask, 3, (double) e.mouse.y / 120);
                            QueuePointerEvents(lorieMouseRelative, MotionNotify, 0, POINTER_RELATIVE, &mask);
                        }
                        break;
                }
                break;
            }
            case EVENT_KEY:
                QueueKeyboardEvents(lorieKeyboard, e.key.state ? KeyPress : KeyRelease, e.key.key);
                break;
            case EVENT_UNICODE: {
                int ks = ucs2keysym((long) e.unicode.code);
                __android_log_print(ANDROID_LOG_DEBUG, "LorieNative", "Trying to input keysym %d\n", ks);
                lorieKeysymKeyboardEvent(ks, TRUE);
                lorieKeysymKeyboardEvent(ks, FALSE);
                break;
            }
            case EVENT_CLIPBOARD_SYNC:
                lorieEnableClipboardSync(e.clipboardSync.enable);
                break;
        }
    }
}

void lorieSendClipboardData(const char* data) {
    if (data && conn_fd != -1)
        write(conn_fd, data, strlen(data));
}

static Bool addFd(unused ClientPtr pClient, void *closure) {
//    SetNotifyFd((int) (int64_t) closure, handleLorieEvents, X_NOTIFY_READ, NULL);
    InputThreadRegisterDev((int) (int64_t) closure, handleLorieEvents, NULL);
    conn_fd = (int) (int64_t) closure;
    return TRUE;
}

JNIEXPORT jobject JNICALL
Java_com_termux_x11_CmdEntryPoint_getXConnection(JNIEnv *env, unused jobject cls) {
    int client[2];
    jclass ParcelFileDescriptorClass = (*env)->FindClass(env, "android/os/ParcelFileDescriptor");
    jmethodID adoptFd = (*env)->GetStaticMethodID(env, ParcelFileDescriptorClass, "adoptFd", "(I)Landroid/os/ParcelFileDescriptor;");
    socketpair(AF_UNIX, SOCK_STREAM, 0, client);
    fcntl(client[0], F_SETFL, fcntl(client[0], F_GETFL, 0) | O_NONBLOCK);
    QueueWorkProc(addFd, NULL, (void*) (int64_t) client[1]);

    return (*env)->CallStaticObjectMethod(env, ParcelFileDescriptorClass, adoptFd, client[0]);
}

void* logcatThread(void *arg) {
    char buffer[4096];
    size_t len;
    while((len = read((int) (int64_t) arg, buffer, 4096)) >=0)
        write(2, buffer, len);
    close((int) (int64_t) arg);
    return NULL;
}

JNIEXPORT jobject JNICALL
Java_com_termux_x11_CmdEntryPoint_getLogcatOutput(JNIEnv *env, unused jobject cls) {
    jclass ParcelFileDescriptorClass = (*env)->FindClass(env, "android/os/ParcelFileDescriptor");
    jmethodID adoptFd = (*env)->GetStaticMethodID(env, ParcelFileDescriptorClass, "adoptFd", "(I)Landroid/os/ParcelFileDescriptor;");
    const char *debug = getenv("TERMUX_X11_DEBUG");
    if (debug && !strcmp(debug, "1")) {
        pthread_t t;
        int p[2];
        pipe(p);
        fchmod(p[1], 0777);
        pthread_create(&t, NULL, logcatThread, (void*) (uint64_t) p[0]);
        return (*env)->CallStaticObjectMethod(env, ParcelFileDescriptorClass, adoptFd, p[1]);
    }
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_termux_x11_CmdEntryPoint_connected(__unused JNIEnv *env, __unused jclass clazz) {
    return conn_fd != -1;
}

static inline void checkConnection(JNIEnv* env) {
    int retval, b = 0;

    if (conn_fd == -1)
        return;

    if ((retval = recv(conn_fd, &b, 1, MSG_PEEK)) <= 0 && errno != EAGAIN) {
        log(DEBUG, "recv %d %s", retval, strerror(errno));
        jclass cls = (*env)->FindClass(env, "com/termux/x11/CmdEntryPoint");
        jmethodID method = !cls ? NULL : (*env)->GetStaticMethodID(env, cls, "requestConnection", "()V");
        if (method)
            (*env)->CallStaticVoidMethod(env, cls, method);

        close(conn_fd);
        conn_fd = -1;
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_connect(unused JNIEnv* env, unused jobject cls, jint fd) {
    conn_fd = fd;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    checkConnection(env);
    log(DEBUG, "XCB connection is successfull");
}

static char clipboard[1024*1024] = {0};

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_handleXEvents(JNIEnv *env, maybe_unused jobject thiz) {
    checkConnection(env);
    if (conn_fd != -1) {
        char none;
        memset(clipboard, 0, sizeof(clipboard));
        if (read(conn_fd, clipboard, sizeof(clipboard)) > 0) {
            clipboard[sizeof(clipboard) - 1] = 0;
            log(DEBUG, "Clipboard content (%zu symbols) is %s", strlen(clipboard), clipboard);
            jmethodID id = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, thiz), "setClipboardText","(Ljava/lang/String;)V");

            jclass cls_Charset = (*env)->FindClass(env, "java/nio/charset/Charset");
            jclass cls_CharBuffer = (*env)->FindClass(env, "java/nio/CharBuffer");
            jmethodID mid_Charset_forName = cls_Charset ? (*env)->GetStaticMethodID(env, cls_Charset, "forName", "(Ljava/lang/String;)Ljava/nio/charset/Charset;") : NULL;
            jmethodID mid_Charset_decode = cls_Charset ? (*env)->GetMethodID(env, cls_Charset, "decode", "(Ljava/nio/ByteBuffer;)Ljava/nio/CharBuffer;") : NULL;
            jmethodID mid_CharBuffer_toString = cls_CharBuffer ? (*env)->GetMethodID(env, cls_CharBuffer, "toString", "()Ljava/lang/String;") : NULL;

            if (!id)
                log(ERROR, "setClipboardText method not found");
            if (!cls_Charset)
                log(ERROR, "java.nio.charset.Charset class not found");
            if (!cls_CharBuffer)
                log(ERROR, "java.nio.CharBuffer class not found");
            if (!mid_Charset_forName)
                log(ERROR, "java.nio.charset.Charset.forName method not found");
            if (!mid_Charset_decode)
                log(ERROR, "java.nio.charset.Charset.decode method not found");
            if (!mid_CharBuffer_toString)
                log(ERROR, "java.nio.CharBuffer.toString method not found");

            if (id && cls_Charset && cls_CharBuffer && mid_Charset_forName && mid_Charset_decode && mid_CharBuffer_toString) {
                jobject bb = (*env)->NewDirectByteBuffer(env, clipboard, strlen(clipboard));
                jobject charset = (*env)->CallStaticObjectMethod(env, cls_Charset, mid_Charset_forName, (*env)->NewStringUTF(env, "UTF-8"));
                jobject cb = (*env)->CallObjectMethod(env, charset, mid_Charset_decode, bb);
                (*env)->DeleteLocalRef(env, bb);

                jstring str = (*env)->CallObjectMethod(env, cb, mid_CharBuffer_toString);
                (*env)->CallVoidMethod(env, thiz, id, str);
            }
        }

        while(read(conn_fd, &none, sizeof(none)) > 0);
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_startLogcat(JNIEnv *env, unused jobject cls, jint fd) {
    log(DEBUG, "Starting logcat with output to given fd");

    switch(fork()) {
        case -1:
            log(ERROR, "fork: %s", strerror(errno));
            return;
        case 0:
            dup2(fd, 1);
            dup2(fd, 2);
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            char buf[64] = {0};
            sprintf(buf, "--pid=%d", getppid());
            execl("/system/bin/logcat", "logcat", buf, NULL);
            log(ERROR, "exec logcat: %s", strerror(errno));
            (*env)->FatalError(env, "Exiting");
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_setClipboardSyncEnabled(unused JNIEnv* env, unused jobject cls, jboolean enable) {
    if (conn_fd != -1) {
        lorieEvent e = { .clipboardSync = { .t = EVENT_CLIPBOARD_SYNC, .enable = enable } };
        write(conn_fd, &e, sizeof(e));
        checkConnection(env);
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_sendWindowChange(unused JNIEnv* env, unused jobject cls, jint width, jint height, jint framerate) {
    if (conn_fd != -1) {
        lorieEvent e = { .screenSize = { .t = EVENT_SCREEN_SIZE, .width = width, .height = height, .framerate = framerate } };
        write(conn_fd, &e, sizeof(e));
        checkConnection(env);
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_sendMouseEvent(unused JNIEnv* env, unused jobject cls, jfloat x, jfloat y, jint which_button, jboolean button_down, jboolean relative) {
    if (conn_fd != -1) {
        lorieEvent e = { .mouse = { .t = EVENT_MOUSE, .x = x, .y = y, .detail = which_button, .down = button_down, .relative = relative } };
        write(conn_fd, &e, sizeof(e));
        checkConnection(env);
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_sendTouchEvent(unused JNIEnv* env, unused jobject cls, jint action, jint id, jint x, jint y) {
    if (conn_fd != -1 && action != -1) {
        lorieEvent e = { .touch = { .t = EVENT_TOUCH, .type = action, .id = id, .x = x, .y = y } };
        write(conn_fd, &e, sizeof(e));
        checkConnection(env);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_termux_x11_LorieView_sendKeyEvent(unused JNIEnv* env, unused jobject cls, jint scan_code, jint key_code, jboolean key_down) {
    if (conn_fd != -1) {
        int code = (scan_code) ?: android_to_linux_keycode[key_code];
        log(DEBUG, "Sending key: %d (%d %d %d)", code + 8, scan_code, key_code, key_down);
        lorieEvent e = { .key = { .t = EVENT_KEY, .key = code + 8, .state = key_down } };
        write(conn_fd, &e, sizeof(e));
        checkConnection(env);
    }

    return true;
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_sendTextEvent(JNIEnv *env, unused jobject thiz, jbyteArray text) {
    if (conn_fd != -1 && text) {
        jsize length = (*env)->GetArrayLength(env, text);
        jbyte *str = (*env)->GetByteArrayElements(env, text, JNI_FALSE);
        char *p = (char*) str;
        mbstate_t state = { 0 };
        log(DEBUG, "Parsing text: %.*s", length, str);

        while (*p) {
            wchar_t wc;
            size_t len = mbrtowc(&wc, p, MB_CUR_MAX, &state);

            if (len == (size_t)-1 || len == (size_t)-2) {
                log(ERROR, "Invalid UTF-8 sequence encountered");
                break;
            }

            if (len == 0)
                break;

            log(DEBUG, "Sending unicode event: %lc (U+%X)", wc, wc);
            lorieEvent e = { .unicode = { .t = EVENT_UNICODE, .code = wc } };
            write(conn_fd, &e, sizeof(e));
            p += len;
            if (p - (char*) str >= length)
                break;
            usleep(30000);
        }

        (*env)->ReleaseByteArrayElements(env, text, str, JNI_ABORT);
        checkConnection(env);
    }
}

JNIEXPORT void JNICALL
Java_com_termux_x11_LorieView_sendUnicodeEvent(JNIEnv *env, unused jobject thiz, jint code) {
    if (conn_fd != -1) {
        log(DEBUG, "Sending unicode event: %lc (U+%X)", code, code);
        lorieEvent e = { .unicode = { .t = EVENT_UNICODE, .code = code } };
        write(conn_fd, &e, sizeof(e));

        checkConnection(env);
    }
}

void abort(void) {
    _exit(134);
}

void exit(int code) {
    _exit(code);
}

#if 1
// It is needed to redirect stderr to logcat
static void* stderrToLogcatThread(unused void* cookie) {
    FILE *fp;
    int p[2];
    size_t len;
    char *line = NULL;
    pipe(p);

    fp = fdopen(p[0], "r");

    dup2(p[1], 2);
    dup2(p[1], 1);
    while ((getline(&line, &len, fp)) != -1) {
        log(DEBUG, "%s%s", line, (line[len - 1] == '\n') ? "" : "\n");
    }

    return NULL;
}

__attribute__((constructor)) static void init(void) {
    pthread_t t;
    if (!strcmp("com.termux.x11", __progname))
        pthread_create(&t, NULL, stderrToLogcatThread, NULL);
}

#endif

