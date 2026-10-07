#include "napi/native_api.h"
#include "poll.h"
#include "fcntl.h"
#include "unistd.h"
#include "termios.h"
#include "cerrno"
#include "cstring"
#include "dirent.h"
#include "cstdio"
#include "map"
#include "string"
#include "mutex"
#include "thread"
#include "atomic"
#include "vector"

#include "hilog/log.h"

#undef LOG_TAG
#define LOG_TAG "Serial"

// =========================
// 每个已打开串口的上下文
// =========================
struct PortContext {
    int fd = -1;
    std::atomic<bool> reading{false};        // 接收线程是否运行
    std::thread reader;                      // 接收线程
    napi_threadsafe_function tsfn = nullptr; // JS 回调桥梁
    napi_env env = nullptr;                  // 保存 env 用于回调时创建值
};

// =========================
// 全局:端口名 -> PortContext
// =========================
static std::map<std::string, PortContext *> g_ports;
static std::mutex g_portsMutex;

// =========================
// 工具:从 napi_value 取 UTF-8 字符串
// =========================
static bool GetStringArg(napi_env env, napi_value arg, char *out, size_t outSize) {
    if (outSize == 0)
        return false;
    size_t len = 0;
    napi_status st = napi_get_value_string_utf8(env, arg, out, outSize - 1, &len);
    return st == napi_ok;
}

// =========================
// 工具:判断某个 name 是否是串口设备
// =========================
static bool isSerialDevice(const char *name) {
    if (strncmp(name, "ttyS", 4) == 0)
        return true;
    if (strncmp(name, "ttyUSB", 6) == 0)
        return true;
    return false;
}

// =========================
// 工具:根据端口名取 PortContext,失败返回 nullptr
// =========================
static PortContext *GetPortCtx(const std::string &port) {
    std::lock_guard<std::mutex> lock(g_portsMutex);
    auto it = g_ports.find(port);
    if (it == g_ports.end()) {
        return nullptr;
    }
    return it->second;
}

// =========================
// 工具:关闭单个 PortContext(释放线程、tsfn、fd)
// 调用前不要持有 g_portsMutex
// =========================
static void DestroyPortCtx(PortContext *ctx) {
    if (ctx == nullptr)
        return;

    // 停止接收线程
    ctx->reading.store(false);
    if (ctx->reader.joinable()) {
        ctx->reader.join();
    }

    // 释放 threadsafe function
    if (ctx->tsfn != nullptr) {
        napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
        ctx->tsfn = nullptr;
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }

    delete ctx;
}

// =========================
// 列出系统所有串口设备
// =========================
static napi_value ListPorts(napi_env env, napi_callback_info info) {
    napi_value result;
    napi_create_array(env, &result);

    DIR *dir = opendir("/dev");
    if (dir == nullptr) {
        char errMsg[128] = {0};
        snprintf(errMsg, sizeof(errMsg), "opendir /dev failed, errno=%d (%s)", errno, strerror(errno));
        napi_throw_error(env, nullptr, errMsg);
        return nullptr;
    }

    struct dirent *entry = nullptr;
    uint32_t index = 0;

    while ((entry = readdir(dir)) != nullptr) {
        if (!isSerialDevice(entry->d_name)) {
            continue;
        }

        char fullPath[256] = {0};
        snprintf(fullPath, sizeof(fullPath), "/dev/%s", entry->d_name);

        napi_value portName;
        napi_create_string_utf8(env, fullPath, NAPI_AUTO_LENGTH, &portName);
        napi_set_element(env, result, index, portName);
        index++;
    }

    closedir(dir);
    return result;
}

// =========================
// 打开串口
// =========================
static napi_value OpenSerial(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr};

    napi_status status = napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (status != napi_ok || argc < 2) {
        napi_throw_error(env, nullptr, "open() requires port and baudRate");
        return nullptr;
    }

    char port[128] = {0};
    if (!GetStringArg(env, argv[0], port, sizeof(port))) {
        napi_throw_error(env, nullptr, "Invalid serial port path");
        return nullptr;
    }

    int32_t baudRate = 115200;
    status = napi_get_value_int32(env, argv[1], &baudRate);
    if (status != napi_ok) {
        napi_throw_error(env, nullptr, "Invalid baud rate");
        return nullptr;
    }

    std::string portKey(port);

    // 若已打开,先关闭旧的
    {
        std::lock_guard<std::mutex> lock(g_portsMutex);
        auto existing = g_ports.find(portKey);
        if (existing != g_ports.end()) {
            PortContext *old = existing->second;
            g_ports.erase(existing);
            // 解锁前不能 delete,但为了不长时间持锁,先移出 map,释放放锁外
            // 这里简单处理:直接释放(注意 DestroyPortCtx 不碰 g_portsMutex)
            DestroyPortCtx(old);
        }
    }

    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        char errMsg[160] = {0};
        snprintf(errMsg, sizeof(errMsg), "open %s failed, errno=%d (%s)", port, errno, strerror(errno));
        napi_throw_error(env, nullptr, errMsg);
        return nullptr;
    }

    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        napi_throw_error(env, nullptr, "tcgetattr failed");
        return nullptr;
    }

    cfmakeraw(&tty);

    speed_t speed;
    switch (baudRate) {
    case 9600:
        speed = B9600;
        break;
    case 19200:
        speed = B19200;
        break;
    case 38400:
        speed = B38400;
        break;
    case 57600:
        speed = B57600;
        break;
    case 115200:
        speed = B115200;
        break;
    case 230400:
        speed = B230400;
        break;
    case 460800:
        speed = B460800;
        break;
    case 921600:
        speed = B921600;
        break;
    default:
        close(fd);
        napi_throw_error(env, nullptr, "Unsupported baud rate");
        return nullptr;
    }

    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CLOCAL;
    tty.c_cflag |= CREAD;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        napi_throw_error(env, nullptr, "tcsetattr failed");
        return nullptr;
    }

    // 创建 PortContext
    PortContext *ctx = new PortContext();
    ctx->fd = fd;
    ctx->env = env;

    {
        std::lock_guard<std::mutex> lock(g_portsMutex);
        g_ports[portKey] = ctx;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

// =========================
// 发送数据
// =========================
static napi_value SendSerial(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};

    napi_status status = napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (status != napi_ok || argc < 2) {
        OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] send() requires port and data");

        napi_throw_error(env, nullptr, "send() requires port and data");

        return nullptr;
    }

    // =========================
    // 获取串口
    // =========================
    char port[128] = {0};

    if (!GetStringArg(env, argv[0], port, sizeof(port))) {

        OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] Invalid port");

        napi_throw_error(env, nullptr, "Invalid port");

        return nullptr;
    }

    // =========================
    // 获取发送数据
    // =========================
    char buffer[1024] = {0};
    size_t len = 0;

    status = napi_get_value_string_utf8(env, argv[1], buffer, sizeof(buffer) - 1, &len);

    if (status != napi_ok) {

        OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] Invalid send data");

        napi_throw_error(env, nullptr, "Invalid send data");

        return nullptr;
    }

    OH_LOG_INFO(LOG_APP, "[SerialTestcpp] SendSerial port=%{public}s len=%{public}zu", port, len);

    OH_LOG_INFO(LOG_APP, "[SerialTestcpp] Send data=%{public}s", buffer);

    // =========================
    // 打印 HEX
    // =========================
    char hex[2048] = {0};
    size_t hexPos = 0;

    for (size_t i = 0; i < len && hexPos < sizeof(hex) - 4; i++) {

        int written = snprintf(hex + hexPos, sizeof(hex) - hexPos, "%02X ", static_cast<unsigned char>(buffer[i]));

        if (written > 0) {
            hexPos += static_cast<size_t>(written);
        }
    }

    OH_LOG_INFO(LOG_APP, "[SerialTestcpp] Send HEX=%{public}s", hex);

    // =========================
    // 获取串口上下文
    // =========================
    PortContext *ctx = GetPortCtx(std::string(port));

    if (ctx == nullptr) {

        OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] Port not opened: %{public}s", port);

        napi_throw_error(env, nullptr, "Port not opened");

        return nullptr;
    }

    OH_LOG_INFO(LOG_APP, "[SerialTestcpp] fd=%{public}d", ctx->fd);

    if (ctx->fd < 0) {

        OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] Invalid serial fd");

        napi_throw_error(env, nullptr, "Invalid serial fd");

        return nullptr;
    }

    // =========================
    // 开始发送
    // =========================
    ssize_t total = 0;

    while (total < static_cast<ssize_t>(len)) {

        ssize_t n = write(ctx->fd, buffer + total, len - total);

        if (n < 0) {

            int err = errno;

            OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] write failed errno=%{public}d msg=%{public}s", err, strerror(err));

            if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {

                usleep(1000);
                continue;
            }

            napi_throw_error(env, nullptr, strerror(err));

            return nullptr;
        }

        if (n == 0) {

            OH_LOG_WARN(LOG_APP, "[SerialTestcpp] write returned 0");

            break;
        }

        total += n;

        OH_LOG_INFO(LOG_APP, "[SerialTestcpp] write n=%{public}ld total=%{public}ld/%{public}zu", static_cast<long>(n),
                    static_cast<long>(total), len);
    }

    // =========================
    // 等待 UART 发送完成
    // =========================
    int drainRet = tcdrain(ctx->fd);

    if (drainRet != 0) {

        OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] tcdrain failed errno=%{public}d msg=%{public}s", errno, strerror(errno));

    } else {

        OH_LOG_INFO(LOG_APP, "[SerialTestcpp] tcdrain success");
    }

    OH_LOG_INFO(LOG_APP, "[SerialTestcpp] SEND FINISH total=%{public}ld", static_cast<long>(total));

    // =========================
    // 返回发送字节数
    // =========================
    napi_value result;

    napi_create_int32(env, static_cast<int32_t>(total), &result);

    return result;
}

// =========================
// 同步读取一次(非阻塞,有多少读多少)
//
// ArkTS:
// const data = serial.read("/dev/ttyS3");   // 返回 string,无数据返回 ""
// =========================
static napi_value ReadSerial(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};

    napi_status status = napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (status != napi_ok || argc < 1) {
        napi_throw_error(env, nullptr, "read() requires port");
        return nullptr;
    }

    char port[128] = {0};
    if (!GetStringArg(env, argv[0], port, sizeof(port))) {
        napi_throw_error(env, nullptr, "Invalid port");
        return nullptr;
    }

    PortContext *ctx = GetPortCtx(std::string(port));
    if (ctx == nullptr) {
        napi_throw_error(env, nullptr, "Port not opened");
        return nullptr;
    }

    char buf[1024] = {0};
    ssize_t n = read(ctx->fd, buf, sizeof(buf));

    napi_value result;
    if (n > 0) {
        napi_create_string_utf8(env, buf, n, &result);
    } else {
        // 无数据或出错都返回空串
        napi_create_string_utf8(env, "", 0, &result);
    }
    return result;
}

// =========================
// threadsafe function 回调:在 JS 线程执行
// =========================
static void OnDataCallback(napi_env env, napi_value js_cb, void *context, void *data) {
    // data 指向一个动态分配的 std::string*(含本次收到的数据)
    auto *payload = static_cast<std::pair<std::string, std::string> *>(data);
    if (payload == nullptr)
        return;

    const std::string &port = payload->first;
    const std::string &chunk = payload->second;

    if (env != nullptr && js_cb != nullptr) {
        napi_value argv[2];
        napi_create_string_utf8(env, port.c_str(), port.size(), &argv[0]);
        napi_create_string_utf8(env, chunk.c_str(), chunk.size(), &argv[1]);
        napi_call_function(env, nullptr, js_cb, 2, argv, nullptr);
    }

    delete payload;
}

// =========================
// 后台接收线程体
// =========================
static void ReaderLoop(PortContext *ctx, const std::string &port) {
    OH_LOG_INFO(LOG_APP, "[SerialTestcpp] ReaderLoop START port=%{public}s fd=%{public}d", port.c_str(), ctx->fd);

    while (ctx->reading) {

        struct pollfd pfd;
        pfd.fd = ctx->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 200);

        if (ret < 0) {

            if (errno == EINTR) {
                continue;
            }

            OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] poll failed port=%{public}s errno=%{public}d msg=%{public}s", port.c_str(),
                         errno, strerror(errno));

            break;
        }

        if (ret == 0) {
            continue;
        }

        OH_LOG_INFO(LOG_APP, "[SerialTestcpp] poll ret=%{public}d revents=0x%{public}x", ret, pfd.revents);

        if (pfd.revents & POLLIN) {

            char buf[4096] = {0};

            ssize_t n = read(ctx->fd, buf, sizeof(buf));

            if (n > 0) {

                std::string data(buf, static_cast<size_t>(n));

                OH_LOG_INFO(LOG_APP, "[SerialTestcpp] RX port=%{public}s bytes=%{public}ld data=%{public}s", port.c_str(),
                            static_cast<long>(n), data.c_str());

                if (ctx->tsfn != nullptr) {

                    auto *payload = new std::pair<std::string, std::string>(port, data);

                    napi_status status = napi_call_threadsafe_function(ctx->tsfn, payload, napi_tsfn_nonblocking);

                    OH_LOG_INFO(LOG_APP, "[SerialTestcpp] tsfn status=%{public}d", status);

                } else {

                    OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] ERROR tsfn == nullptr");
                }

            } else if (n < 0) {

                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {

                    continue;
                }

                OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] read failed errno=%{public}d msg=%{public}s", errno, strerror(errno));

                break;

            } else {

                OH_LOG_WARN(LOG_APP, "[SerialTestcpp] read returned 0");
            }
        }

        if (pfd.revents & POLLERR) {

            OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] POLLERR");

            break;
        }

        if (pfd.revents & POLLHUP) {

            OH_LOG_WARN(LOG_APP, "[SerialTestcpp] POLLHUP");

            break;
        }

        if (pfd.revents & POLLNVAL) {

            OH_LOG_ERROR(LOG_APP, "[SerialTestcpp] POLLNVAL");

            break;
        }
    }

    OH_LOG_INFO(LOG_APP, "[SerialTestcpp] ReaderLoop STOP port=%{public}s", port.c_str());
}
// =========================
// 注册数据到达回调
//
// ArkTS:
// serial.onData("/dev/ttyS3", (port, data) => { ... })
// =========================
static napi_value OnDataSerial(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr};

    napi_status status = napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (status != napi_ok || argc < 2) {
        napi_throw_error(env, nullptr, "onData() requires port and callback");
        return nullptr;
    }

    char port[128] = {0};
    if (!GetStringArg(env, argv[0], port, sizeof(port))) {
        napi_throw_error(env, nullptr, "Invalid port");
        return nullptr;
    }

    // 检查第二个参数是函数
    napi_valuetype vt;
    napi_typeof(env, argv[1], &vt);
    if (vt != napi_function) {
        napi_throw_error(env, nullptr, "Second argument must be a function");
        return nullptr;
    }

    PortContext *ctx = GetPortCtx(std::string(port));
    if (ctx == nullptr) {
        napi_throw_error(env, nullptr, "Port not opened");
        return nullptr;
    }

    // 如果已存在旧的 tsfn,先释放
    if (ctx->tsfn != nullptr) {
        napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
        ctx->tsfn = nullptr;
    }

    // 保存 port 名用于回调传参
    static std::map<std::string, std::string> *portNames = nullptr;
    if (portNames == nullptr) {
        portNames = new std::map<std::string, std::string>();
    }
    (*portNames)[std::string(port)] = std::string(port);

    napi_value resourceName;
    napi_create_string_utf8(env, "SerialOnData", NAPI_AUTO_LENGTH, &resourceName);

    napi_status st = napi_create_threadsafe_function(env,
                                                     argv[1],        // JS 回调函数
                                                     nullptr,        // async_resource
                                                     resourceName,   // 资源名
                                                     0,              // max_queue_size,0 = 无限制
                                                     1,              // initial_thread_count
                                                     nullptr,        // thread_finalize_data
                                                     nullptr,        // thread_finalize_cb
                                                     nullptr,        // context
                                                     OnDataCallback, // call_js_cb
                                                     &ctx->tsfn);

    if (st != napi_ok || ctx->tsfn == nullptr) {
        napi_throw_error(env, nullptr, "Failed to create threadsafe function");
        return nullptr;
    }

    // 启动接收线程(若尚未运行)
    if (!ctx->reading.load()) {
        ctx->reading.store(true);
        std::string portName(port);
        ctx->reader = std::thread(ReaderLoop, ctx, portName);
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

// =========================
// 取消数据到达回调
//
// ArkTS:
// serial.offData("/dev/ttyS3")
// =========================
static napi_value OffDataSerial(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};

    napi_status status = napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (status != napi_ok || argc < 1) {
        napi_throw_error(env, nullptr, "offData() requires port");
        return nullptr;
    }

    char port[128] = {0};
    if (!GetStringArg(env, argv[0], port, sizeof(port))) {
        napi_throw_error(env, nullptr, "Invalid port");
        return nullptr;
    }

    PortContext *ctx = GetPortCtx(std::string(port));
    if (ctx != nullptr) {
        ctx->reading.store(false);
        if (ctx->reader.joinable()) {
            ctx->reader.join();
        }
        if (ctx->tsfn != nullptr) {
            napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
            ctx->tsfn = nullptr;
        }
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

// =========================
// 关闭指定串口
// =========================
static napi_value CloseSerial(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};

    napi_status status = napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (status != napi_ok || argc < 1) {
        napi_throw_error(env, nullptr, "close() requires port");
        return nullptr;
    }

    char port[128] = {0};
    if (!GetStringArg(env, argv[0], port, sizeof(port))) {
        napi_throw_error(env, nullptr, "Invalid port");
        return nullptr;
    }

    PortContext *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_portsMutex);
        auto it = g_ports.find(std::string(port));
        if (it != g_ports.end()) {
            ctx = it->second;
            g_ports.erase(it);
        }
    }

    // 锁外释放,避免长时间持锁
    DestroyPortCtx(ctx);

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

// =========================
// 关闭全部串口
// =========================
static napi_value CloseAllSerial(napi_env env, napi_callback_info info) {
    std::vector<PortContext *> toDestroy;
    {
        std::lock_guard<std::mutex> lock(g_portsMutex);
        for (auto &kv : g_ports) {
            toDestroy.push_back(kv.second);
        }
        g_ports.clear();
    }
    for (auto *ctx : toDestroy) {
        DestroyPortCtx(ctx);
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

// =========================
// 查询是否打开
// =========================
static napi_value IsOpenSerial(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};

    napi_status status = napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (status != napi_ok || argc < 1) {
        napi_throw_error(env, nullptr, "isOpen() requires port");
        return nullptr;
    }

    char port[128] = {0};
    if (!GetStringArg(env, argv[0], port, sizeof(port))) {
        napi_throw_error(env, nullptr, "Invalid port");
        return nullptr;
    }

    bool opened = false;
    {
        std::lock_guard<std::mutex> lock(g_portsMutex);
        opened = g_ports.find(std::string(port)) != g_ports.end();
    }

    napi_value result;
    napi_get_boolean(env, opened, &result);
    return result;
}

// =========================
// NAPI 模块初始化
// =========================
EXTERN_C_START

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"open", nullptr, OpenSerial, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"send", nullptr, SendSerial, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"read", nullptr, ReadSerial, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"onData", nullptr, OnDataSerial, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"offData", nullptr, OffDataSerial, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"close", nullptr, CloseSerial, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"closeAll", nullptr, CloseAllSerial, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isOpen", nullptr, IsOpenSerial, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"listPorts", nullptr, ListPorts, nullptr, nullptr, nullptr, napi_default, nullptr},
    };

    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

EXTERN_C_END

// =========================
// NAPI 模块信息
// =========================
static napi_module serialModule = {.nm_version = 1,
                                   .nm_flags = 0,
                                   .nm_filename = nullptr,
                                   .nm_register_func = Init,
                                   .nm_modname = "serial",
                                   .nm_priv = nullptr,
                                   .reserved = {0}};

extern "C" __attribute__((constructor)) void RegisterSerialModule(void) { napi_module_register(&serialModule); }