namespace PlayServ::Code
{
	template<typename TRequest, typename TResponse>
	void Call(const FString& FunctionName, const TRequest& Request, TFunction<void(bool, const TResponse&, const FPlayServError&)> Callback)
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS)
		{
			if (Callback)
			{
				Callback(false, TResponse{}, FPlayServError::SubsystemUnavailable());
			}
			return;
		}

		FPlayServRPCParams Params;
		if (!StructToRpcParams(TRequest::StaticStruct(), &Request, Params))
		{
			if (Callback)
			{
				Callback(false, TResponse{}, FPlayServError::Make(EPlayServErrorCode::ContractMismatch, TEXT("Failed to serialize request struct")));
			}
			return;
		}

		PS->GetCode()->Call(FunctionName, Params, FPlayServRPCCallback::CreateLambda(
			[Callback, FunctionName](bool bSuccess, const FPlayServRPCResult& Result, const FPlayServError& Error)
			{
				if (!Callback)
				{
					return;
				}
				TResponse Out{};
				if (!bSuccess)
				{
					Callback(false, Out, Error);
					return;
				}
				if (Result.Response.IsValid() && !JsonObjectToStruct(Result.Response, TResponse::StaticStruct(), &Out))
				{
					Callback(false, TResponse{}, FPlayServError::Make(EPlayServErrorCode::ContractMismatch, FString::Printf(TEXT("Failed to deserialize response of '%s'"), *FunctionName)));
					return;
				}
				Callback(true, Out, Error);
			}));
	}

	template<typename TRequest>
	void Call(const FString& FunctionName, const TRequest& Request, TFunction<void(bool, const FPlayServError&)> Callback)
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS)
		{
			if (Callback)
			{
				Callback(false, FPlayServError::SubsystemUnavailable());
			}
			return;
		}

		FPlayServRPCParams Params;
		if (!StructToRpcParams(TRequest::StaticStruct(), &Request, Params))
		{
			if (Callback)
			{
				Callback(false, FPlayServError::Make(EPlayServErrorCode::ContractMismatch, TEXT("Failed to serialize request struct")));
			}
			return;
		}

		PS->GetCode()->Call(FunctionName, Params, FPlayServRPCCallback::CreateLambda(
			[Callback](bool bSuccess, const FPlayServRPCResult&, const FPlayServError& Error)
			{
				if (Callback)
				{
					Callback(bSuccess, Error);
				}
			}));
	}
}
