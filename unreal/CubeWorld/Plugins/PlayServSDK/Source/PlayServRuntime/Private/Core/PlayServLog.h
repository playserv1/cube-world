#pragma once

#include "CoreMinimal.h"

// Warning: never log a token, a key, a request or response body, or a URL with its query; no test reads the log.
DECLARE_LOG_CATEGORY_EXTERN(LogPlayServ, Display, All);
