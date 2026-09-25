#include "Code/PlayServRPCParams.h"

FPlayServRPCParams::FPlayServRPCParams()
	: Params(MakeShared<FJsonObject>())
{
}

FPlayServRPCParams& FPlayServRPCParams::Set(const FString& Key, const FString& Value)
{
	Params->SetStringField(Key, Value);
	return *this;
}

FPlayServRPCParams& FPlayServRPCParams::Set(const FString& Key, int32 Value)
{
	Params->SetNumberField(Key, static_cast<double>(Value));
	return *this;
}

FPlayServRPCParams& FPlayServRPCParams::Set(const FString& Key, int64 Value)
{
	Params->SetNumberField(Key, static_cast<double>(Value));
	return *this;
}

FPlayServRPCParams& FPlayServRPCParams::Set(const FString& Key, double Value)
{
	Params->SetNumberField(Key, Value);
	return *this;
}

FPlayServRPCParams& FPlayServRPCParams::Set(const FString& Key, bool Value)
{
	Params->SetBoolField(Key, Value);
	return *this;
}

FPlayServRPCParams& FPlayServRPCParams::Set(const FString& Key, const TSharedPtr<FJsonValue>& Value)
{
	Params->SetField(Key, Value);
	return *this;
}

FPlayServRPCParams& FPlayServRPCParams::Set(const FString& Key, const TSharedPtr<FJsonObject>& Value)
{
	Params->SetObjectField(Key, Value);
	return *this;
}

TSharedPtr<FJsonObject> FPlayServRPCParams::ToJson() const
{
	return Params;
}
