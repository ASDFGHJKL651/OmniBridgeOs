#include "file.h"

EFI_STATUS uefi_read_file(EFI_HANDLE image_handle, CHAR16 *path,
                          void **out_buf, UINTN *out_size)
{
    EFI_BOOT_SERVICES *bs = 0;
    EFI_STATUS st = efi_get_bs(&bs);
    if (EFI_ERROR(st)) return st;

    EFI_GUID li_guid  = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID fi_guid  = EFI_FILE_INFO_ID;

    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    st = bs->OpenProtocol(image_handle, &li_guid, (void **)&li,
                          image_handle, 0,
                          EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(st)) return st;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = 0;
    st = bs->OpenProtocol(li->DeviceHandle, &sfs_guid, (void **)&sfs,
                          image_handle, 0,
                          EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(st)) return st;

    EFI_FILE_PROTOCOL *root = 0;
    st = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(st)) return st;

    EFI_FILE_PROTOCOL *file = 0;
    st = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) return st;

    /* 先查询 FileInfo 大小 */
    UINTN info_sz = 0;
    root->GetInfo(file, &fi_guid, &info_sz, 0);  /* 第一次必返回 BUFFER_TOO_SMALL */
    if (info_sz == 0) return EFI_LOAD_ERROR;

    EFI_FILE_INFO *info = 0;
    st = bs->AllocatePool(EfiLoaderData, info_sz, (void **)&info);
    if (EFI_ERROR(st)) return st;

    st = file->GetInfo(file, &fi_guid, &info_sz, info);
    if (EFI_ERROR(st)) return st;

    UINTN size = (UINTN)info->FileSize;
    if (size == 0) return EFI_LOAD_ERROR;

    void *buf = 0;
    st = bs->AllocatePool(EfiLoaderData, size, &buf);
    if (EFI_ERROR(st)) return st;

    UINTN read_sz = size;
    st = file->Read(file, &read_sz, buf);

    file->Close(file);
    root->Close(root);
    bs->FreePool(info);

    if (EFI_ERROR(st) || read_sz != size) return EFI_LOAD_ERROR;

    *out_buf  = buf;
    *out_size = size;
    return EFI_SUCCESS;
}