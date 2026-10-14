/** @file
  Unload images by name or firmware file GUID. Includes image name code from ShellPkg UefiHandleParsingLib.c.

  Copyright (c) 2010 - 2017, Intel Corporation. All rights reserved.<BR>
  (C) Copyright 2013-2015 Hewlett-Packard Development Company, L.P.<BR>
  (C) Copyright 2015-2021 Hewlett Packard Enterprise Development LP<BR>
  Copyright (c) 2024, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Library/OcConfigurationLib.h>

#include <PiDxe.h>

#include <Protocol/ComponentName2.h>
#include <Protocol/ComponentName.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>

typedef
VOID
(*PROCESS_IMAGE)(
  IN VOID          *Context,
  IN CONST CHAR16  *Name,
  IN EFI_HANDLE    Handle
  );

typedef struct {
  CHAR16     *UnloadName;
  EFI_GUID   FileGuid;
  BOOLEAN    MatchGuid;
  BOOLEAN    Unloaded;
} UNLOAD_INFO;

typedef struct {
  UINTN          UnloadNameCount;
  UNLOAD_INFO    *UnloadInfo;
} UNLOAD_IMAGE_CONTEXT;

typedef struct {
  CHAR8    *FileBuffer;
  UINTN    FileBufferSize;
} DRIVER_REPORT_CONTEXT;

/**
  Get the best supported language for this driver.

  First base on the user input language  to search, otherwise get the first language
  from the supported language list. The caller needs to free the best language buffer.

  @param[in] SupportedLanguages      The supported languages for this driver.
  @param[in] InputLanguage           The user input language.
  @param[in] Iso639Language          Whether get language for ISO639.

  @return                            The best supported language for this driver.
**/
CHAR8 *
EFIAPI
GetBestLanguageForDriver (
  IN CONST CHAR8  *SupportedLanguages,
  IN CONST CHAR8  *InputLanguage,
  IN BOOLEAN      Iso639Language
  )
{
  CHAR8  *BestLanguage;

  BestLanguage = GetBestLanguage (
                   SupportedLanguages,
                   Iso639Language,
                   (InputLanguage != NULL) ? InputLanguage : "",
                   SupportedLanguages,
                   NULL
                   );

  return BestLanguage;
}

/**
  Function to retrieve the driver name (if possible) from the ComponentName or
  ComponentName2 protocol

  @param[in] TheHandle      The driver handle to get the name of.
  @param[in] Language       The language to use.

  @retval NULL              The name could not be found.
  @return                   A pointer to the string name.  Do not de-allocate the memory.
**/
CONST CHAR16 *
EFIAPI
GetStringNameFromHandle (
  IN CONST EFI_HANDLE  TheHandle,
  IN CONST CHAR8       *Language
  )
{
  EFI_COMPONENT_NAME2_PROTOCOL  *CompNameStruct;
  EFI_STATUS                    Status;
  CHAR16                        *RetVal;
  CHAR8                         *BestLang;

  BestLang = NULL;

  Status = gBS->OpenProtocol (
                  TheHandle,
                  &gEfiComponentName2ProtocolGuid,
                  (VOID **)&CompNameStruct,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (!EFI_ERROR (Status)) {
    BestLang = GetBestLanguageForDriver (CompNameStruct->SupportedLanguages, Language, FALSE);
    Status   = CompNameStruct->GetDriverName (CompNameStruct, BestLang, &RetVal);
    if (BestLang != NULL) {
      FreePool (BestLang);
      BestLang = NULL;
    }

    if (!EFI_ERROR (Status)) {
      return (RetVal);
    }
  }

  Status = gBS->OpenProtocol (
                  TheHandle,
                  &gEfiComponentNameProtocolGuid,
                  (VOID **)&CompNameStruct,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (!EFI_ERROR (Status)) {
    BestLang = GetBestLanguageForDriver (CompNameStruct->SupportedLanguages, Language, FALSE);
    Status   = CompNameStruct->GetDriverName (CompNameStruct, BestLang, &RetVal);
    if (BestLang != NULL) {
      FreePool (BestLang);
    }

    if (!EFI_ERROR (Status)) {
      return (RetVal);
    }
  }

  return (NULL);
}

VOID
ReportImageName (
  IN VOID          *Context,
  IN CONST CHAR16  *Name,
  IN EFI_HANDLE    Handle
  )
{
  DRIVER_REPORT_CONTEXT  *ReportContext;

  ReportContext = Context;

  OcAsciiPrintBuffer (
    &ReportContext->FileBuffer,
    &ReportContext->FileBufferSize,
    "%s\n",
    Name
    );
}

VOID
UnloadImageByName (
  IN VOID          *Context,
  IN CONST CHAR16  *Name,
  IN EFI_HANDLE    Handle
  )
{
  EFI_STATUS            Status;
  UINTN                 Index;
  UNLOAD_IMAGE_CONTEXT  *UnloadContext;

  UnloadContext = Context;

  for (Index = 0; Index < UnloadContext->UnloadNameCount; Index++) {
    if (  !UnloadContext->UnloadInfo[Index].MatchGuid
       && !UnloadContext->UnloadInfo[Index].Unloaded
       && (StrCmp (UnloadContext->UnloadInfo[Index].UnloadName, Name) == 0))
    {
      Status = gBS->UnloadImage (Handle);
      DEBUG ((
        EFI_ERROR (Status) ? DEBUG_WARN : DEBUG_INFO,
        "OC: UnloadImage %s - %r\n",
        UnloadContext->UnloadInfo[Index].UnloadName,
        Status
        ));
      UnloadContext->UnloadInfo[Index].Unloaded = TRUE; ///< Do not report as 'not found' even if we failed to unload it.
      break;
    }
  }
}

/**
  Match a firmware file node in a loaded image's device path.
**/
STATIC
BOOLEAN
ImageHasFileGuid (
  IN EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage,
  IN CONST EFI_GUID             *FileGuid
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_GUID                  NodeGuid;

  Node = LoadedImage->FilePath;
  if ((Node == NULL) || !IsDevicePathValid (Node, 0)) {
    return FALSE;
  }

  while (!IsDevicePathEnd (Node)) {
    if (  (DevicePathType (Node) == MEDIA_DEVICE_PATH)
       && (DevicePathSubType (Node) == MEDIA_PIWG_FW_FILE_DP)
       && (DevicePathNodeLength (Node) == sizeof (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH)))
    {
      CopyMem (&NodeGuid, &((MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)Node)->FvFileName, sizeof (NodeGuid));
      if (CompareGuid (&NodeGuid, FileGuid)) {
        return TRUE;
      }
    }

    Node = NextDevicePathNode (Node);
  }

  return FALSE;
}

/**
  GUID targets need not implement DriverBinding or ComponentName protocols.
  Enumerate loaded images once so mixed GUID/name targets retain reverse order.
**/
STATIC
VOID
ProcessUnloadImages (
  IN UNLOAD_IMAGE_CONTEXT  *Context
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   *Handles;
  UINTN                        HandleCount;
  UINTN                        HandleIndex;
  UINTN                        Index;
  EFI_LOADED_IMAGE_PROTOCOL    *LoadedImage;
  EFI_DRIVER_BINDING_PROTOCOL  *DriverBinding;
  CONST CHAR16                 *Name;
  BOOLEAN                      Attempted;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiLoadedImageProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OC: Failed to locate unload images - %r\n", Status));
    return;
  }

  for (HandleIndex = HandleCount; HandleIndex > 0; --HandleIndex) {
    Status = gBS->HandleProtocol (
                    Handles[HandleIndex - 1],
                    &gEfiLoadedImageProtocolGuid,
                    (VOID **)&LoadedImage
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Attempted = FALSE;
    for (Index = 0; Index < Context->UnloadNameCount; ++Index) {
      if (  Context->UnloadInfo[Index].MatchGuid
         && !Context->UnloadInfo[Index].Unloaded
         && ImageHasFileGuid (LoadedImage, &Context->UnloadInfo[Index].FileGuid))
      {
        DEBUG ((DEBUG_INFO, "OC: GUID unload target %g handle %p callback %p\n", &Context->UnloadInfo[Index].FileGuid, Handles[HandleIndex - 1], LoadedImage->Unload));
        Status = gBS->UnloadImage (Handles[HandleIndex - 1]);
        DEBUG ((
          EFI_ERROR (Status) ? DEBUG_WARN : DEBUG_INFO,
          "OC: UnloadImage GUID %g - %r\n",
          &Context->UnloadInfo[Index].FileGuid,
          Status
          ));
        Context->UnloadInfo[Index].Unloaded = TRUE;
        Attempted                          = TRUE;
        break;
      }
    }

    //
    // The image may have been freed by UnloadImage. Do not probe it again.
    //
    if (Attempted) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[HandleIndex - 1],
                    &gEfiDriverBindingProtocolGuid,
                    (VOID **)&DriverBinding
                    );
    if (!EFI_ERROR (Status)) {
      Name = GetStringNameFromHandle (Handles[HandleIndex - 1], NULL);
      if (Name != NULL) {
        UnloadImageByName (Context, Name, Handles[HandleIndex - 1]);
      }
    }
  }

  FreePool (Handles);
}

//
// Perform action for all driver binding protocols which are
// present on the same handle as loaded image protocol and
// where the handle has a usable name.
//
// Note: In terms of choosing a name to identify the driver, while the loaded image
// section name is shorter and corresponds better to the driver's file name, and the
// firmware volume GUID from the loaded image path identifies the driver more uniquely,
// these are both not always available for a loaded image (across various firmware),
// whereas the driver component name normally is.
//
VOID
ProcessAllDrivers (
  IN VOID        *Context,
  PROCESS_IMAGE  ProcessImage,
  BOOLEAN        Ascending
  )
{
  EFI_STATUS                 Status;
  UINTN                      HandleCount;
  EFI_HANDLE                 *HandleBuffer;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  UINTN                      Index;
  CONST CHAR16               *Name;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiDriverBindingProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (
       Ascending ? (Index = 0) : (Index = HandleCount - 1);
       Index < HandleCount;  ///< UINTN: (HandleCount >= HandleCount) && ((0 - 1) >= HandleCount)
       Ascending ? Index++ : Index--
       )
  {
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiLoadedImageProtocolGuid,
                    (VOID **)&LoadedImage
                    );
    if (!EFI_ERROR (Status)) {
      Name = GetStringNameFromHandle (HandleBuffer[Index], NULL);     ///< Do not free Name.
      if (Name != NULL) {
        ProcessImage (Context, Name, HandleBuffer[Index]);
      }
    }
  }

  FreePool (HandleBuffer);

  return;
}

VOID
OcUnloadDrivers (
  IN  OC_GLOBAL_CONFIG  *Config
  )
{
  UINTN                 Index;
  UINTN                 Index2;
  UINTN                 UnloadInfoSize;
  UNLOAD_IMAGE_CONTEXT  UnloadContext;
  BOOLEAN               HasGuid;

  if (Config->Uefi.Unload.Count == 0) {
    return;
  }

  if (!BaseOverflowMulUN (
         Config->Uefi.Unload.Count,
         sizeof (*UnloadContext.UnloadInfo),
         &UnloadInfoSize
         ))
  {
    UnloadContext.UnloadInfo = AllocateZeroPool (UnloadInfoSize);
  } else {
    UnloadContext.UnloadInfo = NULL;
  }

  if (UnloadContext.UnloadInfo == NULL) {
    DEBUG ((DEBUG_ERROR, "OC: Failed to allocate unload names!\n"));
    return;
  }

  HasGuid = FALSE;
  for (Index = 0; Index < Config->Uefi.Unload.Count; ++Index) {
    UnloadContext.UnloadInfo[Index].UnloadName = AsciiStrCopyToUnicode (
                                                   OC_BLOB_GET (
                                                     Config->Uefi.Unload.Values[Index]
                                                     ),
                                                   0
                                                   );
    if (UnloadContext.UnloadInfo[Index].UnloadName == NULL) {
      for (Index2 = 0; Index2 < Index; ++Index2) {
        FreePool (UnloadContext.UnloadInfo[Index2].UnloadName);
      }

      FreePool (UnloadContext.UnloadInfo);
      DEBUG ((DEBUG_ERROR, "OC: Failed to allocate unload names!\n"));
      return;
    }

    if (StrLen (UnloadContext.UnloadInfo[Index].UnloadName) == 36) {
      UnloadContext.UnloadInfo[Index].MatchGuid = !EFI_ERROR (
                                                   StrToGuid (
                                                     UnloadContext.UnloadInfo[Index].UnloadName,
                                                     &UnloadContext.UnloadInfo[Index].FileGuid
                                                     )
                                                   );
      HasGuid |= UnloadContext.UnloadInfo[Index].MatchGuid;
    }
  }

  UnloadContext.UnloadNameCount = Config->Uefi.Unload.Count;

  if (HasGuid) {
    ProcessUnloadImages (&UnloadContext);
  } else {
    ProcessAllDrivers (&UnloadContext, UnloadImageByName, FALSE);
  }

  for (Index = 0; Index < Config->Uefi.Unload.Count; ++Index) {
    if (!UnloadContext.UnloadInfo[Index].Unloaded) {
      DEBUG ((DEBUG_INFO, "OC: Unload %s - %r\n", UnloadContext.UnloadInfo[Index].UnloadName, EFI_NOT_FOUND));
    }

    FreePool (UnloadContext.UnloadInfo[Index].UnloadName);
  }

  FreePool (UnloadContext.UnloadInfo);

  return;
}

EFI_STATUS
OcDriverInfoDump (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  EFI_STATUS             Status;
  DRIVER_REPORT_CONTEXT  Context;
  CHAR16                 TmpFileName[32];

  ASSERT (Root != NULL);

  Context.FileBufferSize = SIZE_1KB;
  Context.FileBuffer     = AllocateZeroPool (Context.FileBufferSize);
  if (Context.FileBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ProcessAllDrivers (&Context, ReportImageName, TRUE);

  //
  // Save dumped driver info to file.
  //
  if (Context.FileBuffer != NULL) {
    UnicodeSPrint (TmpFileName, sizeof (TmpFileName), L"DriverImageNames.txt");
    Status = OcSetFileData (Root, TmpFileName, Context.FileBuffer, (UINT32)AsciiStrLen (Context.FileBuffer));
    DEBUG ((DEBUG_INFO, "OC: Dumped driver info - %r\n", Status));

    FreePool (Context.FileBuffer);
  }

  return EFI_SUCCESS;
}
