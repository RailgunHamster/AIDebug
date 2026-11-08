/*
 * MinHook x86_64 implementation for Windows.
 */

#include "MinHook.h"
#include "hde64.h"

#if defined(_WIN64)

#include <stdint.h>
#include <string.h>

#define MAX_HOOK_ENTRIES 128
#define TRAMPOLINE_MAX_SIZE 64
#define RELAY_JMP_SIZE 14

#pragma pack(push, 1)
typedef struct {
	uint8_t opcode; /* 0xE9 */
	int32_t operand;
} JMP_REL;

typedef struct {
	uint8_t opcode0; /* 0xFF */
	uint8_t opcode1; /* 0x25 */
	uint32_t dummy;  /* 0x00000000 */
	uint64_t address;
} JMP_ABS;

typedef struct {
	uint8_t opcode0; /* 0xFF */
	uint8_t opcode1; /* 0x15 */
	uint32_t offset; /* 0x00000002 */
	uint8_t jmp_op;  /* 0xEB */
	uint8_t jmp_len; /* 0x08 */
	uint64_t address;
} CALL_ABS;

typedef struct {
	uint8_t opcode;      /* 0x70..0x7F (inverted condition) */
	uint8_t offset;      /* 0x0E */
	uint8_t jmp_op0;     /* 0xFF */
	uint8_t jmp_op1;     /* 0x25 */
	uint32_t jmp_dummy;  /* 0x00000000 */
	uint64_t address;
} JCC_ABS;

typedef struct {
	uint8_t trampoline[TRAMPOLINE_MAX_SIZE];
	JMP_ABS relay;
} MEMORY_SLOT;
#pragma pack(pop)

typedef struct {
	LPVOID pTarget;
	LPVOID pDetour;
	LPVOID pTrampoline;
	MEMORY_SLOT *pSlot;
	uint8_t backup[16];
	uint8_t patchSize;
	int isEnabled;
	int inUse;
} HOOK_ENTRY;

static CRITICAL_SECTION g_cs;
static int g_initialized = 0;
static HOOK_ENTRY g_hooks[MAX_HOOK_ENTRIES];

static MEMORY_SLOT *AllocateNearSlot(LPVOID pOrigin) {
	SYSTEM_INFO si;
	GetSystemInfo(&si);

	uint64_t origin = (uint64_t)pOrigin;
	uint64_t minAddr = (uint64_t)si.lpMinimumApplicationAddress;
	uint64_t maxAddr = (uint64_t)si.lpMaximumApplicationAddress;

	/* Stay within +- 1.5 GB (0x60000000) so 32-bit signed displacement always fits */
	const uint64_t maxRange = 0x60000000ULL;
	if (origin > maxRange && origin - maxRange > minAddr) {
		minAddr = origin - maxRange;
	}
	if (origin + maxRange < maxAddr) {
		maxAddr = origin + maxRange;
	}

	uint64_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;

	/* Search downwards from origin */
	uint64_t addr = (origin & ~(gran - 1));
	while (addr >= minAddr) {
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) {
			break;
		}
		if (mbi.State == MEM_FREE) {
			uint64_t allocAddr = ((uint64_t)mbi.BaseAddress + (gran - 1)) & ~(gran - 1);
			if (allocAddr >= minAddr && allocAddr <= origin) {
				void *p = VirtualAlloc((LPVOID)allocAddr, sizeof(MEMORY_SLOT), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (p) {
					return (MEMORY_SLOT *)p;
				}
			}
		}
		if ((uint64_t)mbi.BaseAddress < gran) {
			break;
		}
		addr = (uint64_t)mbi.BaseAddress - gran;
	}

	/* Search upwards from origin */
	addr = (origin + gran) & ~(gran - 1);
	while (addr <= maxAddr) {
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) {
			break;
		}
		if (mbi.State == MEM_FREE) {
			uint64_t allocAddr = ((uint64_t)mbi.BaseAddress + (gran - 1)) & ~(gran - 1);
			if (allocAddr <= maxAddr) {
				void *p = VirtualAlloc((LPVOID)allocAddr, sizeof(MEMORY_SLOT), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (p) {
					return (MEMORY_SLOT *)p;
				}
			}
		}
		addr = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
	}

	return NULL;
}

static int BuildTrampoline(LPVOID pTarget, LPVOID pDetour, MEMORY_SLOT *pSlot, uint8_t *outPatchSize) {
	const uint8_t *src = (const uint8_t *)pTarget;
	uint8_t *dst = pSlot->trampoline;
	uint32_t oldPos = 0;
	uint32_t newPos = 0;
	int finished = 0;

	while (!finished && oldPos < sizeof(JMP_REL)) {
		hde64s hs;
		unsigned int len = hde64_disasm(src + oldPos, &hs);
		if ((hs.flags & F_ERROR) || len == 0) {
			return 0;
		}

		const uint8_t *inst = src + oldPos;
		if (hs.opcode == 0xE8) {
			/* CALL rel32 */
			uint64_t dest = (uint64_t)inst + hs.len + (int32_t)hs.imm.imm32;
			CALL_ABS callAbs = { 0xFF, 0x15, 2, 0xEB, 8, dest };
			if (newPos + sizeof(callAbs) > TRAMPOLINE_MAX_SIZE - sizeof(JMP_ABS)) {
				return 0;
			}
			memcpy(dst + newPos, &callAbs, sizeof(callAbs));
			newPos += sizeof(callAbs);
		} else if (hs.opcode == 0xE9 || hs.opcode == 0xEB) {
			/* JMP rel32 / rel8 */
			int32_t offset = (hs.opcode == 0xEB) ? (int8_t)hs.imm.imm8 : (int32_t)hs.imm.imm32;
			uint64_t dest = (uint64_t)inst + hs.len + offset;
			JMP_ABS jmpAbs = { 0xFF, 0x25, 0, dest };
			if (newPos + sizeof(jmpAbs) > TRAMPOLINE_MAX_SIZE) {
				return 0;
			}
			memcpy(dst + newPos, &jmpAbs, sizeof(jmpAbs));
			newPos += sizeof(jmpAbs);
			finished = 1;
		} else if ((hs.opcode >= 0x70 && hs.opcode <= 0x7F) ||
				   (hs.opcode == 0x0F && hs.opcode2 >= 0x80 && hs.opcode2 <= 0x8F)) {
			/* Jcc rel8 / rel32 */
			int32_t offset = (hs.opcode == 0x0F) ? (int32_t)hs.imm.imm32 : (int8_t)hs.imm.imm8;
			uint64_t dest = (uint64_t)inst + hs.len + offset;
			uint8_t cond = (hs.opcode == 0x0F ? hs.opcode2 : hs.opcode) & 0x0F;
			JCC_ABS jccAbs = { (uint8_t)(0x70 | (cond ^ 1)), 14, 0xFF, 0x25, 0, dest };
			if (newPos + sizeof(jccAbs) > TRAMPOLINE_MAX_SIZE - sizeof(JMP_ABS)) {
				return 0;
			}
			memcpy(dst + newPos, &jccAbs, sizeof(jccAbs));
			newPos += sizeof(jccAbs);
		} else if ((hs.flags & F_MODRM) && (hs.modrm & 0xC7) == 0x05) {
			/* RIP-relative addressing [rip + disp32] */
			if (newPos + hs.len > TRAMPOLINE_MAX_SIZE - sizeof(JMP_ABS)) {
				return 0;
			}
			memcpy(dst + newPos, inst, hs.len);
			uint64_t targetAddr = (uint64_t)inst + hs.len + (int32_t)hs.disp.disp32;
			int64_t newDisp = (int64_t)targetAddr - (int64_t)((uint64_t)(dst + newPos) + hs.len);
			if (newDisp < INT32_MIN || newDisp > INT32_MAX) {
				return 0;
			}
			int32_t disp32 = (int32_t)newDisp;
			memcpy(dst + newPos + hs.disp_offset, &disp32, 4);
			newPos += hs.len;
			/* Indirect JMP [rip+disp32] (FF 25) ends basic block */
			if (hs.opcode == 0xFF && hs.modrm_reg == 4) {
				finished = 1;
			}
		} else {
			if (newPos + hs.len > TRAMPOLINE_MAX_SIZE - sizeof(JMP_ABS)) {
				return 0;
			}
			memcpy(dst + newPos, inst, hs.len);
			newPos += hs.len;
			if (hs.opcode == 0xC2 || hs.opcode == 0xC3) {
				finished = 1;
			}
		}

		oldPos += hs.len;
	}

	if (oldPos < sizeof(JMP_REL)) {
		return 0;
	}

	if (!finished) {
		JMP_ABS jmpBack = { 0xFF, 0x25, 0, (uint64_t)src + oldPos };
		memcpy(dst + newPos, &jmpBack, sizeof(jmpBack));
		newPos += sizeof(jmpBack);
	}

	pSlot->relay.opcode0 = 0xFF;
	pSlot->relay.opcode1 = 0x25;
	pSlot->relay.dummy = 0;
	pSlot->relay.address = (uint64_t)pDetour;

	*outPatchSize = (uint8_t)oldPos;
	return 1;
}

MH_STATUS WINAPI MH_Initialize(void) {
	if (g_initialized) {
		return MH_ERROR_ALREADY_INITIALIZED;
	}
	InitializeCriticalSection(&g_cs);
	memset(g_hooks, 0, sizeof(g_hooks));
	g_initialized = 1;
	return MH_OK;
}

MH_STATUS WINAPI MH_Uninitialize(void) {
	if (!g_initialized) {
		return MH_ERROR_NOT_INITIALIZED;
	}
	MH_DisableHook(MH_ALL_HOOKS);
	EnterCriticalSection(&g_cs);
	for (int i = 0; i < MAX_HOOK_ENTRIES; i++) {
		if (g_hooks[i].inUse) {
			if (g_hooks[i].pSlot) {
				VirtualFree(g_hooks[i].pSlot, 0, MEM_RELEASE);
			}
			memset(&g_hooks[i], 0, sizeof(HOOK_ENTRY));
		}
	}
	LeaveCriticalSection(&g_cs);
	DeleteCriticalSection(&g_cs);
	g_initialized = 0;
	return MH_OK;
}

MH_STATUS WINAPI MH_CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID *ppOriginal) {
	if (!g_initialized) {
		return MH_ERROR_NOT_INITIALIZED;
	}
	if (!pTarget || !pDetour) {
		return MH_ERROR_UNSUPPORTED_FUNCTION;
	}

	EnterCriticalSection(&g_cs);
	int freeIdx = -1;
	for (int i = 0; i < MAX_HOOK_ENTRIES; i++) {
		if (g_hooks[i].inUse && g_hooks[i].pTarget == pTarget) {
			LeaveCriticalSection(&g_cs);
			return MH_ERROR_ALREADY_CREATED;
		}
		if (!g_hooks[i].inUse && freeIdx < 0) {
			freeIdx = i;
		}
	}
	if (freeIdx < 0) {
		LeaveCriticalSection(&g_cs);
		return MH_ERROR_MEMORY_ALLOC;
	}

	MEMORY_SLOT *pSlot = AllocateNearSlot(pTarget);
	if (!pSlot) {
		LeaveCriticalSection(&g_cs);
		return MH_ERROR_MEMORY_ALLOC;
	}

	uint8_t patchSize = 0;
	if (!BuildTrampoline(pTarget, pDetour, pSlot, &patchSize) || patchSize < 5 || patchSize > 16) {
		VirtualFree(pSlot, 0, MEM_RELEASE);
		LeaveCriticalSection(&g_cs);
		return MH_ERROR_UNSUPPORTED_FUNCTION;
	}

	HOOK_ENTRY *entry = &g_hooks[freeIdx];
	entry->pTarget = pTarget;
	entry->pDetour = pDetour;
	entry->pTrampoline = pSlot->trampoline;
	entry->pSlot = pSlot;
	entry->patchSize = patchSize;
	memcpy(entry->backup, pTarget, patchSize);
	entry->isEnabled = 0;
	entry->inUse = 1;

	FlushInstructionCache(GetCurrentProcess(), pSlot, sizeof(MEMORY_SLOT));

	if (ppOriginal) {
		*ppOriginal = entry->pTrampoline;
	}

	LeaveCriticalSection(&g_cs);
	return MH_OK;
}

static MH_STATUS EnableOneHook(HOOK_ENTRY *entry, int enable) {
	if (entry->isEnabled == enable) {
		return MH_OK;
	}

	DWORD oldProtect = 0;
	if (!VirtualProtect(entry->pTarget, entry->patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		return MH_ERROR_MEMORY_PROTECT;
	}

	if (enable) {
		int64_t rel = (int64_t)&entry->pSlot->relay - ((int64_t)entry->pTarget + sizeof(JMP_REL));
		if (rel < INT32_MIN || rel > INT32_MAX) {
			VirtualProtect(entry->pTarget, entry->patchSize, oldProtect, &oldProtect);
			return MH_ERROR_UNSUPPORTED_FUNCTION;
		}
		uint8_t patchBuf[16];
		memcpy(patchBuf, entry->pTarget, sizeof(patchBuf));
		patchBuf[0] = 0xE9;
		int32_t rel32 = (int32_t)rel;
		memcpy(patchBuf + 1, &rel32, 4);
		for (uint8_t i = 5; i < entry->patchSize; i++) {
			patchBuf[i] = 0x90; /* NOP */
		}

		/* Atomic 8-byte write for the first 8 bytes (covers the 5-byte JMP_REL) */
		int64_t newQword = 0;
		memcpy(&newQword, patchBuf, 8);
		InterlockedExchange64((volatile LONG64 *)entry->pTarget, newQword);
		if (entry->patchSize > 8) {
			memcpy((uint8_t *)entry->pTarget + 8, patchBuf + 8, entry->patchSize - 8);
		}
	} else {
		uint8_t restoreBuf[16];
		memcpy(restoreBuf, entry->pTarget, sizeof(restoreBuf));
		memcpy(restoreBuf, entry->backup, entry->patchSize);
		int64_t origQword = 0;
		memcpy(&origQword, restoreBuf, 8);
		InterlockedExchange64((volatile LONG64 *)entry->pTarget, origQword);
		if (entry->patchSize > 8) {
			memcpy((uint8_t *)entry->pTarget + 8, restoreBuf + 8, entry->patchSize - 8);
		}
	}

	VirtualProtect(entry->pTarget, entry->patchSize, oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), entry->pTarget, entry->patchSize);
	entry->isEnabled = enable;
	return MH_OK;
}

MH_STATUS WINAPI MH_EnableHook(LPVOID pTarget) {
	if (!g_initialized) {
		return MH_ERROR_NOT_INITIALIZED;
	}
	EnterCriticalSection(&g_cs);
	MH_STATUS status = MH_ERROR_NOT_CREATED;
	for (int i = 0; i < MAX_HOOK_ENTRIES; i++) {
		if (g_hooks[i].inUse && (pTarget == MH_ALL_HOOKS || g_hooks[i].pTarget == pTarget)) {
			status = EnableOneHook(&g_hooks[i], 1);
			if (pTarget != MH_ALL_HOOKS) {
				break;
			}
		}
	}
	LeaveCriticalSection(&g_cs);
	return status;
}

MH_STATUS WINAPI MH_DisableHook(LPVOID pTarget) {
	if (!g_initialized) {
		return MH_ERROR_NOT_INITIALIZED;
	}
	EnterCriticalSection(&g_cs);
	MH_STATUS status = (pTarget == MH_ALL_HOOKS) ? MH_OK : MH_ERROR_NOT_CREATED;
	for (int i = 0; i < MAX_HOOK_ENTRIES; i++) {
		if (g_hooks[i].inUse && (pTarget == MH_ALL_HOOKS || g_hooks[i].pTarget == pTarget)) {
			status = EnableOneHook(&g_hooks[i], 0);
			if (pTarget != MH_ALL_HOOKS) {
				break;
			}
		}
	}
	LeaveCriticalSection(&g_cs);
	return status;
}

MH_STATUS WINAPI MH_RemoveHook(LPVOID pTarget) {
	if (!g_initialized) {
		return MH_ERROR_NOT_INITIALIZED;
	}
	EnterCriticalSection(&g_cs);
	for (int i = 0; i < MAX_HOOK_ENTRIES; i++) {
		if (g_hooks[i].inUse && g_hooks[i].pTarget == pTarget) {
			if (g_hooks[i].isEnabled) {
				MH_STATUS status = EnableOneHook(&g_hooks[i], 0);
				if (status != MH_OK) {
					LeaveCriticalSection(&g_cs);
					return status;
				}
			}
			if (g_hooks[i].pSlot) {
				VirtualFree(g_hooks[i].pSlot, 0, MEM_RELEASE);
			}
			memset(&g_hooks[i], 0, sizeof(HOOK_ENTRY));
			LeaveCriticalSection(&g_cs);
			return MH_OK;
		}
	}
	LeaveCriticalSection(&g_cs);
	return MH_ERROR_NOT_CREATED;
}

const char *WINAPI MH_StatusToString(MH_STATUS status) {
	switch (status) {
		case MH_OK: return "MH_OK";
		case MH_ERROR_ALREADY_INITIALIZED: return "MH_ERROR_ALREADY_INITIALIZED";
		case MH_ERROR_NOT_INITIALIZED: return "MH_ERROR_NOT_INITIALIZED";
		case MH_ERROR_ALREADY_CREATED: return "MH_ERROR_ALREADY_CREATED";
		case MH_ERROR_NOT_CREATED: return "MH_ERROR_NOT_CREATED";
		case MH_ERROR_ENABLED: return "MH_ERROR_ENABLED";
		case MH_ERROR_DISABLED: return "MH_ERROR_DISABLED";
		case MH_ERROR_NOT_EXECUTABLE: return "MH_ERROR_NOT_EXECUTABLE";
		case MH_ERROR_UNSUPPORTED_FUNCTION: return "MH_ERROR_UNSUPPORTED_FUNCTION";
		case MH_ERROR_MEMORY_ALLOC: return "MH_ERROR_MEMORY_ALLOC";
		case MH_ERROR_MEMORY_PROTECT: return "MH_ERROR_MEMORY_PROTECT";
		case MH_ERROR_MODULE_NOT_FOUND: return "MH_ERROR_MODULE_NOT_FOUND";
		case MH_ERROR_FUNCTION_NOT_FOUND: return "MH_ERROR_FUNCTION_NOT_FOUND";
		default: return "MH_UNKNOWN";
	}
}

#endif // _WIN64
