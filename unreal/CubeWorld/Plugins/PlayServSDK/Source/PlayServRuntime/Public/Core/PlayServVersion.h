#pragma once

#include "CoreMinimal.h"

/**
 * The PlayServ SDK version, MAJOR.MINOR.PATCH. Check it at compile time:
 *   #if PLAYSERV_SDK_VERSION_NUMBER >= PLAYSERV_MAKE_VERSION(0, 7, 0)
 */

#define PLAYSERV_SDK_VERSION_MAJOR 0

#define PLAYSERV_SDK_VERSION_MINOR 7

#define PLAYSERV_SDK_VERSION_PATCH 0

/** The three components as one integer, 0x00MMNNPP, for comparisons. */
#define PLAYSERV_MAKE_VERSION(Major, Minor, Patch) \
	((Major) << 16 | (Minor) << 8 | (Patch))

/** This SDK's version as one integer. */
#define PLAYSERV_SDK_VERSION_NUMBER \
	PLAYSERV_MAKE_VERSION(PLAYSERV_SDK_VERSION_MAJOR, PLAYSERV_SDK_VERSION_MINOR, PLAYSERV_SDK_VERSION_PATCH)

#define PLAYSERV_STR_INNER(x) #x
#define PLAYSERV_STR(x) PLAYSERV_STR_INNER(x)

/** This SDK's version as text, such as TEXT("0.7.0"). */
#define PLAYSERV_SDK_VERSION \
	TEXT(PLAYSERV_STR(PLAYSERV_SDK_VERSION_MAJOR) "." PLAYSERV_STR(PLAYSERV_SDK_VERSION_MINOR) "." PLAYSERV_STR(PLAYSERV_SDK_VERSION_PATCH))
