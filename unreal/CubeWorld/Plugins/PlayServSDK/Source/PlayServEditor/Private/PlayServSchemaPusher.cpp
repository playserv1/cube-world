#include "PlayServSchemaPusher.h"

#include "Core/PlayServSettings.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlayServPusher, Display, All);

namespace
{
	FString JsonToString(const TSharedRef<FJsonObject>& Json)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Json, Writer);
		return Out;
	}

	// "TObjectPtr<UTestClan> Clan" / "TArray<TObjectPtr<UTestPlayer>> Players" /
	// "UTestClan* Clan" -> "TestClan"/"TestPlayer" (schema names carry no U/A prefix).
	// Nested templates take the INNERMOST type: last '<', first '>' after it.
	FString RefTargetFromDecl(const FString& Decl)
	{
		FString Inner = Decl;
		int32 Lt;
		if (Inner.FindLastChar(TEXT('<'), Lt))
		{
			Inner = Inner.Mid(Lt + 1);
			int32 Gt;
			if (Inner.FindChar(TEXT('>'), Gt))
			{
				Inner = Inner.Left(Gt);
			}
		}
		else
		{
			Inner = Inner.TrimStartAndEnd();
			int32 Space;
			if (Inner.FindChar(TEXT(' '), Space))
			{
				Inner = Inner.Left(Space);
			}
		}
		Inner = Inner.TrimStartAndEnd();
		Inner.RemoveFromEnd(TEXT("*"));
		if (Inner.StartsWith(TEXT("U")) || Inner.StartsWith(TEXT("A")))
		{
			Inner.RightChopInline(1);
		}
		return Inner;
	}

	// "FTestProfile Profile" -> "TestProfile" (part names drop the F prefix).
	FString PartTargetFromDecl(const FString& Decl)
	{
		FString Type = Decl.TrimStartAndEnd();
		int32 Space;
		if (Type.FindChar(TEXT(' '), Space))
		{
			Type = Type.Left(Space);
		}
		if (Type.StartsWith(TEXT("F")))
		{
			Type.RightChopInline(1);
		}
		return Type;
	}

	// "ETestRank Rank" -> "ETestRank" (enum schema names KEEP the E prefix on dev).
	FString EnumTargetFromDecl(const FString& Decl)
	{
		FString Type = Decl.TrimStartAndEnd();
		int32 Space;
		if (Type.FindChar(TEXT(' '), Space))
		{
			Type = Type.Left(Space);
		}
		return Type;
	}

	// Class path "/Script/Module.TestPlayer" -> schema entity name "TestPlayer" (class name
	// minus the U prefix — the manifest's "class" member already carries "UTestPlayer").
	FString SchemaNameFromClass(const FString& ClassName)
	{
		FString Name = ClassName;
		if (Name.StartsWith(TEXT("U")) || Name.StartsWith(TEXT("A")))
		{
			Name.RightChopInline(1);
		}
		return Name;
	}

}

FString FPlayServSchemaPusher::ResolveManifestPath(const FString& Override)
{
	if (!Override.IsEmpty())
	{
		return Override;
	}

	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PlayServSDK"));
	if (!Plugin.IsValid())
	{
		return FString();
	}

	// The UHT exporter writes into the plugin's Intermediate tree; the exact subpath is
	// engine/target dependent, so search for the newest manifest.
	TArray<FString> Found;
	IFileManager::Get().FindFilesRecursive(Found, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Intermediate")), TEXT("PlayServManifest.json"), true, false);
	if (Found.Num() == 0)
	{
		return FString();
	}

	Found.Sort([](const FString& A, const FString& B)
	{
		return IFileManager::Get().GetTimeStamp(*A) > IFileManager::Get().GetTimeStamp(*B);
	});
	return Found[0];
}

TSharedPtr<FJsonObject> FPlayServSchemaPusher::LoadJsonFile(const FString& Path)
{
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *Path))
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	FJsonSerializer::Deserialize(Reader, Json);
	return Json;
}

bool FPlayServSchemaPusher::SyncRequest(const FString& Verb, const FString& Url, const TArray<TPair<FString, FString>>& Headers,
	const FString& Body, int32& OutStatus, TSharedPtr<FJsonObject>& OutJson, FString& OutRaw)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(Verb);
	for (const TPair<FString, FString>& Header : Headers)
	{
		Request->SetHeader(Header.Key, Header.Value);
	}
	if (!Body.IsEmpty())
	{
		Request->SetContentAsString(Body);
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	}
	Request->SetTimeout(30.0f);

	bool bDone = false;
	Request->OnProcessRequestComplete().BindLambda(
		[&bDone, &OutStatus, &OutJson, &OutRaw](FHttpRequestPtr, FHttpResponsePtr HttpResponse, bool bConnected)
		{
			if (bConnected && HttpResponse.IsValid())
			{
				OutStatus = HttpResponse->GetResponseCode();
				OutRaw = HttpResponse->GetContentAsString();
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutRaw);
				FJsonSerializer::Deserialize(Reader, OutJson);
			}
			bDone = true;
		});

	if (!Request->ProcessRequest())
	{
		return false;
	}

	// Commandlets have no engine loop — tick the HTTP manager ourselves until completion.
	const double Deadline = FPlatformTime::Seconds() + 35.0;
	while (!bDone && FPlatformTime::Seconds() < Deadline)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.01f);
		FPlatformProcess::Sleep(0.01f);
	}
	return bDone && OutStatus != 0;
}

TSharedPtr<FJsonObject> FPlayServSchemaPusher::MapField(const TSharedPtr<FJsonObject>& ManifestField, const TSet<FString>& EntityNames, FString& OutWarning)
{
	const FString Name = ManifestField->GetStringField(TEXT("name"));
	const FString KindName = ManifestField->GetStringField(TEXT("kindName"));
	const FString Decl = ManifestField->GetStringField(TEXT("decl"));

	TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
	Field->SetStringField(TEXT("name"), Name);
	Field->SetStringField(TEXT("code_key"), Name);

	if (KindName == TEXT("String"))
	{
		Field->SetStringField(TEXT("type"), TEXT("text"));
	}
	else if (KindName == TEXT("Int32") || KindName == TEXT("Int64"))
	{
		Field->SetStringField(TEXT("type"), TEXT("integer"));
	}
	else if (KindName == TEXT("Float") || KindName == TEXT("Double"))
	{
		Field->SetStringField(TEXT("type"), TEXT("number"));
	}
	else if (KindName == TEXT("Bool"))
	{
		Field->SetStringField(TEXT("type"), TEXT("boolean"));
	}
	else if (KindName == TEXT("Enum"))
	{
		Field->SetStringField(TEXT("type"), TEXT("enum"));
		Field->SetStringField(TEXT("target"), EnumTargetFromDecl(Decl));
	}
	else if (KindName == TEXT("Struct"))
	{
		Field->SetStringField(TEXT("type"), TEXT("inclusion"));
		Field->SetStringField(TEXT("target"), PartTargetFromDecl(Decl));
		Field->SetStringField(TEXT("cardinality"), TEXT("one"));
	}
	else if (KindName == TEXT("Array"))
	{
		// Element type decides: entity refs -> relation many; USTRUCTs -> inclusion many;
		// primitives -> json (no array-of-primitive column type exists).
		if (Decl.Contains(TEXT("TObjectPtr")) || Decl.Contains(TEXT("*")))
		{
			Field->SetStringField(TEXT("type"), TEXT("relation"));
			Field->SetStringField(TEXT("target"), RefTargetFromDecl(Decl));
			Field->SetStringField(TEXT("cardinality"), TEXT("many"));
			// A TArray is ordered by construction — the platform must preserve child order or
			// the SDK's array round-trip scrambles (relation many defaults to unordered).
			Field->SetBoolField(TEXT("ordered"), true);
		}
		else
		{
			int32 Lt;
			FString Inner = Decl;
			if (Inner.FindChar(TEXT('<'), Lt))
			{
				Inner = Inner.Mid(Lt + 1);
			}
			Inner = Inner.Replace(TEXT(">"), TEXT("")).TrimStartAndEnd();
			int32 Space;
			if (Inner.FindChar(TEXT(' '), Space))
			{
				Inner = Inner.Left(Space);
			}

			// Core F-prefixed engine types are NOT parts — an F prefix alone doesn't make a
			// USTRUCT (TArray<FString> is json, not an inclusion of a part "String"; the
			// platform's requires_migration gate refuses that type flip on a rowed table).
			static const TSet<FString> CoreFTypes = {
				TEXT("FString"), TEXT("FName"), TEXT("FText"), TEXT("FDateTime"), TEXT("FTimespan"),
				TEXT("FGuid"), TEXT("FVector"), TEXT("FVector2D"), TEXT("FRotator"), TEXT("FTransform"),
				TEXT("FColor"), TEXT("FLinearColor"),
			};
			if (Inner.StartsWith(TEXT("F")) && !CoreFTypes.Contains(Inner))
			{
				Field->SetStringField(TEXT("type"), TEXT("inclusion"));
				Field->SetStringField(TEXT("target"), PartTargetFromDecl(Inner));
				Field->SetStringField(TEXT("cardinality"), TEXT("many"));
			}
			else
			{
				Field->SetStringField(TEXT("type"), TEXT("json"));
			}
		}
	}
	else if (KindName == TEXT("ObjectRef"))
	{
		const FString Target = RefTargetFromDecl(Decl);
		if (!EntityNames.Contains(Target))
		{
			OutWarning = FString::Printf(TEXT("skipping '%s' — ref target '%s' is not a pushed entity"), *Name, *Target);
			return nullptr;
		}
		// relation(one), NOT reference: the platform's runtime READ plane omits reference
		// columns entirely (no plain-GET value, no ?fields=, ?expand= rejects them), while
		// relation(one) round-trips as a plain id string in both directions — exactly the
		// SDK's single-ref wire shape. Revisit if the reader learns to project reference columns.
		Field->SetStringField(TEXT("type"), TEXT("relation"));
		Field->SetStringField(TEXT("target"), Target);
		Field->SetStringField(TEXT("cardinality"), TEXT("one"));
	}
	else
	{
		OutWarning = FString::Printf(TEXT("skipping '%s' — unsupported kind '%s'"), *Name, *KindName);
		return nullptr;
	}

	return Field;
}

TSharedPtr<FJsonObject> FPlayServSchemaPusher::BuildPushDto(const TSharedPtr<FJsonObject>& Manifest, const TSharedPtr<FJsonObject>& Overlay, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* ManifestEntities;
	if (!Manifest->TryGetArrayField(TEXT("entities"), ManifestEntities))
	{
		OutError = TEXT("manifest has no entities array");
		return nullptr;
	}

	TSet<FString> Excluded;
	const TArray<TSharedPtr<FJsonValue>>* ExcludedArr;
	if (Overlay.IsValid() && Overlay->TryGetArrayField(TEXT("excludedClassPaths"), ExcludedArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *ExcludedArr)
		{
			Excluded.Add(V->AsString());
		}
	}

	const TSharedPtr<FJsonObject>* OverlayEntities = nullptr;
	if (Overlay.IsValid())
	{
		Overlay->TryGetObjectField(TEXT("entities"), OverlayEntities);
	}

	// First pass: the pushed entity-name set (ref targets must resolve inside it).
	TSet<FString> EntityNames;
	for (const TSharedPtr<FJsonValue>& EntityValue : *ManifestEntities)
	{
		const TSharedPtr<FJsonObject> Entity = EntityValue->AsObject();
		if (!Excluded.Contains(Entity->GetStringField(TEXT("classPath"))))
		{
			EntityNames.Add(SchemaNameFromClass(Entity->GetStringField(TEXT("class"))));
		}
	}

	TArray<TSharedPtr<FJsonValue>> PushEntities;
	for (const TSharedPtr<FJsonValue>& EntityValue : *ManifestEntities)
	{
		const TSharedPtr<FJsonObject> Entity = EntityValue->AsObject();
		const FString ClassPath = Entity->GetStringField(TEXT("classPath"));
		if (Excluded.Contains(ClassPath))
		{
			UE_LOG(LogPlayServPusher, Display, TEXT("  excluded: %s"), *ClassPath);
			continue;
		}

		const FString SchemaName = SchemaNameFromClass(Entity->GetStringField(TEXT("class")));
		const bool bClientWritable = Entity->GetBoolField(TEXT("clientWritable"));

		TSharedPtr<FJsonObject> EntityDto = MakeShared<FJsonObject>();
		EntityDto->SetStringField(TEXT("name"), SchemaName);

		bool bSingleton = false;
		if (Entity->TryGetBoolField(TEXT("singleton"), bSingleton) && bSingleton)
		{
			EntityDto->SetBoolField(TEXT("singleton"), true);
		}
		bool bOwned = false;
		if (Entity->TryGetBoolField(TEXT("owned"), bOwned) && bOwned)
		{
			EntityDto->SetStringField(TEXT("owned_by"), TEXT("player"));
			EntityDto->SetStringField(TEXT("read"), TEXT("owner"));
			EntityDto->SetStringField(TEXT("on_player_delete"), TEXT("cascade-delete"));
		}

		TArray<TSharedPtr<FJsonValue>> Fields;
		for (const TSharedPtr<FJsonValue>& FieldValue : Entity->GetArrayField(TEXT("fields")))
		{
			FString Warning;
			TSharedPtr<FJsonObject> Field = MapField(FieldValue->AsObject(), EntityNames, Warning);
			if (Field.IsValid())
			{
				Fields.Add(MakeShared<FJsonValueObject>(Field));
			}
			else if (!Warning.IsEmpty())
			{
				UE_LOG(LogPlayServPusher, Warning, TEXT("  %s: %s"), *SchemaName, *Warning);
			}
		}
		if (bOwned)
		{
			TSharedPtr<FJsonObject> PlayerIdField = MakeShared<FJsonObject>();
			PlayerIdField->SetStringField(TEXT("name"), TEXT("player_id"));
			PlayerIdField->SetStringField(TEXT("code_key"), TEXT("player_id"));
			PlayerIdField->SetStringField(TEXT("type"), TEXT("text"));
			PlayerIdField->SetBoolField(TEXT("unique"), true);
			PlayerIdField->SetBoolField(TEXT("required"), true);
			Fields.Add(MakeShared<FJsonValueObject>(PlayerIdField));
		}
		EntityDto->SetArrayField(TEXT("fields"), Fields);

		// ACL from the marking; clientRead default true, overridable per overlay.
		bool bClientRead = true;
		const TSharedPtr<FJsonObject>* EntityOverlay = nullptr;
		if (OverlayEntities == nullptr || !(*OverlayEntities)->TryGetObjectField(ClassPath, EntityOverlay))
		{
			OutError = FString::Printf(
				TEXT("no overlay entry for '%s'. Ownership, read policy and on-player-delete would be left unset and the ")
				TEXT("table pushed client-readable, so a renamed class silently downgrades a player-owned table to ")
				TEXT("project-shared. Add the class to 'entities' in Config/PlayServSchemaOverlay.json, or to ")
				TEXT("'excludedClassPaths' if it must not be pushed."),
				*ClassPath);
			return nullptr;
		}

		{
			(*EntityOverlay)->TryGetBoolField(TEXT("clientRead"), bClientRead);
			FString StrVal;
			if ((*EntityOverlay)->TryGetStringField(TEXT("ownedBy"), StrVal))
			{
				EntityDto->SetStringField(TEXT("owned_by"), StrVal);
			}
			if ((*EntityOverlay)->TryGetStringField(TEXT("read"), StrVal))
			{
				EntityDto->SetStringField(TEXT("read"), StrVal);
			}
			if ((*EntityOverlay)->TryGetStringField(TEXT("onPlayerDelete"), StrVal))
			{
				EntityDto->SetStringField(TEXT("on_player_delete"), StrVal);
			}
			if ((*EntityOverlay)->TryGetStringField(TEXT("description"), StrVal))
			{
				EntityDto->SetStringField(TEXT("description"), StrVal);
			}
		}

		FString OnPlayerDelete;
		if (bOwned && EntityDto->TryGetStringField(TEXT("on_player_delete"), OnPlayerDelete) && OnPlayerDelete == TEXT("anonymise"))
		{
			OutError = FString::Printf(TEXT("'%s' is PlayServPlayerOwned, and on_player_delete 'anonymise' rewrites only the owner: the deleted player's id would stay in its player_id field. Use cascade-delete."), *ClassPath);
			return nullptr;
		}

		TSharedPtr<FJsonObject> ClientAcl = MakeShared<FJsonObject>();
		ClientAcl->SetBoolField(TEXT("read"), bClientRead);
		ClientAcl->SetBoolField(TEXT("write"), bClientWritable);
		TSharedPtr<FJsonObject> Acl = MakeShared<FJsonObject>();
		Acl->SetObjectField(TEXT("client"), ClientAcl);
		EntityDto->SetObjectField(TEXT("acl"), Acl);

		TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetStringField(TEXT("code_key"), ClassPath);
		Wrapper->SetObjectField(TEXT("entity"), EntityDto);
		PushEntities.Add(MakeShared<FJsonValueObject>(Wrapper));
	}

	// Parts come from the overlay (the manifest carries no USTRUCT definitions).
	TArray<TSharedPtr<FJsonValue>> PushParts;
	const TArray<TSharedPtr<FJsonValue>>* OverlayParts;
	if (Overlay.IsValid() && Overlay->TryGetArrayField(TEXT("parts"), OverlayParts))
	{
		for (const TSharedPtr<FJsonValue>& PartValue : *OverlayParts)
		{
			const TSharedPtr<FJsonObject> OverlayPart = PartValue->AsObject();
			TSharedPtr<FJsonObject> PartDto = MakeShared<FJsonObject>();
			PartDto->SetStringField(TEXT("name"), OverlayPart->GetStringField(TEXT("name")));
			FString Description;
			if (OverlayPart->TryGetStringField(TEXT("description"), Description))
			{
				PartDto->SetStringField(TEXT("description"), Description);
			}
			TArray<TSharedPtr<FJsonValue>> PartFields;
			for (const TSharedPtr<FJsonValue>& FieldValue : OverlayPart->GetArrayField(TEXT("fields")))
			{
				const TSharedPtr<FJsonObject> OverlayField = FieldValue->AsObject();
				TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
				Field->SetStringField(TEXT("name"), OverlayField->GetStringField(TEXT("name")));
				Field->SetStringField(TEXT("type"), OverlayField->GetStringField(TEXT("type")));
				FString Target;
				if (OverlayField->TryGetStringField(TEXT("target"), Target))
				{
					Field->SetStringField(TEXT("target"), Target);
				}
				FString Cardinality;
				if (OverlayField->TryGetStringField(TEXT("cardinality"), Cardinality))
				{
					Field->SetStringField(TEXT("cardinality"), Cardinality);
				}
				Field->SetStringField(TEXT("code_key"), OverlayField->GetStringField(TEXT("name")));
				PartFields.Add(MakeShared<FJsonValueObject>(Field));
			}
			PartDto->SetArrayField(TEXT("fields"), PartFields);

			TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
			Wrapper->SetStringField(TEXT("code_key"), OverlayPart->GetStringField(TEXT("codeKey")));
			Wrapper->SetObjectField(TEXT("part"), PartDto);
			PushParts.Add(MakeShared<FJsonValueObject>(Wrapper));
		}
	}

	// Enums come from the manifest: the UHT exporter emits every UENUM an entity (or one of its
	// USTRUCT parts) uses, with the short value names the serializer writes. The platform applies
	// `enums` before parts and entities, so the enum-typed fields above resolve their `target`
	// inside the same push — no hand-created enum is a prerequisite.
	// Only enums a PUSHED field targets ride along: the exporter walks every entity in the target,
	// excluded ones included, and an enum nobody pushed uses would land as an orphan — or 409
	// against a hand-created enum of the same name that no pushed field even references.
	TSet<FString> EnumTargets;
	auto CollectEnumTargets = [&EnumTargets](const TArray<TSharedPtr<FJsonValue>>& Wrappers, const TCHAR* ElementKey)
	{
		for (const TSharedPtr<FJsonValue>& WrapperValue : Wrappers)
		{
			const TSharedPtr<FJsonObject> Element = WrapperValue->AsObject()->GetObjectField(ElementKey);
			for (const TSharedPtr<FJsonValue>& FieldValue : Element->GetArrayField(TEXT("fields")))
			{
				const TSharedPtr<FJsonObject> Field = FieldValue->AsObject();
				FString Target;
				if (Field->GetStringField(TEXT("type")) == TEXT("enum") && Field->TryGetStringField(TEXT("target"), Target))
				{
					EnumTargets.Add(Target);
				}
			}
		}
	};
	CollectEnumTargets(PushEntities, TEXT("entity"));
	CollectEnumTargets(PushParts, TEXT("part"));

	TArray<TSharedPtr<FJsonValue>> PushEnums;
	TSet<FString> UndefinedTargets = EnumTargets;
	const TArray<TSharedPtr<FJsonValue>>* ManifestEnums;
	if (Manifest->TryGetArrayField(TEXT("enums"), ManifestEnums))
	{
		for (const TSharedPtr<FJsonValue>& EnumValue : *ManifestEnums)
		{
			const TSharedPtr<FJsonObject> ManifestEnum = EnumValue->AsObject();
			FString Name;
			FString CodeKey;
			if (!ManifestEnum.IsValid() || !ManifestEnum->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()
				|| !ManifestEnum->TryGetStringField(TEXT("codeKey"), CodeKey) || CodeKey.IsEmpty())
			{
				OutError = TEXT("manifest enum without a name or codeKey — rebuild with the current PlayServUht");
				return nullptr;
			}
			if (!EnumTargets.Contains(Name))
			{
				UE_LOG(LogPlayServPusher, Display, TEXT("  enum %s: no pushed field targets it — not pushed"), *Name);
				continue;
			}
			UndefinedTargets.Remove(Name);

			TSharedPtr<FJsonObject> EnumDto = MakeShared<FJsonObject>();
			EnumDto->SetStringField(TEXT("name"), Name);
			TArray<TSharedPtr<FJsonValue>> Values;
			const TArray<TSharedPtr<FJsonValue>>* ManifestValues;
			if (ManifestEnum->TryGetArrayField(TEXT("values"), ManifestValues))
			{
				for (const TSharedPtr<FJsonValue>& Value : *ManifestValues)
				{
					Values.Add(MakeShared<FJsonValueString>(Value->AsString()));
				}
			}
			EnumDto->SetArrayField(TEXT("values"), Values);

			TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
			Wrapper->SetStringField(TEXT("code_key"), CodeKey);
			Wrapper->SetObjectField(TEXT("enum"), EnumDto);
			PushEnums.Add(MakeShared<FJsonValueObject>(Wrapper));
		}
	}
	for (const FString& Target : UndefinedTargets)
	{
		UE_LOG(LogPlayServPusher, Warning, TEXT("  enum %s is targeted by a pushed field but the manifest does not define it — it must already exist on the platform, or the push is refused"), *Target);
	}

	TSharedPtr<FJsonObject> Dto = MakeShared<FJsonObject>();
	Dto->SetArrayField(TEXT("enums"), PushEnums);
	Dto->SetArrayField(TEXT("entities"), PushEntities);
	Dto->SetArrayField(TEXT("parts"), PushParts);
	return Dto;
}

FPlayServSchemaPusher::FResult FPlayServSchemaPusher::Run(const FString& ManifestPathOverride, const FString& ServerKeyOverride, const FString& BaseURLOverride, bool bDryRun, const FString& OverlayPathOverride)
{
	FResult Result;

	const FString ManifestPath = ResolveManifestPath(ManifestPathOverride);
	if (ManifestPath.IsEmpty())
	{
		Result.Summary = TEXT("PlayServManifest.json not found under the plugin Intermediate tree — build the project first (or pass -ManifestPath=)");
		return Result;
	}
	UE_LOG(LogPlayServPusher, Display, TEXT("Manifest: %s"), *ManifestPath);

	TSharedPtr<FJsonObject> Manifest = LoadJsonFile(ManifestPath);
	if (!Manifest.IsValid())
	{
		Result.Summary = TEXT("manifest failed to parse");
		return Result;
	}
	if (static_cast<int32>(Manifest->GetNumberField(TEXT("manifestVersion"))) != 2)
	{
		Result.Summary = TEXT("manifest is not v2 — rebuild with the current PlayServUht");
		return Result;
	}

	const FString OverlayPath = OverlayPathOverride.IsEmpty() ? FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("PlayServSchemaOverlay.json")) : OverlayPathOverride;
	TSharedPtr<FJsonObject> Overlay = LoadJsonFile(OverlayPath);
	if (!Overlay.IsValid())
	{
		UE_LOG(LogPlayServPusher, Warning, TEXT("No overlay at %s — pushing without policies/parts"), *OverlayPath);
	}

	FString BaseURL = BaseURLOverride.IsEmpty() ? UPlayServSettings::GetBaseURL() : BaseURLOverride;
	BaseURL.RemoveFromEnd(TEXT("/"));
	FString ServerKey = ServerKeyOverride;
	if (ServerKey.IsEmpty())
	{
		FString PlayServIni;
		FConfigCacheIni::LoadGlobalIniFile(PlayServIni, TEXT("PlayServ"));
		GConfig->GetString(TEXT("PlayServ.DevSecrets"), TEXT("ServerKey"), ServerKey, PlayServIni);
	}
	if (BaseURL.IsEmpty() || ServerKey.IsEmpty())
	{
		Result.Summary = TEXT("BaseURL not configured (DefaultGame.ini / -PlayServBaseURL=) or ServerKey not configured (DefaultPlayServ.ini [PlayServ.DevSecrets] / -PlayServServerKey=)");
		return Result;
	}

	// 1) sk_ -> operator session (POST /api/v1/auth/cli, no body, Bearer sk_).
	int32 Status = 0;
	TSharedPtr<FJsonObject> Json;
	FString Raw;
	if (!SyncRequest(TEXT("POST"), BaseURL + TEXT("/api/v1/auth/cli"),
		{ { TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ServerKey) } }, FString(), Status, Json, Raw)
		|| Status != 200 || !Json.IsValid())
	{
		Result.Summary = FString::Printf(TEXT("auth/cli exchange failed (HTTP %d)"), Status);
		return Result;
	}
	const FString OperatorToken = Json->GetStringField(TEXT("access_token"));
	const FString ProjectSlug = Json->GetStringField(TEXT("project_slug"));
	const FString Env = Json->GetStringField(TEXT("env"));
	UE_LOG(LogPlayServPusher, Display, TEXT("Operator session for project %s env %s"), *ProjectSlug, *Env);

	const TArray<TPair<FString, FString>> OperatorHeaders = {
		{ TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *OperatorToken) },
		{ TEXT("X-Project-Slug"), ProjectSlug },
		{ TEXT("X-Env"), Env },
	};

	// 2) current schema (revision + per-element contentHash for the diff).
	if (!SyncRequest(TEXT("GET"), BaseURL + TEXT("/api/v1/schema"), OperatorHeaders, FString(), Status, Json, Raw)
		|| Status != 200 || !Json.IsValid())
	{
		Result.Summary = FString::Printf(TEXT("GET /api/v1/schema failed (HTTP %d)"), Status);
		return Result;
	}
	const FString Revision = Json->GetStringField(TEXT("revision"));
	TMap<FString, FString> HashBefore;
	for (const TCHAR* Kind : { TEXT("enums"), TEXT("entities"), TEXT("parts") })
	{
		const TArray<TSharedPtr<FJsonValue>>* Elements;
		if (Json->TryGetArrayField(Kind, Elements))
		{
			for (const TSharedPtr<FJsonValue>& ElementValue : *Elements)
			{
				const TSharedPtr<FJsonObject> Element = ElementValue->AsObject();
				HashBefore.Add(Element->GetStringField(TEXT("name")), Element->GetStringField(TEXT("content_hash")));
			}
		}
	}

	// 3) build + push.
	FString BuildError;
	TSharedPtr<FJsonObject> Dto = BuildPushDto(Manifest, Overlay, BuildError);
	if (!Dto.IsValid())
	{
		Result.Summary = BuildError;
		return Result;
	}
	Dto->SetStringField(TEXT("expected_revision"), Revision);

	if (bDryRun)
	{
		UE_LOG(LogPlayServPusher, Display, TEXT("DRY RUN - DTO built, nothing pushed:\n%s"), *JsonToString(Dto.ToSharedRef()));
		Result.bSuccess = true;
		Result.Summary = TEXT("dry run - the DTO above was NOT pushed");
		return Result;
	}

	if (!SyncRequest(TEXT("POST"), BaseURL + TEXT("/api/v1/schema:push-from-code"), OperatorHeaders,
		JsonToString(Dto.ToSharedRef()), Status, Json, Raw))
	{
		Result.Summary = TEXT("push request failed to complete");
		return Result;
	}
	if (Status != 200 || !Json.IsValid())
	{
		FString Code;
		if (Json.IsValid())
		{
			Json->TryGetStringField(TEXT("code"), Code);
		}
		Result.Summary = FString::Printf(TEXT("push refused (HTTP %d %s): %s"), Status, *Code, *Raw.Left(300));
		return Result;
	}

	// 4) diff-style report from the returned overview.
	int32 Created = 0, Updated = 0, Unchanged = 0;
	FString Diff;
	for (const TCHAR* Kind : { TEXT("enums"), TEXT("entities"), TEXT("parts") })
	{
		const TArray<TSharedPtr<FJsonValue>>* Elements;
		if (Json->TryGetArrayField(Kind, Elements))
		{
			for (const TSharedPtr<FJsonValue>& ElementValue : *Elements)
			{
				const TSharedPtr<FJsonObject> Element = ElementValue->AsObject();
				const FString Name = Element->GetStringField(TEXT("name"));
				const FString HashNow = Element->GetStringField(TEXT("content_hash"));
				const FString* Before = HashBefore.Find(Name);
				if (Before == nullptr)
				{
					++Created;
					Diff += FString::Printf(TEXT("  + %s (created)\n"), *Name);
				}
				else if (*Before != HashNow)
				{
					++Updated;
					Diff += FString::Printf(TEXT("  ~ %s (updated)\n"), *Name);
				}
				else
				{
					++Unchanged;
				}
			}
		}
	}

	Result.bSuccess = true;
	Result.Summary = FString::Printf(TEXT("Push OK: %d created, %d updated, %d unchanged. Revision %s -> %s\n%s"),
		Created, Updated, Unchanged, *Revision, *Json->GetStringField(TEXT("revision")), *Diff);
	return Result;
}
