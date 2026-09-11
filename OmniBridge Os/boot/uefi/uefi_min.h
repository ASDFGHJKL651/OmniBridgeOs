#ifndef OMNIBRIDGE_UEFI_MIN_H
#define OMNIBRIDGE_UEFI_MIN_H

#include <stdint.h>
#include <stddef.h>

typedef void *EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef uint16_t CHAR16;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;
typedef uint64_t UINTN;
typedef int64_t  INTN;

struct EFI_SYSTEM_TABLE;
struct EFI_BOOT_SERVICES;

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS           0
#define EFI_LOAD_ERROR        1
#define EFI_INVALID_PARAMETER 2
#define EFI_BUFFER_TOO_SMALL  5
#define EFI_OUT_OF_RESOURCES  9

/* EFI memory types */
#define EfiReservedMemoryType      0
#define EfiLoaderCode              1
#define EfiLoaderData              2
#define EfiBootServicesCode        3
#define EfiBootServicesData        4
#define EfiRuntimeServicesCode     5
#define EfiRuntimeServicesData     6
#define EfiConventionalMemory      7
#define EfiUnusableMemory          8
#define EfiACPIReclaimMemory       9
#define EfiACPIMemoryNVS          10
#define EfiMemoryMappedIO         11
#define EfiMemoryMappedIOPortSpace 12
#define EfiPalCode                13
#define EfiPersistentMemory       14
#define EfiMaxMemoryType          15

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;                      /* 0x00 */
    void *RaiseTPL;                            /* 0x18 */
    void *RestoreTPL;                          /* 0x20 */
    void *AllocatePages;                       /* 0x28 */
    void *FreePages;                           /* 0x30 */
    EFI_STATUS (EFIAPI *GetMemoryMap)(         /* 0x38 */
        UINTN *MemoryMapSize,
        EFI_MEMORY_DESCRIPTOR *MemoryMap,
        UINTN *MapKey,
        UINTN *DescriptorSize,
        uint32_t *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(         /* 0x40 */
        UINTN PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);/* 0x48 */
    void *CreateEvent;                         /* 0x50 */
    void *SetTimer;                            /* 0x58 */
    void *WaitForEvent;                        /* 0x60 */
    void *SignalEvent;                         /* 0x68 */
    void *CloseEvent;                          /* 0x70 */
    void *CheckEvent;                          /* 0x78 */
    EFI_STATUS (EFIAPI *InstallProtocolInterface)( /* 0x80 */
        EFI_HANDLE *Handle, void *Protocol, uint32_t InterfaceType, void *Interface);
    void *ReinstallProtocolInterface;          /* 0x88 */
    void *UninstallProtocolInterface;          /* 0x90 */
    EFI_STATUS (EFIAPI *HandleProtocol)(       /* 0x98 */
        EFI_HANDLE Handle, void *Protocol, void **Interface);
    void *Reserved;                            /* 0xA0 */
    void *RegisterProtocolNotify;              /* 0xA8 */
    void *LocateHandle;                        /* 0xB0 */
    void *LocateDevicePath;                    /* 0xB8 */
    void *InstallConfigurationTable;           /* 0xC0 */
    void *LoadImage;                           /* 0xC8 */
    void *StartImage;                          /* 0xD0 */
    void *Exit;                                /* 0xD8 */
    void *UnloadImage;                         /* 0xE0 */
    EFI_STATUS (EFIAPI *ExitBootServices)(     /* 0xE8 */
        EFI_HANDLE ImageHandle, UINTN MapKey);
    /* ---------- 以下为本次新增，用于 OpenProtocol / LocateProtocol ---------- */
    void *GetNextMonotonicCount;               /* 0xF0 */
    void *Stall;                               /* 0xF8 */
    void *SetWatchdogTimer;                    /* 0x100 */
    void *ConnectController;                   /* 0x108 */
    void *DisconnectController;                /* 0x110 */
    EFI_STATUS (EFIAPI *OpenProtocol)(         /* 0x118 */
        EFI_HANDLE Handle, void *Protocol,
        void **Interface, EFI_HANDLE AgentHandle,
        EFI_HANDLE ControllerHandle, uint32_t Attributes);
    void *CloseProtocol;                       /* 0x120 */
    void *OpenProtocolInformation;             /* 0x128 */
    void *ProtocolsPerHandle;                  /* 0x130 */
    void *LocateHandleBuffer;                  /* 0x138 */
    EFI_STATUS (EFIAPI *LocateProtocol)(       /* 0x140 */
        void *Protocol, void *Registration, void **Interface);
    void *InstallMultipleProtocolInterfaces;   /* 0x148 */
    void *UninstallMultipleProtocolInterfaces; /* 0x150 */
    EFI_STATUS (EFIAPI *CalculateCrc32)(       /* 0x158 */
        void *Data, UINTN DataSize, uint32_t *Crc32);
    void *CopyMem;                             /* 0x160 */
    void *SetMem;                              /* 0x168 */
    void *CreateEventEx;                       /* 0x170 */
} EFI_BOOT_SERVICES;

typedef struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    uint32_t _pad0;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    void *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#define EFI_ERROR(Status) ((Status) != EFI_SUCCESS)

/* ---------- GUID ---------- */
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, \
      { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_FILE_INFO_ID \
    { 0x09576e92, 0x6d3f, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
/* OpenProtocol Attributes */
#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL 0x00000001
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL       0x00000002

#define EFI_FILE_MODE_READ  0x0000000000000001ULL

/* ---------- Loaded Image Protocol ---------- */
typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---------- File Protocol ---------- */
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName, uint64_t OpenMode, uint64_t Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *This, uint64_t *Position);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This, uint64_t Position);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *Type,
                                 UINTN *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *Type,
                                 UINTN BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *This);
};

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                     EFI_FILE_PROTOCOL **Root);
};

/* ---------- File Info ---------- */
typedef struct {
    uint16_t Year;
    uint8_t  Month;
    uint8_t  Day;
    uint8_t  Hour;
    uint8_t  Minute;
    uint8_t  Second;
    uint8_t  Pad1;
    uint32_t Nanosecond;
    int16_t  TimeZone;
    uint8_t  Daylight;
    uint8_t  Pad2;
} EFI_TIME;

typedef struct {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    uint64_t Attribute;
    CHAR16   FileName[1];   /* 柔性数组 */
} EFI_FILE_INFO;

/* ---------- 本 stub 提供 ---------- */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

/* 由 main.c 提供，供 file.c 使用 */
EFI_STATUS efi_get_bs(EFI_BOOT_SERVICES **out);

#endif