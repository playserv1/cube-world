# Versioning

Include `PlayServVersion.h` for compile-time version checks:

```cpp
#include "PlayServVersion.h"

#if PLAYSERV_SDK_VERSION_NUMBER >= PLAYSERV_MAKE_VERSION(0, 7, 0)
    // code path that requires 0.7.0+ APIs
#endif

UE_LOG(LogMyGame, Display, TEXT("PlayServ SDK %s"), PLAYSERV_SDK_VERSION);
```

| Macro | Value |
|-------|-------|
| `PLAYSERV_SDK_VERSION_MAJOR` | `0` |
| `PLAYSERV_SDK_VERSION_MINOR` | `7` |
| `PLAYSERV_SDK_VERSION_PATCH` | `0` |
| `PLAYSERV_SDK_VERSION_NUMBER` | `0x000700`, for comparisons |
| `PLAYSERV_SDK_VERSION` | `TEXT("0.7.0")` |
| `PLAYSERV_MAKE_VERSION(M, N, P)` | Compile-time comparison helper |

The plugin descriptor (`PlayServSDK.uplugin`) carries the same version as `VersionName`; the SDK reports it to the platform when it opens the live-update connection.

## Engines

Unreal Engine 5.8, source and launcher builds. Building one checkout with two engines in turn (a launcher and a source build, or two versions) needs `Intermediate/` and `Binaries/` cleared, in the project and the plugin, in between: the code generator is compiled per engine, and its output cannot be shared.

## Beta status

0.7.0 ships with `IsBetaVersion: true` in the `.uplugin`. The API can still change during beta, and some platform details may tighten before it is locked. The login shape (`LoginExternal` with a caller-supplied provider credential), the entity marking (`UCLASS(PlayServEntity)`), the `PlayServ::` namespace and the error model are settled.

## Support

For bugs, feature requests, or integration support, contact your PlayServ representative.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
