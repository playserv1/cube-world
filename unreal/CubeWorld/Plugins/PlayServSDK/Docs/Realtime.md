# Realtime

`Reload` fetches the platform's state once. `Subscribe` keeps an entity current: while subscribed, the platform sends the record's latest state after each change and the SDK applies it to your instance. Use it for data that another client, a dedicated server or a cloud function may change, such as a shared session, a leaderboard row or a co-op inventory.

```cpp
#include "PlayServ.h"

// Keep the instance current, and hear which top-level fields changed.
FPlayServSubscriptionHandle Handle = PlayServ::Data::Subscribe(Player,
    FOnPlayServObjectChanged::CreateLambda(
        [](UObject* Entity, const TArray<FName>& ChangedFields)
        {
            if (ChangedFields.Contains(GET_MEMBER_NAME_CHECKED(UMyPlayer, Currency)))
            {
                // refresh only the currency widget
            }
        }));

// Keep-current only: no delegate. The class's PlayServOnChanged functions (Entities.md) still run.
FPlayServSubscriptionHandle Silent = PlayServ::Data::Subscribe(Player);

// ... later (EndPlay / BeginDestroy):
PlayServ::Data::Unsubscribe(Handle);
PlayServ::Data::Unsubscribe(Silent);
```

`FOnPlayServObjectChanged` is a single-cast delegate: `CreateLambda`, `CreateUObject`, `CreateSP` and friends all work. `FPlayServSubscriptionHandle` is an opaque token: `IsValid()` is false when the subscription was refused, and `Unsubscribe` ignores an invalid or ended handle.

## Requirements

- The entity is a marked entity class ([Entities](Entities.md)) with a record id (loaded, or saved once).
- A client session is logged in; a server session cannot subscribe.
- The entity is not a singleton: in this release a change made to a singleton through the platform would not reach a subscription, so `Subscribe` refuses one. `Reload` it instead.

A `Subscribe` that does not meet these returns an invalid handle and logs a warning.

## Semantics

- **The platform's state wins.** Each update is the record's whole state, applied as `Reload` applies it ([Serialization](Serialization.md)): unsaved local edits on a subscribed instance are overwritten. Treat a subscribed entity as a mirror you mostly read; if you also edit it, `Save` promptly.
- **Top-level fields.** `ChangedFields` lists the top-level UPROPERTY names whose values changed; a change inside a struct or an array names the field that holds it (`Profile`, never `Profile.Age`). Compare with `GET_MEMBER_NAME_CHECKED(...)`, so a rename breaks the build instead of the check. Values are compared as the platform stores them, so a reference to the same record is not a change. An update that changes nothing does not call the delegate.
- **Order.** The whole document is applied, then the `PlayServOnChanged` functions of the changed fields ([Entities](Entities.md)), then the delegates. A delegate may subscribe, unsubscribe or log out; a handle it ended is not called.
- **Latest state, not every change.** Several changes in quick succession can arrive as one update.
- **Per-record.** Two handles on the same record share one platform subscription; different records are independent. Several instances of one record can each be subscribed and each gets updated.
- **The SDK holds the entity weakly.** Keep it alive yourself (`TStrongObjectPtr`, a `UPROPERTY`): a garbage-collected entity is dropped without notice. `Unsubscribe` every handle when you are done.
- **A deleted record ends its subscriptions without notice.** The instance keeps its last state and no delegate is called. Nothing else on the connection is affected.
- **Logout ends every subscription**, and so does shutdown. If the connection is lost and the SDK's one reconnect attempt fails, every subscription is dropped with a warning in the log and the entities keep their last state; subscribe again after signing in again.
- **Registration takes a moment.** The first `Subscribe` opens the connection, and a subscription counts from when it is registered. A change made to the record right as you call `Subscribe` can come before that and produce no update.

## Connection

One WebSocket per process to `wss://<BaseURL host>/ws/`, opened on the first `Subscribe` and closed at shutdown. The SDK answers the platform's keepalives, renews the player's access token on the connection, and reconnects once after an unexpected close. Credentials on it are never logged, as for every request ([Errors and logging](Errors.md)).

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
