namespace PlayServ::Data::Private
{
	template<typename T>
	constexpr bool IsSingleton()
	{
		if constexpr (requires { T::bPlayServSingleton; })
		{
			return T::bPlayServSingleton;
		}
		else
		{
			return false;
		}
	}

	template<typename T>
	constexpr bool IsPlayerOwned()
	{
		if constexpr (requires { T::bPlayServPlayerOwned; })
		{
			return T::bPlayServPlayerOwned;
		}
		else
		{
			return false;
		}
	}
}

namespace PlayServ::Data
{
	template<typename T>
	T* Create()
	{
		static_assert(!Private::IsSingleton<T>(), "A singleton is never created: load its one row with PlayServ::Data::LoadSingleton<T>");
		static_assert(!Private::IsPlayerOwned<T>(), "A player-owned row is found, and created on the player's side, by PlayServ::Data::LoadPlayerOwned<T>");
		return NewObject<T>(GetTransientPackage());
	}

	template<typename T>
	void Load(const FString& RecordId, TFunction<void(bool, T*, const FPlayServError&)> Callback)
	{
		static_assert(!Private::IsSingleton<T>(), "A singleton has no record id to load by: use PlayServ::Data::LoadSingleton<T>");
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS)
		{
			if (Callback)
			{
				Callback(false, nullptr, FPlayServError::SubsystemUnavailable());
			}
			return;
		}
		PS->GetData()->LoadEntity(GetTransientPackage(), T::StaticClass(), T::StaticClass()->GetName(), RecordId,
			FPlayServEntityLoadCallback::CreateLambda(
				[Callback](bool bSuccess, UObject* Result, const FPlayServError& Error)
				{
					if (Callback)
					{
						Callback(bSuccess, static_cast<T*>(Result), Error);
					}
				}));
	}

	template<typename T>
	void LoadPlayerOwned(const APlayerController* Player, TFunction<void(bool, T*, bool, const FPlayServError&)> Callback)
	{
		static_assert(Private::IsPlayerOwned<T>(), "LoadPlayerOwned needs a UCLASS(PlayServEntity, PlayServPlayerOwned) class");
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS)
		{
			if (Callback)
			{
				Callback(false, nullptr, false, FPlayServError::SubsystemUnavailable());
			}
			return;
		}
		PS->GetData()->LoadPlayerOwnedEntity(GetTransientPackage(), T::StaticClass(), Player, FPlayServEntityLoadPlayerOwnedCallback::CreateLambda(
			[Callback](bool bSuccess, UObject* Result, bool bCreated, const FPlayServError& Error)
			{
				if (Callback)
				{
					Callback(bSuccess, static_cast<T*>(Result), bCreated, Error);
				}
			}));
	}

	template<typename T>
	void LoadPlayerOwned(const FString& PlayerId, TFunction<void(bool, T*, bool, const FPlayServError&)> Callback)
	{
		static_assert(Private::IsPlayerOwned<T>(), "LoadPlayerOwned needs a UCLASS(PlayServEntity, PlayServPlayerOwned) class");
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS)
		{
			if (Callback)
			{
				Callback(false, nullptr, false, FPlayServError::SubsystemUnavailable());
			}
			return;
		}
		PS->GetData()->LoadPlayerOwnedEntity(GetTransientPackage(), T::StaticClass(), PlayerId, FPlayServEntityLoadPlayerOwnedCallback::CreateLambda(
			[Callback](bool bSuccess, UObject* Result, bool bCreated, const FPlayServError& Error)
			{
				if (Callback)
				{
					Callback(bSuccess, static_cast<T*>(Result), bCreated, Error);
				}
			}));
	}

	template<typename T>
	void LoadSingleton(TFunction<void(bool, T*, const FPlayServError&)> Callback)
	{
		static_assert(Private::IsSingleton<T>(), "LoadSingleton needs a UCLASS(PlayServSingleton) class");
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS)
		{
			if (Callback)
			{
				Callback(false, nullptr, FPlayServError::SubsystemUnavailable());
			}
			return;
		}
		PS->GetData()->LoadSingletonEntity(GetTransientPackage(), T::StaticClass(), FPlayServEntityLoadCallback::CreateLambda(
			[Callback](bool bSuccess, UObject* Result, const FPlayServError& Error)
			{
				if (Callback)
				{
					Callback(bSuccess, static_cast<T*>(Result), Error);
				}
			}));
	}

	template<typename T>
	void LoadAll(const FPlayServFilter& Filter, TFunction<void(bool, TArray<T*>, const FPlayServError&)> Callback)
	{
		static_assert(!Private::IsSingleton<T>(), "A singleton has one row: load it with PlayServ::Data::LoadSingleton<T>");
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS)
		{
			if (Callback)
			{
				Callback(false, TArray<T*>(), FPlayServError::SubsystemUnavailable());
			}
			return;
		}
		PS->GetData()->LoadAllEntities(GetTransientPackage(), T::StaticClass(), T::StaticClass()->GetName(), Filter,
			FPlayServEntityLoadAllCallback::CreateLambda(
				[Callback](bool bSuccess, const TArray<UObject*>& Results, const FPlayServError& Error)
				{
					if (!Callback)
					{
						return;
					}
					TArray<T*> Typed;
					Typed.Reserve(Results.Num());
					for (UObject* Obj : Results)
					{
						Typed.Add(static_cast<T*>(Obj));
					}
					Callback(bSuccess, MoveTemp(Typed), Error);
				}));
	}

	template<typename T>
	void DeleteById(const FString& RecordId, FPlayServSimpleCallback Callback)
	{
		static_assert(!Private::IsSingleton<T>(), "The row of a singleton belongs to its table and is never deleted");
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS)
		{
			Callback.ExecuteIfBound(false, FPlayServError::SubsystemUnavailable());
			return;
		}
		PS->GetData()->Delete(T::StaticClass()->GetName(), RecordId, MoveTemp(Callback));
	}

	template<typename T>
	void DeleteAll(const FPlayServFilter& Filter, FPlayServDeleteAllCallback Callback)
	{
		static_assert(!Private::IsSingleton<T>(), "The row of a singleton belongs to its table and is never deleted");
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS)
		{
			Callback.ExecuteIfBound(false, FPlayServDeleteAllResult(), FPlayServError::SubsystemUnavailable());
			return;
		}
		PS->GetData()->DeleteAll(T::StaticClass()->GetName(), Filter, MoveTemp(Callback));
	}
}
