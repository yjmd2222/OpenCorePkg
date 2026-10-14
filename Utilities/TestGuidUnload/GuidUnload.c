/** @file
  Host tests for the actual GUID/name unload implementation.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <assert.h>
#include <stdio.h>
#include "../../Library/OcMainLib/OpenCoreUefiUnloadDrivers.c"
EFI_GUID gEfiComponentName2ProtocolGuid = EFI_COMPONENT_NAME2_PROTOCOL_GUID;
#define IMAGE_COUNT 4
STATIC EFI_LOADED_IMAGE_PROTOCOL Images[IMAGE_COUNT];
STATIC EFI_DRIVER_BINDING_PROTOCOL Binding;
STATIC EFI_COMPONENT_NAME_PROTOCOL Component;
STATIC BOOLEAN Bound[IMAGE_COUNT], Alive[IMAGE_COUNT];
STATIC EFI_STATUS Result[IMAGE_COUNT];
STATIC UINTN Calls[IMAGE_COUNT], CallCount;
STATIC struct {
  EFI_DEVICE_PATH_PROTOCOL Prefix;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH File;
  EFI_DEVICE_PATH_PROTOCOL End;
} Paths[IMAGE_COUNT];
STATIC UINTN IndexOf(EFI_HANDLE H) { return (UINTN)H - 1; }
STATIC EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE Search, EFI_GUID *Guid, VOID *Key, UINTN *Count, EFI_HANDLE **Handles) {
  UINTN I;
  *Handles = AllocatePool(IMAGE_COUNT * sizeof(**Handles)); *Count = 0;
  for (I = 0; I < IMAGE_COUNT; ++I) {
    if (Alive[I] && (CompareGuid(Guid, &gEfiLoadedImageProtocolGuid) || (CompareGuid(Guid, &gEfiDriverBindingProtocolGuid) && Bound[I]))) {
      (*Handles)[(*Count)++] = (EFI_HANDLE)(I + 1);
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Protocol(EFI_HANDLE H, EFI_GUID *Guid, VOID **Interface) {
  UINTN I = IndexOf(H);
  assert(I < IMAGE_COUNT && Alive[I]);
  if (CompareGuid(Guid, &gEfiLoadedImageProtocolGuid)) { *Interface = &Images[I]; return EFI_SUCCESS; }
  if (CompareGuid(Guid, &gEfiDriverBindingProtocolGuid) && Bound[I]) { *Interface = &Binding; return EFI_SUCCESS; }
  return EFI_NOT_FOUND;
}
STATIC EFI_STATUS EFIAPI Open(EFI_HANDLE H, EFI_GUID *Guid, VOID **Interface, EFI_HANDLE Agent, EFI_HANDLE Controller, UINT32 Attributes) {
  UINTN I = IndexOf(H); assert(Alive[I]);
  if (Bound[I] && CompareGuid(Guid, &gEfiComponentNameProtocolGuid)) { *Interface = &Component; return EFI_SUCCESS; }
  return EFI_NOT_FOUND;
}
STATIC EFI_STATUS EFIAPI DriverName(EFI_COMPONENT_NAME_PROTOCOL *This, CHAR8 *Lang, CHAR16 **Name) {
  *Name = L"Legacy Driver"; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Unload(EFI_HANDLE H) {
  UINTN I = IndexOf(H); Calls[CallCount++] = I;
  if (!EFI_ERROR(Result[I])) { Alive[I] = FALSE; }
  return Result[I];
}
STATIC VOID Setup(VOID) {
  UINTN I;
  ZeroMem(Images, sizeof(Images)); ZeroMem(Paths, sizeof(Paths)); ZeroMem(Bound, sizeof(Bound)); CallCount = 0;
  Component.GetDriverName = DriverName; Component.SupportedLanguages = "eng";
  for (I = 0; I < IMAGE_COUNT; ++I) {
    Alive[I] = TRUE; Result[I] = EFI_SUCCESS;
    Paths[I].Prefix.Type = HARDWARE_DEVICE_PATH; Paths[I].Prefix.SubType = HW_PCI_DP;
    SetDevicePathNodeLength(&Paths[I].Prefix, sizeof(Paths[I].Prefix));
    Paths[I].File.Header.Type = MEDIA_DEVICE_PATH; Paths[I].File.Header.SubType = MEDIA_PIWG_FW_FILE_DP;
    SetDevicePathNodeLength(&Paths[I].File.Header, sizeof(Paths[I].File));
    SetDevicePathEndNode(&Paths[I].End); Images[I].FilePath = &Paths[I].Prefix;
  }
  assert(!EFI_ERROR(StrToGuid(L"3022E512-B94A-4F12-806D-7EF1177899D8", &Paths[2].File.FvFileName)));
  Paths[0].File.FvFileName = Paths[2].File.FvFileName;
}
STATIC VOID Target(UNLOAD_INFO *Info, CHAR16 *Name) {
  ZeroMem(Info, sizeof(*Info)); Info->UnloadName = Name;
  if (StrLen(Name) == 36) { Info->MatchGuid = !EFI_ERROR(StrToGuid(Name, &Info->FileGuid)); }
}
int ENTRY_POINT(void) {
  UNLOAD_INFO Info[2]; UNLOAD_IMAGE_CONTEXT Context = { 2, Info };
  OC_GLOBAL_CONFIG Config; OC_STRING Strings[2]; OC_STRING *Values[2] = { &Strings[0], &Strings[1] };
  gBS->LocateHandleBuffer = Locate; gBS->HandleProtocol = Protocol; gBS->OpenProtocol = Open; gBS->UnloadImage = Unload;
  Setup(); Target(&Info[0], L"3022e512-b94a-4f12-806d-7ef1177899d8"); Target(&Info[1], L"Legacy Driver"); Bound[1] = TRUE;
  ProcessUnloadImages(&Context);
  assert(CallCount == 2 && Calls[0] == 2 && Calls[1] == 1);
  assert(Info[0].Unloaded && Info[1].Unloaded && Alive[0]);
  Setup(); Target(&Info[0], L"3022E512-B94A-4F12-806D-7EF1177899D8"); Target(&Info[1], L"Missing Driver"); Result[2] = EFI_UNSUPPORTED;
  ProcessUnloadImages(&Context); assert(CallCount == 1 && Calls[0] == 2 && Info[0].Unloaded && Alive[2] && !Info[1].Unloaded);
  Setup(); Target(&Info[0], L"3022E512-B94A-4F12-806D-7EF1177899D8"); Target(&Info[1], L"Missing Driver");
  Images[2].FilePath = NULL; Paths[0].File.Header.Length[0] = 3;
  ProcessUnloadImages(&Context); assert(CallCount == 0 && !Info[0].Unloaded);
  Setup(); Target(&Info[0], L"Legacy Driver"); Target(&Info[1], L"3022E512-B94A-4F12-806D-7EF1177899DZ"); Bound[1] = TRUE;
  assert(!Info[1].MatchGuid); ProcessAllDrivers(&Context, UnloadImageByName, FALSE); assert(CallCount == 1 && Calls[0] == 1);
  Setup(); Target(&Info[0], L"3022E512-B94A-4F12-806D-7EF1177899D8"); Target(&Info[1], L"Legacy Driver");
  Images[0].FilePath = NULL; SetDevicePathNodeLength(&Paths[2].File.Header, sizeof(Paths[2].File) - 1);
  ProcessUnloadImages(&Context); assert(CallCount == 0);
  ZeroMem(&Config, sizeof(Config)); ZeroMem(Strings, sizeof(Strings));
  Config.Uefi.Unload.Count = 2; Config.Uefi.Unload.Values = Values;
  Strings[0].DynValue = "3022e512-b94a-4f12-806d-7ef1177899d8"; Strings[1].DynValue = "Legacy Driver";
  Setup(); Bound[1] = TRUE; OcUnloadDrivers(&Config);
  assert(CallCount == 2 && Calls[0] == 2 && Calls[1] == 1);
  Strings[0].DynValue = "3022E512-B94A-4F12-806D-7EF1177899DZ";
  Setup(); Bound[1] = TRUE; OcUnloadDrivers(&Config);
  assert(CallCount == 1 && Calls[0] == 1);
  Strings[0].DynValue = "FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF"; Strings[1].DynValue = "Missing Driver";
  Setup(); OcUnloadDrivers(&Config); assert(CallCount == 0);
  puts("PASS: GUID paths, lowercase, reverse mixed order, legacy names, missing/malformed paths, firmware refusal, no post-unload access");
  return 0;
}
