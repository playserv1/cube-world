#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "PlayServSettings.generated.h"

/**
 * Project settings for the PlayServ SDK, under Project Settings > Game > PlayServ, stored in Config/DefaultGame.ini
 * (section /Script/PlayServRuntime.PlayServSettings). The server key goes in Config/DedicatedServerGame.ini, which the
 * engine loads only in a dedicated server and packages only into server builds. A command-line value wins over an
 * environment variable, which wins over the ini; the deployment token is read from the environment only.
 */
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="PlayServ"))
class PLAYSERVRUNTIME_API UPlayServSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/**
	 * Base URL of the PlayServ platform this build talks to, such as https://<your-platform-host>. Override with
	 * -PlayServBaseURL= or the PLAYSERV_API_URL environment variable.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Connection")
	FString BaseURL;

	/** Client key (pk_*): the public credential game clients use, one per environment; safe to ship. Override with -PlayServClientKey=. */
	UPROPERTY(Config, EditAnywhere, Category="Connection")
	FString ClientKey;

	/**
	 * Server key (sk_*): the dedicated server's secret, for PlayServ::Auth::LoginServer(). Set it only in
	 * Config/DedicatedServerGame.ini, or per launch with -PlayServServerKey= or PLAYSERV_SERVER_KEY. A server key in a
	 * process that is not a dedicated server is logged as an error; the key itself is never logged.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Connection|Dedicated Server", meta=(PasswordField=true))
	FString ServerKey;

	/**
	 * The project's default room type. A dedicated server registers its rooms under it, and the Browse and JoinRoom
	 * overloads without a slug use it; the overloads with one reach other room types. Not a secret. Override with
	 * -PlayServRoomDefaultSlug= or PLAYSERV_EXECUTOR_SLUG.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Connection")
	FString RoomDefaultSlug;

	/** Default region label for rooms that declare none; at most 32 characters. */
	UPROPERTY(Config, EditAnywhere, Category="Connection|Dedicated Server")
	FString RoomRegion;

	/** Development only: admit players whose room ticket cannot be verified. They get no PlayServ identity. Ignored in Shipping builds. */
	UPROPERTY(Config, EditAnywhere, Category="Connection|Dedicated Server")
	bool bAdmissionFailOpenInDevelopment = false;

	/** Seconds before a PlayServ request fails with EPlayServErrorCode::Timeout; 0 uses the engine default. No launch override. */
	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(ClampMin="0.0"))
	float RequestTimeoutSeconds = 20.0f;

	/** The settings in effect, overrides applied. */
	static const UPlayServSettings* Get();

	/** BaseURL, overrides applied. */
	static FString GetBaseURL();

	/** ClientKey, overrides applied. */
	static FString GetClientKey();

	/** ServerKey, overrides applied; empty when none is configured. */
	static FString GetServerKey();

	/** RoomDefaultSlug, overrides applied. PlayServ hosting sets PLAYSERV_EXECUTOR_SLUG for a server it starts. */
	static FString GetRoomDefaultSlug();

	/** The deployment token PlayServ hosting gives a server it starts (PLAYSERV_DEPLOYMENT_TOKEN); empty for a server the studio runs. */
	static FString GetDeploymentToken();

	/** The room PlayServ hosting started this process for (-PlayServRoomName= or PLAYSERV_ROOM_NAME). */
	static FString GetLaunchRoomName();

	static FString GetRoomRegion();
	static bool IsAdmissionFailOpenInDevelopment();

	/** Apply the -PlayServ…= command-line overrides and the PLAYSERV_* environment variables. No secret is logged. */
	static void ApplyCommandLineOverrides();

	/** Warn about missing connection settings, and log an error for a server key outside a dedicated server. */
	static void ValidateSettings();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	static FString ResolveServerKey(const FString& CommandLineValue, const FString& EnvironmentValue, const FString& IniValue);

	static FString ResolveLaunchValue(const FString& CommandLineValue, const FString& EnvironmentValue);

	static int32 ParseLaunchPort(const FString& Text);

	friend class UPlayServRooms;

	static int32 GetLaunchListenPort();

	static FString GetLaunchPublicHost();

	static FString GetLaunchAttributesJson();

	FString DeploymentToken;

	FString LaunchRoomName;

	int32 LaunchListenPort = 0;

	FString LaunchPublicHost;

	FString LaunchAttributesJson;

#if !UE_BUILD_SHIPPING
	friend struct FPlayServSettingsTestAccess;
#endif
};
