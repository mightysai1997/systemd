/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "efi.h"

#define EFI_CC_MEASUREMENT_PROTOCOL_GUID \
        GUID_DEF(0x96751a3d, 0x72f4, 0x41a6, 0xa7, 0x94, 0xed, 0x5d, 0x0e, 0x67, 0xae, 0x6b)

#define EFI_CC_EVENT_HEADER_VERSION   1
#define EFI_CC_EVENT_LOG_FORMAT_TCG_2   0x00000002

typedef struct {
        uint8_t Type;
        uint8_t SubType;
} EFI_CC_TYPE;

typedef struct {
        uint8_t Major;
        uint8_t Minor;
} EFI_CC_VERSION;

typedef struct {
        uint8_t Size;
        EFI_CC_VERSION StructureVersion;
        EFI_CC_VERSION ProtocolVersion;
        uint32_t HashAlgorithmBitmap;
        uint32_t SupportedEventLogs;
        EFI_CC_TYPE CcType;
} EFI_CC_BOOT_SERVICE_CAPABILITY;

typedef struct {
        uint32_t HeaderSize;
        uint16_t HeaderVersion;
        uint32_t MrIndex;
        uint32_t EventType;
} _packed_ EFI_CC_EVENT_HEADER;

typedef struct {
        uint32_t Size;
        EFI_CC_EVENT_HEADER Header;
        uint8_t Event[1];
} _packed_ EFI_CC_EVENT;

typedef struct EFI_CC_MEASUREMENT_PROTOCOL EFI_CC_MEASUREMENT_PROTOCOL;
struct EFI_CC_MEASUREMENT_PROTOCOL {
        EFI_STATUS (EFIAPI *GetCapability)(
                        EFI_CC_MEASUREMENT_PROTOCOL *This,
                        EFI_CC_BOOT_SERVICE_CAPABILITY *ProtocolCapability);
        void *GetEventLog;
        EFI_STATUS (EFIAPI *HashLogExtendEvent)(
                        EFI_CC_MEASUREMENT_PROTOCOL *This,
                        uint64_t Flags,
                        EFI_PHYSICAL_ADDRESS DataToHash,
                        uint64_t DataToHashLen,
                        EFI_CC_EVENT *EfiCcEvent);
        EFI_STATUS (EFIAPI *MapPcrToMrIndex)(
                        EFI_CC_MEASUREMENT_PROTOCOL *This,
                        uint32_t PcrIndex,
                        uint32_t *MrIndex);
};
