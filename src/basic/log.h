/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "macro.h"
#include "ratelimit.h"

/* Some structures we reference but don't want to pull in headers for */
struct iovec;
struct signalfd_siginfo;

typedef enum LogTarget{
        LOG_TARGET_CONSOLE,
        LOG_TARGET_CONSOLE_PREFIXED,
        LOG_TARGET_KMSG,
        LOG_TARGET_JOURNAL,
        LOG_TARGET_JOURNAL_OR_KMSG,
        LOG_TARGET_SYSLOG,
        LOG_TARGET_SYSLOG_OR_KMSG,
        LOG_TARGET_AUTO, /* console if stderr is not journal, JOURNAL_OR_KMSG otherwise */
        LOG_TARGET_NULL,
        _LOG_TARGET_MAX,
        _LOG_TARGET_INVALID = -EINVAL,
} LogTarget;

/* This log level disables logging completely. It can only be passed to log_set_max_level() and cannot be
 * used a regular log level. */
#define LOG_NULL (LOG_EMERG - 1)

/* Note to readers: << and >> have lower precedence (are evaluated earlier) than & and | */
#define SYNTHETIC_ERRNO(num)                (1 << 30 | (num))
#define IS_SYNTHETIC_ERRNO(val)             ((val) >> 30 & 1)
#define ERRNO_VALUE(val)                    (abs(val) & ~(1 << 30))

/* The callback function to be invoked when syntax warnings are seen
 * in the unit files. */
typedef void (*log_syntax_callback_t)(const char *unit, int level, void *userdata);
void set_log_syntax_callback(log_syntax_callback_t cb, void *userdata);

static inline void clear_log_syntax_callback(dummy_t *dummy) {
          set_log_syntax_callback(/* cb= */ NULL, /* userdata= */ NULL);
}

const char *log_target_to_string(LogTarget target) _const_;
LogTarget log_target_from_string(const char *s) _pure_;
void log_set_target(LogTarget target);
int log_set_target_from_string(const char *e);
LogTarget log_get_target(void) _pure_;

void log_set_max_level(int level);
int log_set_max_level_from_string(const char *e);
int log_get_max_level(void) _pure_;

void log_set_facility(int facility);

void log_show_color(bool b);
bool log_get_show_color(void) _pure_;
void log_show_location(bool b);
bool log_get_show_location(void) _pure_;
void log_show_time(bool b);
bool log_get_show_time(void) _pure_;
void log_show_tid(bool b);
bool log_get_show_tid(void) _pure_;

int log_show_color_from_string(const char *e);
int log_show_location_from_string(const char *e);
int log_show_time_from_string(const char *e);
int log_show_tid_from_string(const char *e);

/* Functions below that open and close logs or configure logging based on the
 * environment should not be called from library code — this is always a job
 * for the application itself. */

assert_cc(STRLEN(__FILE__) > STRLEN(RELATIVE_SOURCE_PATH) + 1);
#define PROJECT_FILE (&__FILE__[STRLEN(RELATIVE_SOURCE_PATH) + 1])

int log_open(void);
void log_close(void);
void log_forget_fds(void);

void log_parse_environment_variables(void);
void log_parse_environment(void);

int log_dispatch_internal(
                int level,
                int error,
                const char *file,
                int line,
                const char *func,
                const char *object_field,
                const char *object,
                const char *extra,
                const char *extra_field,
                char *buffer);

int log_internal(
                int level,
                int error,
                const char *file,
                int line,
                const char *func,
                const char *format, ...) _printf_(6,7);

int log_internalv(
                int level,
                int error,
                const char *file,
                int line,
                const char *func,
                const char *format,
                va_list ap) _printf_(6,0);

int log_object_internalv(
                int level,
                int error,
                const char *file,
                int line,
                const char *func,
                const char *object_field,
                const char *object,
                const char *extra_field,
                const char *extra,
                const char *format,
                va_list ap) _printf_(10,0);

int log_object_internal(
                int level,
                int error,
                const char *file,
                int line,
                const char *func,
                const char *object_field,
                const char *object,
                const char *extra_field,
                const char *extra,
                const char *format, ...) _printf_(10,11);

int log_struct_internal(
                int level,
                int error,
                const char *file,
                int line,
                const char *func,
                const char *format, ...) _printf_(6,0) _sentinel_;

int log_oom_internal(
                int level,
                const char *file,
                int line,
                const char *func);

int log_format_iovec(
                struct iovec *iovec,
                size_t iovec_len,
                size_t *n,
                bool newline_separator,
                int error,
                const char *format,
                va_list ap) _printf_(6, 0);

int log_struct_iovec_internal(
                int level,
                int error,
                const char *file,
                int line,
                const char *func,
                const struct iovec *input_iovec,
                size_t n_input_iovec);

/* This modifies the buffer passed! */
int log_dump_internal(
                int level,
                int error,
                const char *file,
                int line,
                const char *func,
                char *buffer);

/* Logging for various assertions */
_noreturn_ void log_assert_failed(
                const char *text,
                const char *file,
                int line,
                const char *func);

_noreturn_ void log_assert_failed_unreachable(
                const char *file,
                int line,
                const char *func);

void log_assert_failed_return(
                const char *text,
                const char *file,
                int line,
                const char *func);

#define log_dispatch(level, error, buffer)                              \
        log_dispatch_internal(level, error, PROJECT_FILE, __LINE__, __func__, NULL, NULL, NULL, NULL, buffer)

/* Logging with level */
#define log_full_errno_zerook(level, error, ...)                        \
        ({                                                              \
                int _level = (level), _e = (error);                     \
                _e = (log_get_max_level() >= LOG_PRI(_level))           \
                        ? log_internal(_level, _e, PROJECT_FILE, __LINE__, __func__, __VA_ARGS__) \
                        : -ERRNO_VALUE(_e);                             \
                _e < 0 ? _e : -ESTRPIPE;                                \
        })

#if BUILD_MODE_DEVELOPER && !defined(TEST_CODE)
#  define ASSERT_NON_ZERO(x) assert((x) != 0)
#else
#  define ASSERT_NON_ZERO(x)
#endif

#define log_full_errno(level, error, ...)                               \
        ({                                                              \
                int _error = (error);                                   \
                ASSERT_NON_ZERO(_error);                                \
                log_full_errno_zerook(level, _error, __VA_ARGS__);      \
        })

#define log_full(level, fmt, ...)                                      \
        ({                                                             \
                if (BUILD_MODE_DEVELOPER)                              \
                        assert(!strstr(fmt, "%m"));                    \
                (void) log_full_errno_zerook(level, 0, fmt, ##__VA_ARGS__); \
        })

int log_emergency_level(void);

/* Normal logging */
#define log_debug(...)     log_full(LOG_DEBUG,   __VA_ARGS__)
#define log_info(...)      log_full(LOG_INFO,    __VA_ARGS__)
#define log_notice(...)    log_full(LOG_NOTICE,  __VA_ARGS__)
#define log_warning(...)   log_full(LOG_WARNING, __VA_ARGS__)
#define log_error(...)     log_full(LOG_ERR,     __VA_ARGS__)
#define log_emergency(...) log_full(log_emergency_level(), __VA_ARGS__)

/* Logging triggered by an errno-like error */
#define log_debug_errno(error, ...)     log_full_errno(LOG_DEBUG,   error, __VA_ARGS__)
#define log_info_errno(error, ...)      log_full_errno(LOG_INFO,    error, __VA_ARGS__)
#define log_notice_errno(error, ...)    log_full_errno(LOG_NOTICE,  error, __VA_ARGS__)
#define log_warning_errno(error, ...)   log_full_errno(LOG_WARNING, error, __VA_ARGS__)
#define log_error_errno(error, ...)     log_full_errno(LOG_ERR,     error, __VA_ARGS__)
#define log_emergency_errno(error, ...) log_full_errno(log_emergency_level(), error, __VA_ARGS__)

/* This logs at the specified level the first time it is called, and then
 * logs at debug. If the specified level is debug, this logs only the first
 * time it is called. */
#define log_once(level, ...)                                             \
        ({                                                               \
                if (ONCE)                                                \
                        log_full(level, __VA_ARGS__);                    \
                else if (LOG_PRI(level) != LOG_DEBUG)                    \
                        log_debug(__VA_ARGS__);                          \
        })

#define log_once_errno(level, error, ...)                                \
        ({                                                               \
                int _err = (error);                                      \
                if (ONCE)                                                \
                        _err = log_full_errno(level, _err, __VA_ARGS__); \
                else if (LOG_PRI(level) != LOG_DEBUG)                    \
                        _err = log_debug_errno(_err, __VA_ARGS__);       \
                else                                                     \
                        _err = -ERRNO_VALUE(_err);                       \
                _err;                                                    \
        })

#if LOG_TRACE
#  define log_trace(...)          log_debug(__VA_ARGS__)
#  define log_trace_errno(...)    log_debug_errno(__VA_ARGS__)
#else
#  define log_trace(...)          do {} while (0)
#  define log_trace_errno(e, ...) (-ERRNO_VALUE(e))
#endif

/* Structured logging */
#define log_struct_errno(level, error, ...)                             \
        log_struct_internal(level, error, PROJECT_FILE, __LINE__, __func__, __VA_ARGS__, NULL)
#define log_struct(level, ...) log_struct_errno(level, 0, __VA_ARGS__)

#define log_struct_iovec_errno(level, error, iovec, n_iovec)            \
        log_struct_iovec_internal(level, error, PROJECT_FILE, __LINE__, __func__, iovec, n_iovec)
#define log_struct_iovec(level, iovec, n_iovec) log_struct_iovec_errno(level, 0, iovec, n_iovec)

/* This modifies the buffer passed! */
#define log_dump(level, buffer)                                         \
        log_dump_internal(level, 0, PROJECT_FILE, __LINE__, __func__, buffer)

#define log_oom() log_oom_internal(LOG_ERR, PROJECT_FILE, __LINE__, __func__)
#define log_oom_debug() log_oom_internal(LOG_DEBUG, PROJECT_FILE, __LINE__, __func__)

bool log_on_console(void) _pure_;

/* Helper to wrap the main message in structured logging. The macro doesn't do much,
 * except to provide visual grouping of the format string and its arguments. */
#if LOG_MESSAGE_VERIFICATION || defined(__COVERITY__)
/* Do a fake formatting of the message string to let the scanner verify the arguments against the format
 * message. The variable will never be set to true, but we don't tell the compiler that :) */
extern bool _log_message_dummy;
#  define LOG_MESSAGE(fmt, ...) "MESSAGE=%.0d" fmt, (_log_message_dummy && printf(fmt, ##__VA_ARGS__)), ##__VA_ARGS__
#else
#  define LOG_MESSAGE(fmt, ...) "MESSAGE=" fmt, ##__VA_ARGS__
#endif

void log_received_signal(int level, const struct signalfd_siginfo *si);

/* If turned on, any requests for a log target involving "syslog" will be implicitly upgraded to the equivalent journal target */
void log_set_upgrade_syslog_to_journal(bool b);

/* If turned on, and log_open() is called, we'll not use STDERR_FILENO for logging ever, but rather open /dev/console */
void log_set_always_reopen_console(bool b);

/* If turned on, we'll open the log stream implicitly if needed on each individual log call. This is normally not
 * desired as we want to reuse our logging streams. It is useful however  */
void log_set_open_when_needed(bool b);

/* If turned on, then we'll never use IPC-based logging, i.e. never log to syslog or the journal. We'll only log to
 * stderr, the console or kmsg */
void log_set_prohibit_ipc(bool b);

int log_dup_console(void);

int log_syntax_internal(
                const char *unit,
                int level,
                const char *config_file,
                unsigned config_line,
                int error,
                const char *file,
                int line,
                const char *func,
                const char *format, ...) _printf_(9, 10);

int log_syntax_invalid_utf8_internal(
                const char *unit,
                int level,
                const char *config_file,
                unsigned config_line,
                const char *file,
                int line,
                const char *func,
                const char *rvalue);

#define log_syntax(unit, level, config_file, config_line, error, ...)   \
        ({                                                              \
                int _level = (level), _e = (error);                     \
                (log_get_max_level() >= LOG_PRI(_level))                \
                        ? log_syntax_internal(unit, _level, config_file, config_line, _e, PROJECT_FILE, __LINE__, __func__, __VA_ARGS__) \
                        : -ERRNO_VALUE(_e);                             \
        })

#define log_syntax_invalid_utf8(unit, level, config_file, config_line, rvalue) \
        ({                                                              \
                int _level = (level);                                   \
                (log_get_max_level() >= LOG_PRI(_level))                \
                        ? log_syntax_invalid_utf8_internal(unit, _level, config_file, config_line, PROJECT_FILE, __LINE__, __func__, rvalue) \
                        : -EINVAL;                                      \
        })

#define DEBUG_LOGGING _unlikely_(log_get_max_level() >= LOG_DEBUG)

void log_setup(void);

typedef struct LogRateLimit {
        int error;
        int level;
        RateLimit ratelimit;
} LogRateLimit;

#define log_ratelimit_internal(_level, _error, _ratelimit, _format, _file, _line, _func, ...)        \
({                                                                              \
        int _log_ratelimit_error = (_error);                                    \
        int _log_ratelimit_level = (_level);                                    \
        static LogRateLimit _log_ratelimit = {                                  \
                .ratelimit = (_ratelimit),                                      \
        };                                                                      \
        unsigned _num_dropped_errors = ratelimit_num_dropped(&_log_ratelimit.ratelimit); \
        if (_log_ratelimit_error != _log_ratelimit.error || _log_ratelimit_level != _log_ratelimit.level) { \
                ratelimit_reset(&_log_ratelimit.ratelimit);                     \
                _log_ratelimit.error = _log_ratelimit_error;                    \
                _log_ratelimit.level = _log_ratelimit_level;                    \
        }                                                                       \
        if (log_get_max_level() == LOG_DEBUG || ratelimit_below(&_log_ratelimit.ratelimit)) \
                _log_ratelimit_error = _num_dropped_errors > 0                  \
                ? log_internal(_log_ratelimit_level, _log_ratelimit_error, _file, _line, _func, _format " (Dropped %u similar message(s))", ##__VA_ARGS__, _num_dropped_errors) \
                : log_internal(_log_ratelimit_level, _log_ratelimit_error, _file, _line, _func, _format, ##__VA_ARGS__); \
        _log_ratelimit_error;                                                   \
})

#define log_ratelimit_full_errno(level, error, _ratelimit, format, ...)             \
        ({                                                              \
                int _level = (level), _e = (error);                     \
                _e = (log_get_max_level() >= LOG_PRI(_level))           \
                        ? log_ratelimit_internal(_level, _e, _ratelimit, format, PROJECT_FILE, __LINE__, __func__, ##__VA_ARGS__) \
                        : -ERRNO_VALUE(_e);                             \
                _e < 0 ? _e : -ESTRPIPE;                                \
        })

#define log_ratelimit_full(level, _ratelimit, format, ...)                          \
        log_ratelimit_full_errno(level, 0, _ratelimit, format, ##__VA_ARGS__)

/* Normal logging */
#define log_ratelimit_debug(...)     log_ratelimit_full(LOG_DEBUG,   __VA_ARGS__)
#define log_ratelimit_info(...)      log_ratelimit_full(LOG_INFO,    __VA_ARGS__)
#define log_ratelimit_notice(...)    log_ratelimit_full(LOG_NOTICE,  __VA_ARGS__)
#define log_ratelimit_warning(...)   log_ratelimit_full(LOG_WARNING, __VA_ARGS__)
#define log_ratelimit_error(...)     log_ratelimit_full(LOG_ERR,     __VA_ARGS__)
#define log_ratelimit_emergency(...) log_ratelimit_full(log_emergency_level(), __VA_ARGS__)

/* Logging triggered by an errno-like error */
#define log_ratelimit_debug_errno(error, ...)     log_ratelimit_full_errno(LOG_DEBUG,   error, __VA_ARGS__)
#define log_ratelimit_info_errno(error, ...)      log_ratelimit_full_errno(LOG_INFO,    error, __VA_ARGS__)
#define log_ratelimit_notice_errno(error, ...)    log_ratelimit_full_errno(LOG_NOTICE,  error, __VA_ARGS__)
#define log_ratelimit_warning_errno(error, ...)   log_ratelimit_full_errno(LOG_WARNING, error, __VA_ARGS__)
#define log_ratelimit_error_errno(error, ...)     log_ratelimit_full_errno(LOG_ERR,     error, __VA_ARGS__)
#define log_ratelimit_emergency_errno(error, ...) log_ratelimit_full_errno(log_emergency_level(), error, __VA_ARGS__)
