#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "PlayServSchemaPusher.h"

#include "Framework/Notifications/NotificationManager.h"
#include "ToolMenus.h"
#include "Widgets/Notifications/SNotificationList.h"

// Editor-side tooling for the PlayServ SDK. Registers Tools -> PlayServ -> Push Schemas,
// which runs the same pusher code path as the PlayServPushSchemas commandlet.
class FPlayServEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FPlayServEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("PlayServ", INVTEXT("PlayServ"));
		Section.AddMenuEntry(
			"PlayServPushSchemas",
			INVTEXT("Push Schemas to Platform"),
			INVTEXT("Convert PlayServManifest.json (+ overlay) to a schema push and apply it to the configured PlayServ environment. Same code path as -run=PlayServPushSchemas."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				// Deliberately synchronous — an operator-initiated action; the two round
				// trips (~2 s) block briefly and keep one code path with the commandlet.
				const FPlayServSchemaPusher::FResult Result = FPlayServSchemaPusher::Run(FString(), FString(), FString());

				FNotificationInfo Info(FText::FromString(Result.bSuccess
					? FString::Printf(TEXT("PlayServ schema push succeeded.\n%s"), *Result.Summary.Left(400))
					: FString::Printf(TEXT("PlayServ schema push FAILED: %s"), *Result.Summary.Left(400))));
				Info.ExpireDuration = Result.bSuccess ? 6.0f : 12.0f;
				Info.bUseSuccessFailIcons = true;
				TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
				if (Notification.IsValid())
				{
					Notification->SetCompletionState(Result.bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
				}
			})));
	}
};

IMPLEMENT_MODULE(FPlayServEditorModule, PlayServEditor)
