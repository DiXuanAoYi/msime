#pragma once
/* 输入引擎的 C 接口。
 *
 * 这是引擎对外的“最薄一层”：Rust、Swift、Python 或别的语言的前端都只需要认这一个头文件，
 * 不用碰 C++ 的类型。它包的是 include/metasequoia/session.h 的 Session，一个会话一个句柄，
 * 同一个句柄的调用由宿主串行化，不同句柄可以并发。
 *
 * 约定：
 *   - 字符串都是 UTF-8、以 0 结尾。返回的 `const char *` 归会话所有，到下一次对同一句柄的调用前有效。
 *   - 返回 int 的函数：1 表示按键被引擎处理（宿主应吞掉这个键），0 表示没处理（宿主放行给应用）。
 *   - 上屏文字用 msime_commit_text 取；快照用 msime_snapshot_json 取。
 *   - 接口只增不改；加字段进 JSON，不改已有字段的含义。版本见 msime_abi_version。
 *   - msime_options 只在末尾加字段，调用方要先整体清零（`msime_options options = {0}`）。
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(_WIN32)
#define MSIME_API __declspec(dllexport)
#else
#define MSIME_API __attribute__((visibility("default")))
#endif

    typedef struct msime_session msime_session;

    typedef struct msime_options
    {
        const char *resources_dir; /* 只读资源（词库、模型），必填 */
        const char *user_data_dir; /* 用户词库、学习结果，必填 */
        const char *cache_dir;     /* 缓存，必填 */
        const char *scheme;        /* "quanpin"（默认）、"shuangpin"、"wubi"、"japanese"；NULL 视为 quanpin */
        int helpcode;              /* 辅助码，0 关 1 开 */
        int learning;              /* 候选学习，0 关 1 开 */
        /* 以下两项是 ABI 版本 2 加的，放在末尾；按版本 1 编译的调用方不会设置它们，必须先整体清零。 */
        unsigned fuzzy_rules;      /* 模糊音位掩码，见 core/fuzzy_pinyin_options.h，0 为精确匹配 */
        unsigned autocorrect_types; /* 全拼纠错类型位掩码，见 quanpin/quanpin_utils.h，0 关 */
    } msime_options;

    enum msime_command
    {
        MSIME_CMD_BACKSPACE = 0,
        MSIME_CMD_COMMIT_CANDIDATE = 1,
        MSIME_CMD_COMMIT_RAW = 2,
        MSIME_CMD_CANCEL = 3,
        MSIME_CMD_MOVE_LEFT = 4,
        MSIME_CMD_MOVE_RIGHT = 5,
        MSIME_CMD_MOVE_HOME = 6,
        MSIME_CMD_MOVE_END = 7,
        MSIME_CMD_DELETE_FORWARD = 8
    };

    MSIME_API int msime_abi_version(void);

    /* 失败返回 NULL，原因写进 error（最多 error_len 字节，含结尾 0）。 */
    MSIME_API msime_session *msime_session_create(const msime_options *options, char *error, size_t error_len);
    MSIME_API void msime_session_destroy(msime_session *session);

    MSIME_API int msime_character(msime_session *session, char value, int shift_only);
    MSIME_API int msime_command(msime_session *session, int command);
    MSIME_API int msime_candidate_key(msime_session *session, char value);
    MSIME_API int msime_punctuation(msime_session *session, char value);
    MSIME_API int msime_select(msime_session *session, size_t index);
    MSIME_API int msime_finish(msime_session *session);

    /* 最近一次调用要上屏的文字；没有就是空串。 */
    MSIME_API const char *msime_commit_text(const msime_session *session);

    /* 当前组合状态：
     * {"scheme":"quanpin","preedit":"...","raw":"...","normalized":"...","caret":3,
     *  "candidates":[{"text":"你好","pinyin":"ni'hao","weight":1234,"fixed":0,"fuzzy":false}, ...]} */
    MSIME_API const char *msime_snapshot_json(msime_session *session);

#ifdef __cplusplus
}
#endif
