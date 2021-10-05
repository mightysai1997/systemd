/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>

#include "sd-bus.h"

#include "bus-common-errors.h"
#include "bus-error.h"

BUS_ERROR_MAP_ELF_REGISTER const sd_bus_error_map bus_common_errors[] = {
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_UNIT,                 ENOENT),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_UNIT_FOR_PID,              ESRCH),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_UNIT_FOR_INVOCATION_ID,    ENOENT),
        SD_BUS_ERROR_MAP(BUS_ERROR_UNIT_EXISTS,                  EEXIST),
        SD_BUS_ERROR_MAP(BUS_ERROR_LOAD_FAILED,                  EIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_BAD_UNIT_SETTING,             ENOEXEC),
        SD_BUS_ERROR_MAP(BUS_ERROR_JOB_FAILED,                   EREMOTEIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_JOB,                  ENOENT),
        SD_BUS_ERROR_MAP(BUS_ERROR_NOT_SUBSCRIBED,               EINVAL),
        SD_BUS_ERROR_MAP(BUS_ERROR_ALREADY_SUBSCRIBED,           EINVAL),
        SD_BUS_ERROR_MAP(BUS_ERROR_ONLY_BY_DEPENDENCY,           EINVAL),
        SD_BUS_ERROR_MAP(BUS_ERROR_TRANSACTION_JOBS_CONFLICTING, EDEADLK),
        SD_BUS_ERROR_MAP(BUS_ERROR_TRANSACTION_ORDER_IS_CYCLIC,  EDEADLK),
        SD_BUS_ERROR_MAP(BUS_ERROR_TRANSACTION_IS_DESTRUCTIVE,   EDEADLK),
        SD_BUS_ERROR_MAP(BUS_ERROR_UNIT_MASKED,                  ERFKILL),
        SD_BUS_ERROR_MAP(BUS_ERROR_UNIT_GENERATED,               EADDRNOTAVAIL),
        SD_BUS_ERROR_MAP(BUS_ERROR_UNIT_LINKED,                  ELOOP),
        SD_BUS_ERROR_MAP(BUS_ERROR_JOB_TYPE_NOT_APPLICABLE,      EBADR),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_ISOLATION,                 EPERM),
        SD_BUS_ERROR_MAP(BUS_ERROR_SHUTTING_DOWN,                ECANCELED),
        SD_BUS_ERROR_MAP(BUS_ERROR_SCOPE_NOT_RUNNING,            EHOSTDOWN),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_DYNAMIC_USER,         ESRCH),
        SD_BUS_ERROR_MAP(BUS_ERROR_NOT_REFERENCED,               EUNATCH),
        SD_BUS_ERROR_MAP(BUS_ERROR_DISK_FULL,                    ENOSPC),

        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_MACHINE,              ENXIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_IMAGE,                ENOENT),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_MACHINE_FOR_PID,           ENXIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_MACHINE_EXISTS,               EEXIST),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_PRIVATE_NETWORKING,        ENOSYS),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_USER_MAPPING,         ENXIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_GROUP_MAPPING,        ENXIO),

        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_PORTABLE_IMAGE,       ENOENT),
        SD_BUS_ERROR_MAP(BUS_ERROR_BAD_PORTABLE_IMAGE_TYPE,      EMEDIUMTYPE),

        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_SESSION,              ENXIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SESSION_FOR_PID,           ENXIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_USER,                 ENXIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_USER_FOR_PID,              ENXIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_SEAT,                 ENXIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_SESSION_NOT_ON_SEAT,          EINVAL),
        SD_BUS_ERROR_MAP(BUS_ERROR_NOT_IN_CONTROL,               EINVAL),
        SD_BUS_ERROR_MAP(BUS_ERROR_DEVICE_IS_TAKEN,              EINVAL),
        SD_BUS_ERROR_MAP(BUS_ERROR_DEVICE_NOT_TAKEN,             EINVAL),
        SD_BUS_ERROR_MAP(BUS_ERROR_OPERATION_IN_PROGRESS,        EINPROGRESS),
        SD_BUS_ERROR_MAP(BUS_ERROR_SLEEP_VERB_NOT_SUPPORTED,     EOPNOTSUPP),
        SD_BUS_ERROR_MAP(BUS_ERROR_SESSION_BUSY,                 EBUSY),
        SD_BUS_ERROR_MAP(BUS_ERROR_NOT_YOUR_DEVICE,              EPERM),

        SD_BUS_ERROR_MAP(BUS_ERROR_AUTOMATIC_TIME_SYNC_ENABLED,  EALREADY),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_NTP_SUPPORT,               EOPNOTSUPP),

        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_PROCESS,              ESRCH),

        SD_BUS_ERROR_MAP(BUS_ERROR_NO_NAME_SERVERS,              ESRCH),
        SD_BUS_ERROR_MAP(BUS_ERROR_INVALID_REPLY,                EINVAL),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_RR,                   ENOENT),
        SD_BUS_ERROR_MAP(BUS_ERROR_CNAME_LOOP,                   EDEADLK),
        SD_BUS_ERROR_MAP(BUS_ERROR_ABORTED,                      ECANCELED),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_SERVICE,              EUNATCH),
        SD_BUS_ERROR_MAP(BUS_ERROR_DNSSEC_FAILED,                EHOSTUNREACH),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_TRUST_ANCHOR,              EHOSTUNREACH),
        SD_BUS_ERROR_MAP(BUS_ERROR_RR_TYPE_UNSUPPORTED,          EOPNOTSUPP),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_LINK,                 ENXIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_LINK_BUSY,                    EBUSY),
        SD_BUS_ERROR_MAP(BUS_ERROR_NETWORK_DOWN,                 ENETDOWN),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SOURCE,                    ESRCH),
        SD_BUS_ERROR_MAP(BUS_ERROR_STUB_LOOP,                    ELOOP),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_DNSSD_SERVICE,        ENOENT),
        SD_BUS_ERROR_MAP(BUS_ERROR_DNSSD_SERVICE_EXISTS,         EEXIST),

        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "FORMERR",               EBADMSG),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "SERVFAIL",              EHOSTDOWN),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "NXDOMAIN",              ENXIO),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "NOTIMP",                ENOSYS),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "REFUSED",               EACCES),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "YXDOMAIN",              EEXIST),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "YRRSET",                EEXIST),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "NXRRSET",               ENOENT),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "NOTAUTH",               EACCES),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "NOTZONE",               EREMOTE),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "BADVERS",               EBADMSG),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "BADKEY",                EKEYREJECTED),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "BADTIME",               EBADMSG),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "BADMODE",               EBADMSG),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "BADNAME",               EBADMSG),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "BADALG",                EBADMSG),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "BADTRUNC",              EBADMSG),
        SD_BUS_ERROR_MAP(_BUS_ERROR_DNS "BADCOOKIE",             EBADR),

        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_TRANSFER,             ENXIO),
        SD_BUS_ERROR_MAP(BUS_ERROR_TRANSFER_IN_PROGRESS,         EBUSY),

        SD_BUS_ERROR_MAP(BUS_ERROR_NO_PRODUCT_UUID,              EOPNOTSUPP),
        SD_BUS_ERROR_MAP(BUS_ERROR_FILE_IS_PROTECTED,            EACCES),
        SD_BUS_ERROR_MAP(BUS_ERROR_READ_ONLY_FILESYSTEM,         EROFS),

        SD_BUS_ERROR_MAP(BUS_ERROR_SPEED_METER_INACTIVE,         EOPNOTSUPP),
        SD_BUS_ERROR_MAP(BUS_ERROR_UNMANAGED_INTERFACE,          EOPNOTSUPP),

        SD_BUS_ERROR_MAP(BUS_ERROR_NO_SUCH_HOME,                 EEXIST),
        SD_BUS_ERROR_MAP(BUS_ERROR_UID_IN_USE,                   EEXIST),
        SD_BUS_ERROR_MAP(BUS_ERROR_USER_NAME_EXISTS,             EEXIST),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_EXISTS,                  EEXIST),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_ALREADY_ACTIVE,          EALREADY),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_ALREADY_FIXATED,         EALREADY),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_UNFIXATED,               EADDRNOTAVAIL),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_NOT_ACTIVE,              EALREADY),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_ABSENT,                  EREMOTE),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_BUSY,                    EBUSY),
        SD_BUS_ERROR_MAP(BUS_ERROR_BAD_PASSWORD,                 ENOKEY),
        SD_BUS_ERROR_MAP(BUS_ERROR_LOW_PASSWORD_QUALITY,         EUCLEAN),
        SD_BUS_ERROR_MAP(BUS_ERROR_BAD_PASSWORD_AND_NO_TOKEN,    EBADSLT),
        SD_BUS_ERROR_MAP(BUS_ERROR_TOKEN_PIN_NEEDED,             ENOANO),
        SD_BUS_ERROR_MAP(BUS_ERROR_TOKEN_PROTECTED_AUTHENTICATION_PATH_NEEDED, ERFKILL),
        SD_BUS_ERROR_MAP(BUS_ERROR_TOKEN_USER_PRESENCE_NEEDED,   EMEDIUMTYPE),
        SD_BUS_ERROR_MAP(BUS_ERROR_TOKEN_USER_VERIFICATION_NEEDED, ENOCSI),
        SD_BUS_ERROR_MAP(BUS_ERROR_TOKEN_ACTION_TIMEOUT,         ENOSTR),
        SD_BUS_ERROR_MAP(BUS_ERROR_TOKEN_PIN_LOCKED,             EOWNERDEAD),
        SD_BUS_ERROR_MAP(BUS_ERROR_TOKEN_BAD_PIN,                ENOLCK),
        SD_BUS_ERROR_MAP(BUS_ERROR_TOKEN_BAD_PIN_FEW_TRIES_LEFT, ETOOMANYREFS),
        SD_BUS_ERROR_MAP(BUS_ERROR_TOKEN_BAD_PIN_ONE_TRY_LEFT,   EUCLEAN),
        SD_BUS_ERROR_MAP(BUS_ERROR_BAD_SIGNATURE,                EKEYREJECTED),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_RECORD_MISMATCH,         EUCLEAN),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_RECORD_DOWNGRADE,        ESTALE),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_RECORD_SIGNED,           EROFS),
        SD_BUS_ERROR_MAP(BUS_ERROR_BAD_HOME_SIZE,                ERANGE),
        SD_BUS_ERROR_MAP(BUS_ERROR_NO_PRIVATE_KEY,               ENOPKG),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_LOCKED,                  ENOEXEC),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_NOT_LOCKED,              ENOEXEC),
        SD_BUS_ERROR_MAP(BUS_ERROR_TOO_MANY_OPERATIONS,          ENOBUFS),
        SD_BUS_ERROR_MAP(BUS_ERROR_AUTHENTICATION_LIMIT_HIT,     ETOOMANYREFS),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_CANT_AUTHENTICATE,       EKEYREVOKED),
        SD_BUS_ERROR_MAP(BUS_ERROR_HOME_IN_USE,                  EADDRINUSE),

        SD_BUS_ERROR_MAP_END
};
