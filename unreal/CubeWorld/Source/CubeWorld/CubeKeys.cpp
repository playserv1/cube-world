#include "CubeKeys.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/InputSettings.h"
#include "HAL/PlatformApplicationMisc.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
	bool IsDown(const FKey& Key)
	{
#if PLATFORM_WINDOWS
		if (!Key.IsValid() || Key.IsGamepadKey() || Key.IsMouseButton()) return false;
		const uint32* KeyCode = nullptr;
		const uint32* CharCode = nullptr;
		FInputKeyManager::Get().GetCodesFromKey(Key, KeyCode, CharCode);
		const uint32 Code = KeyCode ? *KeyCode : (CharCode ? *CharCode : 0);
		return Code != 0 && (::GetAsyncKeyState((int)Code) & 0x8000) != 0;
#else
		return false;
#endif
	}

	float Axis(const UInputSettings* Settings, FName Name)
	{
		TArray<FInputAxisKeyMapping> Mappings;
		Settings->GetAxisMappingByName(Name, Mappings);
		float Sum = 0;
		for (const FInputAxisKeyMapping& M : Mappings) if (IsDown(M.Key)) Sum += M.Scale;
		return FMath::Clamp(Sum, -1.f, 1.f);
	}

	bool Action(const UInputSettings* Settings, FName Name)
	{
		TArray<FInputActionKeyMapping> Mappings;
		Settings->GetActionMappingByName(Name, Mappings);
		for (const FInputActionKeyMapping& M : Mappings) if (IsDown(M.Key)) return true;
		return false;
	}

	/** Adds up the mouse's raw movement as Slate hands it on, and lets it go on to the game. */
	class FMouseTap : public IInputProcessor
	{
	public:
		FVector2D Delta = FVector2D::ZeroVector;
		virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}
		virtual bool HandleMouseMoveEvent(FSlateApplication&, const FPointerEvent& Event) override
		{
			// The viewport turns a cursor delta into MouseX = x and MouseY = -y (FSceneViewport).
			const FVector2D D = Event.GetCursorDelta();
			Delta += FVector2D(D.X, -D.Y);
			return false;
		}
		virtual const TCHAR* GetDebugName() const override { return TEXT("CubeMouseTap"); }
	};

	TSharedPtr<FMouseTap> Tap;
}

bool CubeKeys::Read(FCubeKeys& Out)
{
	// -fakekeys: W and Shift held on a keyboard nobody touches, for testing a crossing without the game in front.
	static const bool bFake = FParse::Param(FCommandLine::Get(), TEXT("fakekeys"));
	// -fakejump: Space held too, to climb out of craters on the way.
	static const bool bFakeJump = FParse::Param(FCommandLine::Get(), TEXT("fakejump"));
	if (bFake) { Out = FCubeKeys(); Out.Forward = 1; Out.bSprint = true; Out.bJump = bFakeJump; return true; }
#if PLATFORM_WINDOWS
	const UInputSettings* Settings = UInputSettings::GetInputSettings();
	if (!Settings || !FPlatformApplicationMisc::IsThisApplicationForeground()) return false;
	Out.Forward = Axis(Settings, TEXT("MoveForward"));
	Out.Strafe = Axis(Settings, TEXT("MoveRight"));
	Out.bJump = Action(Settings, TEXT("Jump"));
	Out.bSprint = Action(Settings, TEXT("Sprint"));
	Out.bSneak = Action(Settings, TEXT("Sneak"));
	return true;
#else
	return false;
#endif
}

void CubeKeys::StartMouse()
{
	if (!FSlateApplication::IsInitialized()) return;
	if (!Tap.IsValid())
	{
		Tap = MakeShared<FMouseTap>();
		FSlateApplication::Get().RegisterInputPreProcessor(Tap);
	}
	Tap->Delta = FVector2D::ZeroVector;
}

FVector2D CubeKeys::TakeMouse()
{
	if (!Tap.IsValid()) return FVector2D::ZeroVector;
	const FVector2D D = Tap->Delta;
	Tap->Delta = FVector2D::ZeroVector;
	return D;
}

void CubeKeys::StopMouse()
{
	if (Tap.IsValid() && FSlateApplication::IsInitialized()) FSlateApplication::Get().UnregisterInputPreProcessor(Tap);
	Tap.Reset();
}
