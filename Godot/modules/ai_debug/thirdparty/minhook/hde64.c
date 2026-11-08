/*
 * x86_64 Instruction Disassembler for MinHook prologue analysis.
 * Accurately decodes prefixes, REX, 1/2/3-byte opcodes, ModR/M, SIB,
 * displacements (including RIP-relative [rip+disp32]), and immediates.
 */

#include "hde64.h"
#include <string.h>

static int opcode_has_modrm_1byte(uint8_t op) {
	/* Group 1-byte opcodes that do NOT have a ModR/M byte */
	switch (op) {
		/* ADD/OR/ADC/SBB/AND/SUB/XOR/CMP AL/rAX, imm; PUSH/POP seg (invalid in 64-bit except prefixes) */
		case 0x04: case 0x05: case 0x0C: case 0x0D:
		case 0x14: case 0x15: case 0x1C: case 0x1D:
		case 0x24: case 0x25: case 0x2C: case 0x2D:
		case 0x34: case 0x35: case 0x3C: case 0x3D:
		/* PUSH/POP reg (0x50..0x5F) */
		case 0x50: case 0x51: case 0x52: case 0x53:
		case 0x54: case 0x55: case 0x56: case 0x57:
		case 0x58: case 0x59: case 0x5A: case 0x5B:
		case 0x5C: case 0x5D: case 0x5E: case 0x5F:
		/* PUSH imm32 (0x68), PUSH imm8 (0x6A), INS/OUTS (0x6C..0x6F) */
		case 0x68: case 0x6A: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
		/* Jcc rel8 (0x70..0x7F) */
		case 0x70: case 0x71: case 0x72: case 0x73:
		case 0x74: case 0x75: case 0x76: case 0x77:
		case 0x78: case 0x79: case 0x7A: case 0x7B:
		case 0x7C: case 0x7D: case 0x7E: case 0x7F:
		/* NOP / XCHG rAX, r64 (0x90..0x97), CBW/CWD/WAIT/PUSHF/POPF/SAHF/LAHF (0x98..0x9F) */
		case 0x90: case 0x91: case 0x92: case 0x93:
		case 0x94: case 0x95: case 0x96: case 0x97:
		case 0x98: case 0x99: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
		/* MOV AL/rAX, moffs (0xA0..0xA3), MOVS/CMPS/TEST/STOS/LODS/SCAS (0xA4..0xAF) */
		case 0xA0: case 0xA1: case 0xA2: case 0xA3:
		case 0xA4: case 0xA5: case 0xA6: case 0xA7:
		case 0xA8: case 0xA9: case 0xAA: case 0xAB:
		case 0xAC: case 0xAD: case 0xAE: case 0xAF:
		/* MOV r8, imm8 (0xB0..0xB7) and MOV r64/r32, imm64/imm32 (0xB8..0xBF) */
		case 0xB0: case 0xB1: case 0xB2: case 0xB3:
		case 0xB4: case 0xB5: case 0xB6: case 0xB7:
		case 0xB8: case 0xB9: case 0xBA: case 0xBB:
		case 0xBC: case 0xBD: case 0xBE: case 0xBF:
		/* RET imm16 (0xC2), RET (0xC3), ENTER (0xC8), LEAVE (0xC9), RETF (0xCA, 0xCB), INT3/INT/IRET (0xCC..0xCF) */
		case 0xC2: case 0xC3: case 0xC8: case 0xC9:
		case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
		/* XLAT (0xD7), LOOP/JCXZ/IN/OUT/CALL/JMP (0xE0..0xEF), HLT/CMC/CLC/STC/CLI/STI/CLD/STD (0xF4, 0xF5, 0xF8..0xFD) */
		case 0xD7:
		case 0xE0: case 0xE1: case 0xE2: case 0xE3:
		case 0xE4: case 0xE5: case 0xE6: case 0xE7:
		case 0xE8: case 0xE9: case 0xEB:
		case 0xEC: case 0xED: case 0xEE: case 0xEF:
		case 0xF4: case 0xF5:
		case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD:
			return 0;
		default:
			return 1;
	}
}

static int opcode_has_modrm_2byte(uint8_t op2) {
	switch (op2) {
		case 0x05: /* SYSCALL */
		case 0x06: /* CLTS */
		case 0x07: /* SYSRET */
		case 0x08: /* INVD */
		case 0x09: /* WBINVD */
		case 0x0B: /* UD2 */
		case 0x30: /* WRMSR */
		case 0x31: /* RDTSC */
		case 0x32: /* RDMSR */
		case 0x33: /* RDPMC */
		case 0x34: /* SYSENTER */
		case 0x35: /* SYSEXIT */
		case 0x37: /* GETSEC */
		case 0x77: /* EMMS */
		/* Jcc rel32 (0x80..0x8F) */
		case 0x80: case 0x81: case 0x82: case 0x83:
		case 0x84: case 0x85: case 0x86: case 0x87:
		case 0x88: case 0x89: case 0x8A: case 0x8B:
		case 0x8C: case 0x8D: case 0x8E: case 0x8F:
		case 0xA0: case 0xA1: case 0xA2: /* PUSH/POP FS, CPUID */
		case 0xA8: case 0xA9: /* PUSH/POP GS */
		/* BSWAP (0xC8..0xCF) */
		case 0xC8: case 0xC9: case 0xCA: case 0xCB:
		case 0xCC: case 0xCD: case 0xCE: case 0xCF:
			return 0;
		default:
			return 1;
	}
}

unsigned int hde64_disasm(const void *code, hde64s *hs) {
	const uint8_t *p = (const uint8_t *)code;
	const uint8_t *start = p;
	memset(hs, 0, sizeof(*hs));

	/* 1. Legacy prefixes */
	for (;;) {
		uint8_t c = *p;
		if (c == 0xF0) {
			hs->p_lock = c;
			hs->flags |= F_PREFIX_LOCK;
		} else if (c == 0xF2) {
			hs->p_rep = c;
			hs->flags |= F_PREFIX_REPNZ;
		} else if (c == 0xF3) {
			hs->p_rep = c;
			hs->flags |= F_PREFIX_REP;
		} else if (c == 0x66) {
			hs->p_66 = c;
			hs->flags |= F_PREFIX_66;
		} else if (c == 0x67) {
			hs->p_67 = c;
			hs->flags |= F_PREFIX_67;
		} else if (c == 0x2E || c == 0x36 || c == 0x3E || c == 0x26 || c == 0x64 || c == 0x65) {
			hs->p_seg = c;
			hs->flags |= F_PREFIX_SEG;
		} else {
			break;
		}
		p++;
		if (p - start >= 15) {
			hs->flags |= F_ERROR | F_ERROR_LENGTH;
			hs->len = 15;
			return 15;
		}
	}

	/* 2. REX prefix (0x40..0x4F) */
	if ((*p & 0xF0) == 0x40) {
		hs->rex = *p++;
		hs->rex_w = (hs->rex >> 3) & 1;
		hs->rex_r = (hs->rex >> 2) & 1;
		hs->rex_x = (hs->rex >> 1) & 1;
		hs->rex_b = hs->rex & 1;
		hs->flags |= F_PREFIX_REX;
	}

	/* 3. Opcode */
	uint8_t op = *p++;
	hs->opcode = op;
	int is_2byte = 0;
	int is_3byte_38 = 0;
	int is_3byte_3a = 0;
	int has_modrm = 0;

	if (op == 0x0F) {
		is_2byte = 1;
		uint8_t op2 = *p++;
		hs->opcode2 = op2;
		if (op2 == 0x38) {
			is_3byte_38 = 1;
			p++; /* 3rd opcode byte */
			has_modrm = 1;
		} else if (op2 == 0x3A) {
			is_3byte_3a = 1;
			p++; /* 3rd opcode byte */
			has_modrm = 1;
		} else {
			has_modrm = opcode_has_modrm_2byte(op2);
		}
	} else {
		has_modrm = opcode_has_modrm_1byte(op);
	}

	/* 4. ModR/M and SIB */
	if (has_modrm) {
		hs->flags |= F_MODRM;
		uint8_t modrm = *p++;
		hs->modrm = modrm;
		hs->modrm_mod = (modrm >> 6) & 3;
		hs->modrm_reg = (modrm >> 3) & 7;
		hs->modrm_rm = modrm & 7;

		if (hs->modrm_mod != 3) {
			if (hs->modrm_rm == 4) {
				hs->flags |= F_SIB;
				uint8_t sib = *p++;
				hs->sib = sib;
				hs->sib_scale = (sib >> 6) & 3;
				hs->sib_index = (sib >> 3) & 7;
				hs->sib_base = sib & 7;
			}

			if (hs->modrm_mod == 0) {
				if (hs->modrm_rm == 5) {
					/* RIP-relative [rip + disp32] */
					hs->flags |= F_DISP32;
					hs->disp_offset = (uint8_t)(p - start);
					memcpy(&hs->disp.disp32, p, 4);
					p += 4;
				} else if ((hs->flags & F_SIB) && hs->sib_base == 5) {
					hs->flags |= F_DISP32;
					hs->disp_offset = (uint8_t)(p - start);
					memcpy(&hs->disp.disp32, p, 4);
					p += 4;
				}
			} else if (hs->modrm_mod == 1) {
				hs->flags |= F_DISP8;
				hs->disp_offset = (uint8_t)(p - start);
				hs->disp.disp8 = *p++;
			} else if (hs->modrm_mod == 2) {
				hs->flags |= F_DISP32;
				hs->disp_offset = (uint8_t)(p - start);
				memcpy(&hs->disp.disp32, p, 4);
				p += 4;
			}
		}
	}

	/* 5. Immediates */
	if (!is_2byte) {
		switch (op) {
			/* imm8 */
			case 0x04: case 0x0C: case 0x14: case 0x1C:
			case 0x24: case 0x2C: case 0x34: case 0x3C:
			case 0x6A: case 0x6B:
			case 0x80: case 0x82: case 0x83:
			case 0xA8:
			case 0xB0: case 0xB1: case 0xB2: case 0xB3:
			case 0xB4: case 0xB5: case 0xB6: case 0xB7:
			case 0xC0: case 0xC1: case 0xC6:
			case 0xCD:
			case 0xE4: case 0xE5: case 0xE6: case 0xE7:
				hs->flags |= F_IMM8;
				hs->imm.imm8 = *p++;
				break;

			/* rel8 */
			case 0x70: case 0x71: case 0x72: case 0x73:
			case 0x74: case 0x75: case 0x76: case 0x77:
			case 0x78: case 0x79: case 0x7A: case 0x7B:
			case 0x7C: case 0x7D: case 0x7E: case 0x7F:
			case 0xE0: case 0xE1: case 0xE2: case 0xE3:
			case 0xEB:
				hs->flags |= F_IMM8 | F_RELATIVE;
				hs->imm.imm8 = *p++;
				break;

			/* imm16 */
			case 0xC2: case 0xCA:
				hs->flags |= F_IMM16;
				memcpy(&hs->imm.imm16, p, 2);
				p += 2;
				break;

			/* ENTER imm16, imm8 */
			case 0xC8:
				hs->flags |= F_IMM16 | F_IMM8;
				p += 3;
				break;

			/* imm16 or imm32 */
			case 0x05: case 0x0D: case 0x15: case 0x1D:
			case 0x25: case 0x2D: case 0x35: case 0x3D:
			case 0x68: case 0x69:
			case 0x81:
			case 0xA9:
			case 0xC7:
				if (hs->p_66 && !hs->rex_w) {
					hs->flags |= F_IMM16;
					memcpy(&hs->imm.imm16, p, 2);
					p += 2;
				} else {
					hs->flags |= F_IMM32;
					memcpy(&hs->imm.imm32, p, 4);
					p += 4;
				}
				break;

			/* rel32 (CALL / JMP) */
			case 0xE8: case 0xE9:
				hs->flags |= F_IMM32 | F_RELATIVE;
				memcpy(&hs->imm.imm32, p, 4);
				p += 4;
				break;

			/* MOV r64/r32/r16, imm64/imm32/imm16 (0xB8..0xBF) */
			case 0xB8: case 0xB9: case 0xBA: case 0xBB:
			case 0xBC: case 0xBD: case 0xBE: case 0xBF:
				if (hs->rex_w) {
					hs->flags |= F_IMM64;
					memcpy(&hs->imm.imm64, p, 8);
					p += 8;
				} else if (hs->p_66) {
					hs->flags |= F_IMM16;
					memcpy(&hs->imm.imm16, p, 2);
					p += 2;
				} else {
					hs->flags |= F_IMM32;
					memcpy(&hs->imm.imm32, p, 4);
					p += 4;
				}
				break;

			/* moffs64 (0xA0..0xA3) */
			case 0xA0: case 0xA1: case 0xA2: case 0xA3:
				if (hs->p_67) {
					hs->flags |= F_IMM32;
					p += 4;
				} else {
					hs->flags |= F_IMM64;
					p += 8;
				}
				break;

			/* Group 3 TEST r/m8, imm8 (0xF6 /0 or /1) */
			case 0xF6:
				if (hs->modrm_reg == 0 || hs->modrm_reg == 1) {
					hs->flags |= F_IMM8;
					hs->imm.imm8 = *p++;
				}
				break;

			/* Group 3 TEST r/m32, imm32 (0xF7 /0 or /1) */
			case 0xF7:
				if (hs->modrm_reg == 0 || hs->modrm_reg == 1) {
					if (hs->p_66 && !hs->rex_w) {
						hs->flags |= F_IMM16;
						p += 2;
					} else {
						hs->flags |= F_IMM32;
						p += 4;
					}
				}
				break;
		}
	} else if (is_3byte_3a) {
		hs->flags |= F_IMM8;
		hs->imm.imm8 = *p++;
	} else if (!is_3byte_38) {
		uint8_t op2 = hs->opcode2;
		if (op2 >= 0x80 && op2 <= 0x8F) {
			/* Jcc rel32 */
			hs->flags |= F_IMM32 | F_RELATIVE;
			memcpy(&hs->imm.imm32, p, 4);
			p += 4;
		} else if (op2 == 0x70 || op2 == 0x71 || op2 == 0x72 || op2 == 0x73 ||
				   op2 == 0xA4 || op2 == 0xAC || op2 == 0xBA || op2 == 0xC2 ||
				   op2 == 0xC4 || op2 == 0xC5 || op2 == 0xC6) {
			hs->flags |= F_IMM8;
			hs->imm.imm8 = *p++;
		}
	}

	hs->len = (uint8_t)(p - start);
	return hs->len;
}
