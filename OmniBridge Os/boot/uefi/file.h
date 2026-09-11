#ifndef OMNIBRIDGE_UEFI_FILE_H
#define OMNIBRIDGE_UEFI_FILE_H

#include "uefi_min.h"

/* 从当前镜像所在卷读取 path（UTF-16，以 0 结尾）。
 * 成功时返回 AllocatePool 分配的缓冲区，调用方负责 FreePool。 */
EFI_STATUS uefi_read_file(EFI_HANDLE image_handle, CHAR16 *path,
                          void **out_buf, UINTN *out_size);

#endif