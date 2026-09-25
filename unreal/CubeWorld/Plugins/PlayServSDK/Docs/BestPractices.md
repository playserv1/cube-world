# Best practices

## Capture UObjects as `TWeakObjectPtr` in async lambdas

Callbacks run later, when the `UObject*` that made the call may have been garbage-collected. Capture it weakly:

```cpp
TWeakObjectPtr<UMyPlayer> WeakP = P;
PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
    [WeakP](bool bOk, const FPlayServError& Err)
    {
        UMyPlayer* StrongP = WeakP.Get();
        if (!StrongP) { return; }   // entity gone
        StrongP->OnSaved(bOk);
    }));
```

## Keep entities alive yourself

The SDK never extends an entity's lifetime: operations, queues and subscriptions all hold it weakly. Own the instance through a `UPROPERTY`, a `TStrongObjectPtr`, or by renaming it onto an owning object.

## Prefer the `PlayServ::` namespace API

Use `PlayServ::Data::Save(...)` rather than `Subsystem->GetData()->SaveEntity(...)`. The namespace API is the supported public surface and handles an unavailable subsystem for you; the module methods exist for callers that already hold the subsystem.

## Check `IsLoggedIn()` before data operations

A player-owned table answers a client only with the owning player's session; without one the call is refused. Start those calls after a successful login, and bind `OnSessionLost` to sign in again.

## Create the tables before you play

Every entity class needs a table on the platform ([Schema](Schema.md)). The SDK warns at startup about a class with no table; otherwise the first call on it fails with `NotFound`.

## Never log tokens

Treat `GetAccessToken()` output, server keys, the launcher exchange code and Steam tickets as opaque secrets and keep them out of user-visible surfaces, logs and crash reports.

## Use `LogPlayServ Verbose` when debugging

Per-request traces carry a correlation id and duration, so a platform incident is diagnosable from a single client log.

## Populate before touching entity ref fields

After `Load`, entity reference fields are stubs, with the id set and no data. Reading those fields before `Populate` returns defaults, not platform data.

## Save promptly on subscribed entities

A live update overwrites the whole instance. If you edit a subscribed entity locally, save it before the next push can arrive, or keep a separate write model.

## Find a record by a field, not by an id you chose

Record ids are minted by the platform and cannot be chosen. A player's own row needs no identity of
your own: mark the class `PlayServPlayerOwned` ([Entities](Entities.md)) and load it with
`LoadPlayerOwned`. For anything else, give the identity that means something to your game a field of
its own, such as a unique name, and load by it with `LoadAll` and a filter ([Data](Data.md)).

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
