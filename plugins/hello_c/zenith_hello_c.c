/* ===========================================================================
 * hello_c —— 纯 C 示例插件（用 C 编译器编译）
 * 验证 Zenith C ABI 对纯 C 插件的完整可用性：
 *   宿主日志 / 配置读取 / 服务注册（C 函数表 vtable 形态）
 * 本文件不包含任何 C++ 头（greeter.hpp 的 C 部分可安全在 C 模式使用）。
 * =========================================================================== */
#include <stdio.h>
#include <string.h>

#include "zenith/abi/zenith_abi.h"
#include "greeter.hpp" /* 只使用其中的纯 C 部分 */

static const zenith_host_api* g_host = NULL;
static char g_prefix[64] = "Bonjour";

typedef struct {
    zenith_c_greeter base; /* vt 指针必须位于首部 */
} CGreeterImpl;

static CGreeterImpl g_greeter;

static const char* cg_greet(void* self, const char* name, char* buf, size_t cap)
{
    (void)self;
    if (name == NULL) name = "world";
    if (snprintf(buf, cap, "%s, %s! (from hello_c plugin, pure C)", g_prefix, name) < 0) {
        return NULL;
    }
    return buf;
}

static const zenith_c_greeter_vt g_greeter_vt = { &cg_greet };

static zenith_status plugin_init(const zenith_host_api* host)
{
    const char* prefix = NULL;
    g_host = host;

    host->log_write(host->log_ctx, ZENITH_LOG_INFO, __FILE__, __LINE__,
                    "hello_c: init (compiled as pure C, abi checked by host)");

    /* 演示读取宿主配置；未配置时回落默认值 */
    if (host->config_get(host->config_ctx, "hello_c.greeting_prefix", "Bonjour", &prefix)
            == ZENITH_OK && prefix != NULL) {
        strncpy(g_prefix, prefix, sizeof(g_prefix) - 1);
        g_prefix[sizeof(g_prefix) - 1] = '\0';
    }

    g_greeter.base.vt = &g_greeter_vt;
    /* 静态存储实例：无需 destroy 回调 */
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  ZENITH_C_GREETER_NAME, ZENITH_C_GREETER_VERSION,
                                  &g_greeter, NULL);
}

static zenith_status plugin_start(void)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "hello_c: start (prefix = %s)", g_prefix);
    g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, __FILE__, __LINE__, msg);
    return ZENITH_OK;
}

static zenith_status plugin_stop(void)
{
    g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, __FILE__, __LINE__,
                      "hello_c: stop");
    return ZENITH_OK;
}

static void plugin_shutdown(void)
{
    g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, __FILE__, __LINE__,
                      "hello_c: shutdown");
    g_host = NULL;
}

static zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "hello_c",
    "0.1.0",
    "Zenith pure-C sample plugin: registers a CGreeter vtable service",
    "Zenith Authors",
    &plugin_init,
    &plugin_start,
    &plugin_stop,
    &plugin_shutdown,
    { NULL, NULL, NULL, NULL },
};

ZENITH_EXPORT const zenith_plugin_desc* zenith_plugin_entry(void)
{
    return &g_desc;
}
