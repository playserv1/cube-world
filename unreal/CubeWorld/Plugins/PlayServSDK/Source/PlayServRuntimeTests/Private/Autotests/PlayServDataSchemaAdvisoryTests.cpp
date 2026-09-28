#include "PlayServTestAccess.h"
#include "Misc/AutomationTest.h"
#include "TestEntities.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Data.SchemaAdvisory.TableNameIsTheClassName
//
// The startup advisory warns about entity classes with no platform table. It derived the table
// name from the codegen class path and then stripped a leading U or A — but a class path already
// carries the engine name, which has no prefix. Every entity whose name starts with U or A was
// reported as missing its table ("toneBackendFactionReputation" for AtoneBackendFactionReputation).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSchemaAdvisoryTableNameTest,
	"PlayServ.Data.SchemaAdvisory.TableNameIsTheClassName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSchemaAdvisoryTableNameTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a name starting with A keeps its first letter"), FPlayServDataTestAccess::SchemaNameForClassPath(TEXT("/Script/Atone.AtoneBackendFactionReputation")), FString(TEXT("AtoneBackendFactionReputation")));
	TestEqual(TEXT("a name starting with U keeps its first letter"), FPlayServDataTestAccess::SchemaNameForClassPath(TEXT("/Script/Game.UserProfile")), FString(TEXT("UserProfile")));
	TestEqual(TEXT("any other name is unchanged"), FPlayServDataTestAccess::SchemaNameForClassPath(TEXT("/Script/UnrealTestProject.BlobPlayerProfile")), FString(TEXT("BlobPlayerProfile")));
	TestEqual(TEXT("the advisory checks the same name data calls resolve"), FPlayServDataTestAccess::SchemaNameForClassPath(UTestPlayer::StaticClass()->GetPathName()), UTestPlayer::StaticClass()->GetName());
	return true;
}

#endif // !UE_BUILD_SHIPPING
