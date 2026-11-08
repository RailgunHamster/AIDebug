/*
 * MinHook - Minimalistic API Hook Library for x64/x86
 * Embedded subset for Godot ai_debug module (Windows x64).
 */

#pragma once

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MH_STATUS {
	MH_UNKNOWN = -1,
	MH_OK = 0,
	MH_ERROR_ALREADY_INITIALIZED,
	MH_ERROR_NOT_INITIALIZED,
	MH_ERROR_ALREADY_CREATED,
	MH_ERROR_NOT_CREATED,
	MH_ERROR_ENABLED,
	MH_ERROR_DISABLED,
	MH_ERROR_NOT_EXECUTABLE,
	MH_ERROR_UNSUPPORTED_FUNCTION,
	MH_ERROR_MEMORY_ALLOC,
	MH_ERROR_MEMORY_PROTECT,
	MH_ERROR_MODULE_NOT_FOUND,
	MH_ERROR_FUNCTION_NOT_FOUND
} MH_STATUS;

#define MH_ALL_HOOKS NULL

MH_STATUS WINAPI MH_Initialize(void);
MH_STATUS WINAPI MH_Uninitialize(void);
MH_STATUS WINAPI MH_CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID *ppOriginal);
MH_STATUS WINAPI MH_RemoveHook(LPVOID pTarget);
MH_STATUS WINAPI MH_EnableHook(LPVOID pTarget);
MH_STATUS WINAPI MH_DisableHook(LPVOID pTarget);
const char *WINAPI MH_StatusToString(MH_STATUS status);

#ifdef __cplusplus
}
#endif

#endif // _WIN32 || _WIN64
