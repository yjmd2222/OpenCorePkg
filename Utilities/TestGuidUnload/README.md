# GUID unload host tests

Includes the actual OpenCoreUefiUnloadDrivers.c implementation with mocked firmware services and real UDK GUID/device-path libraries. Verifies canonical/lowercase GUID config dispatch, firmware file nodes after a prefix, unnamed images without DriverBinding, legacy and mixed name/GUID reverse order, duplicate-image attempt suppression, missing/invalid paths, invalid GUID fallback, not-found targets, firmware EFI_UNSUPPORTED, and no probing after successful unload.

On macOS with Command Line Tools:

```sh
make CC=/Library/Developer/CommandLineTools/usr/bin/clang USE_SHARED_OBJS=1 CFLAGS='-isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk' LDFLAGS='-Wl,-dead_strip -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'
./GuidUnload
```

Unsupported/Not Found log lines are expected mock outcomes; the final PASS line confirms success. This does not prove a particular firmware driver supports unloading.
