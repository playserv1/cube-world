#include "PlayServTestCommon.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Conventions.* — rules for what the plugin delivers to a studio.
//
// The export (Scripts/Export-PlayServSDK.ps1) delivers the plugin folder without Binaries, Intermediate, the test
// module and the editor module, plus the customer reference. Two rules hold for those files:
//
// - Private code carries no prose. Public declarations keep their doc comments; code under Private/ and the header
//   tool plugin carries only one-line `// Warning:` comments, where the code invites a change that breaks it and no
//   test can catch it. Why the code is shaped the way it is lives in the specs.
// - Nothing delivered names anything internal: ticket keys, ADRs, stories and epics, repository paths, customers,
//   the demo game, hosting vendors.
//
// Both read the source tree the editor was built from.
// ---------------------------------------------------------------------------

namespace PlayServConventionsTest
{
	struct FComment
	{
		int32 Line = 0;
		FString Text;
	};

	FString PluginDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PlayServSDK"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()) : FString();
	}

	TArray<FString> FilesUnder(const FString& Dir, const TArray<FString>& Extensions)
	{
		TArray<FString> Files;
		for (const FString& Extension : Extensions)
		{
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *Dir, *FString::Printf(TEXT("*.%s"), *Extension), true, false);
			Files.Append(Found);
		}
		Files.RemoveAll([](const FString& File)
		{
			return File.Contains(TEXT("/bin/")) || File.Contains(TEXT("/obj/"));
		});
		Files.Sort();
		return Files;
	}

	int32 SkipQuoted(const FString& Source, int32 Index, TCHAR Quote)
	{
		int32 Cursor = Index + 1;
		while (Cursor < Source.Len() && Source[Cursor] != Quote && Source[Cursor] != TEXT('\n'))
		{
			Cursor += Source[Cursor] == TEXT('\\') ? 2 : 1;
		}
		return Cursor + 1;
	}

	TArray<FComment> CommentsOf(const FString& Source, bool bCSharp)
	{
		TArray<FComment> Comments;
		int32 Line = 1;
		int32 Index = 0;
		while (Index < Source.Len())
		{
			const TCHAR Char = Source[Index];
			const TCHAR Next = Index + 1 < Source.Len() ? Source[Index + 1] : TEXT('\0');
			if (Char == TEXT('\n'))
			{
				++Line;
				++Index;
			}
			else if (Char == TEXT('/') && Next == TEXT('/'))
			{
				int32 End = Source.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
				End = End == INDEX_NONE ? Source.Len() : End;
				Comments.Add({ Line, Source.Mid(Index, End - Index).TrimEnd() });
				Index = End;
			}
			else if (Char == TEXT('/') && Next == TEXT('*'))
			{
				int32 End = Source.Find(TEXT("*/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 2);
				End = End == INDEX_NONE ? Source.Len() : End + 2;
				const FString Text = Source.Mid(Index, End - Index);
				Comments.Add({ Line, Text.Left(120) });
				for (const TCHAR Inner : Text)
				{
					Line += Inner == TEXT('\n') ? 1 : 0;
				}
				Index = End;
			}
			else if (!bCSharp && Char == TEXT('R') && Next == TEXT('"') && (Index == 0 || !FChar::IsIdentifier(Source[Index - 1])))
			{
				const int32 Open = Source.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
				const FString Close = TEXT(")") + Source.Mid(Index + 2, Open - Index - 2) + TEXT("\"");
				int32 End = Source.Find(Close, ESearchCase::CaseSensitive, ESearchDir::FromStart, Open);
				End = End == INDEX_NONE ? Source.Len() : End + Close.Len();
				for (int32 Inner = Index; Inner < End; ++Inner)
				{
					Line += Source[Inner] == TEXT('\n') ? 1 : 0;
				}
				Index = End;
			}
			else if (bCSharp && Char == TEXT('@') && Next == TEXT('"'))
			{
				int32 Cursor = Index + 2;
				while (Cursor < Source.Len() && !(Source[Cursor] == TEXT('"') && (Cursor + 1 >= Source.Len() || Source[Cursor + 1] != TEXT('"'))))
				{
					Line += Source[Cursor] == TEXT('\n') ? 1 : 0;
					Cursor += Source[Cursor] == TEXT('"') ? 2 : 1;
				}
				Index = Cursor + 1;
			}
			else if (Char == TEXT('"') || Char == TEXT('\''))
			{
				Index = SkipQuoted(Source, Index, Char);
			}
			else
			{
				++Index;
			}
		}
		return Comments;
	}

	FString Relative(const FString& File, const FString& Root)
	{
		FString Path = File;
		FPaths::MakePathRelativeTo(Path, *(Root + TEXT("/")));
		return Path;
	}
}

// ---------------------------------------------------------------------------
// PlayServ.Conventions.PrivateCodeCarriesOnlyWarnings
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServConventionsPrivateCodeCarriesOnlyWarningsTest,
	"PlayServ.Conventions.PrivateCodeCarriesOnlyWarnings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServConventionsPrivateCodeCarriesOnlyWarningsTest::RunTest(const FString& Parameters)
{
	using namespace PlayServConventionsTest;

	const FString Plugin = PluginDir();
	if (Plugin.IsEmpty())
	{
		AddError(TEXT("the PlayServSDK plugin is not loaded"));
		return false;
	}

	TArray<FString> Files = FilesUnder(Plugin / TEXT("Source/PlayServRuntime/Private"), { TEXT("h"), TEXT("cpp"), TEXT("inl") });
	Files.Append(FilesUnder(Plugin / TEXT("Source/PlayServUht"), { TEXT("cs") }));
	TestTrue(TEXT("the private sources are found"), Files.Num() > 40);

	for (const FString& File : Files)
	{
		FString Source;
		if (!FFileHelper::LoadFileToString(Source, *File))
		{
			AddError(FString::Printf(TEXT("cannot read %s"), *File));
			continue;
		}
		for (const FComment& Comment : CommentsOf(Source, File.EndsWith(TEXT(".cs"))))
		{
			if (!Comment.Text.StartsWith(TEXT("// Warning: ")))
			{
				AddError(FString::Printf(TEXT("%s:%d is a comment but not a one-line warning: %s"), *Relative(File, Plugin), Comment.Line, *Comment.Text.Left(120)));
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Conventions.DeliveredFilesNameNothingInternal
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServConventionsDeliveredFilesNameNothingInternalTest,
	"PlayServ.Conventions.DeliveredFilesNameNothingInternal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServConventionsDeliveredFilesNameNothingInternalTest::RunTest(const FString& Parameters)
{
	using namespace PlayServConventionsTest;

	const FString Plugin = PluginDir();
	if (Plugin.IsEmpty())
	{
		AddError(TEXT("the PlayServSDK plugin is not loaded"));
		return false;
	}

	TArray<FString> Files = FilesUnder(Plugin / TEXT("Source/PlayServRuntime"), { TEXT("h"), TEXT("cpp"), TEXT("inl"), TEXT("cs") });
	Files.Append(FilesUnder(Plugin / TEXT("Source/PlayServUht"), { TEXT("cs"), TEXT("csproj"), TEXT("props") }));
	Files.Append(FilesUnder(Plugin / TEXT("Docs"), { TEXT("md") }));
	Files.Append(FilesUnder(Plugin / TEXT("Config"), { TEXT("ini") }));
	for (const TCHAR* Name : { TEXT("PlayServSDK.uplugin"), TEXT("README.md") })
	{
		if (FPaths::FileExists(Plugin / Name))
		{
			Files.Add(Plugin / Name);
		}
	}
	TestTrue(TEXT("the delivered files are found"), Files.Num() > 60);

	const FRegexPattern Internal(TEXT("PSV-[0-9]+|ADR-[0-9]+|\\b(Story|Epic) [0-9]+|(^|[\\s`'\"(\\[])(Specs|contracts|docs|backend|unreal)/|playserv-platform|\\bAtone\\b|Skeldar|Drive ?State|\\bBlob[A-Z. ]|\\bblob-|Edgegap|ARBITRIUM"));
	for (const FString& File : Files)
	{
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *File))
		{
			AddError(FString::Printf(TEXT("cannot read %s"), *File));
			continue;
		}
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			FRegexMatcher Matcher(Internal, Lines[Index]);
			if (Matcher.FindNext())
			{
				AddError(FString::Printf(TEXT("%s:%d names something internal ('%s'): %s"), *Relative(File, Plugin), Index + 1, *Matcher.GetCaptureGroup(0), *Lines[Index].TrimStartAndEnd().Left(120)));
			}
		}
	}
	return true;
}

#endif // !UE_BUILD_SHIPPING
