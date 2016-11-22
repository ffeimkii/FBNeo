// Taito SuperChase driver for FBAlpha
//
// Notes: (Nov.21.2016)
//
//   Analogue inputs need to be added, as the game is nearly unplayable with
// the current digital-hack.
//

#include "tiles_generic.h"
#include "m68000_intf.h"
#include "z80_intf.h"
#include "taito.h"
#include "taito_ic.h"
#include "eeprom.h"
#include "es5506.h"

static UINT8 *TaitoF3SoundRam = NULL;
static UINT8 *TaitoF3SharedRam = NULL;
static UINT8 *TaitoES5510DSPRam = NULL;
static INT32 TaitoF3Counter;
static INT32 TaitoF3VectorReg;
static INT32 M68681IMR;
static INT32 IMRStatus;
static UINT32 TaitoES5510GPRLatch;
static UINT32 *TaitoES5510GPR = NULL;

static UINT32 TaitoF3SoundTriggerIRQCycles = 0;
static UINT32 TaitoF3SoundTriggerIRQPulseCycles = 0;
static UINT32 TaitoF3SoundTriggerIRQCycleCounter = 0;
static UINT32 TaitoF3SoundTriggerIRQPulseCycleCounter = 0;
#define IRQ_TRIGGER_OFF		0
#define IRQ_TRIGGER_ONCE	1
#define IRQ_TRIGGER_PULSE	2
static INT32 TaitoF3SoundTriggerIRQCyclesMode = 0;

static UINT8 gearshifter = 0; // Toggle.

static INT32 __fastcall TaitoF3SoundIRQCallback(INT32 /*irq*/)
{
	return TaitoF3VectorReg;
}

static void TaitoF3SoundReset()
{
	memcpy(TaitoF3SoundRam, Taito68KRom3, 8);

	SekOpen(2);
	SekReset();
	SekClose();
}

UINT8 __fastcall TaitoF3Sound68KReadByte(UINT32 a)
{
	if (a >= 0x140000 && a <= 0x140fff) {
		INT32 Offset = (a & 0xfff) >> 1;
		UINT8 *Ram = (UINT8*)TaitoF3SharedRam;
		return Ram[Offset^1];
	}

	if (a >= 0x260000 && a <= 0x2601ff) {
		INT32 Offset = (a & 0x1ff);

		if (Offset == 0x12 * 2) return 0;
		if (Offset == 0x16 * 2) return 0x27;

		return TaitoES5510DSPRam[Offset^1];
	}

	if (a >= 0x280000 && a <= 0x28001f) {
		INT32 Offset = (a & 0x1f) >> 1;
		if (Offset == 0x05) {
			INT32 Ret = IMRStatus;
			IMRStatus = 0;
			return Ret;
		}

		if (Offset == 0x0e) return 0x01;

		if (Offset == 0x0f) {
			SekSetIRQLine(6, CPU_IRQSTATUS_NONE);
			return 0x00;
		}

		return 0xff;
	}

	if (a >= 0x200000 && a <= 0x20001f) {
		INT32 Offset = ((a & 0x1f) >> 1);
		INT16 rc = ES5505Read(Offset);
		if (Offset&1) rc >>=8;

		return rc&0xff;
	}

	bprintf(PRINT_NORMAL, _T("Sound 68K Read byte => %06X\n"), a);

	return 0;
}

void __fastcall TaitoF3Sound68KWriteByte(UINT32 a, UINT8 d)
{
	if (a >= 0x140000 && a <= 0x140fff) {
		INT32 Offset = (a & 0xfff) >> 1;
		UINT8 *Ram = (UINT8*)TaitoF3SharedRam;
		Ram[Offset^1] = d;
		return;
	}

	if (a >= 0x260000 && a <= 0x2601ff) {
		INT32 Offset = (a & 0x1ff);

		TaitoES5510DSPRam[Offset^1] = d;

		switch (Offset) {
			case 0x00: {
				TaitoES5510GPRLatch = (TaitoES5510GPRLatch & 0x00ffff) | ((d & 0xff) << 16);
				return;
			}

			case 0x01: {
				TaitoES5510GPRLatch = (TaitoES5510GPRLatch & 0xff00ff) | ((d & 0xff) << 8);
				return;
			}

			case 0x02: {
				TaitoES5510GPRLatch= (TaitoES5510GPRLatch & 0xffff00) | ((d & 0xff) << 0);
				return;
			}

			case 0x03: {
				return;
			}

			case 0x80: {
				if (d < 0xc0) {
					TaitoES5510GPRLatch = TaitoES5510GPR[d];
				}
				return;
			}

			case 0xa0: {
				if (d < 0xc0) {
					TaitoES5510GPR[d] = TaitoES5505Rom[(TaitoES5510GPRLatch >> 8) & (TaitoES5505RomSize - 1)];
				}
				return;
			}

			case 0xc0: {
				return;
			}

			case 0xe0: {
				return;
			}

			default: {
//				bprintf(PRINT_NORMAL,_T("es5510_dsp_w byte %x -> %x\n"), Offset, d);
				return;
			}
		}
	}

	if (a >= 0x280000 && a <= 0x28001f) {
		INT32 Offset = (a & 0x1f) >> 1;

		switch (Offset) {
			case 0x04: {
				switch ((d >> 4) & 0x07) {
					case 0x0: {
						return;
					}

					case 0x01: {
						return;
					}

					case 0x02: {
						return;
					}

					case 0x03: {
						//bprintf(PRINT_NORMAL, _T("counter is %04x (/16), so interrupt once in %d cycles\n"), TaitoF3Counter, (16000000 / 2000000) * TaitoF3Counter * 16);
						TaitoF3SoundTriggerIRQCyclesMode = IRQ_TRIGGER_ONCE;
						TaitoF3SoundTriggerIRQCycleCounter = 0;
						TaitoF3SoundTriggerIRQCycles = (16000000 / 2000000) * TaitoF3Counter * 16;
						return;
					}

					case 0x04: {
						return;
					}

					case 0x05: {
						return;
					}

					case 0x06: {
						//bprintf(PRINT_NORMAL, _T("counter is %04x, so interrupt every %d cycles\n"), TaitoF3Counter, (16000000 / 2000000) * TaitoF3Counter);
						TaitoF3SoundTriggerIRQCyclesMode = IRQ_TRIGGER_PULSE;
						TaitoF3SoundTriggerIRQPulseCycleCounter = 0;
						TaitoF3SoundTriggerIRQPulseCycles = (16000000 / 2000000) * TaitoF3Counter;
						return;
					}

					case 0x07: {
						return;
					}
				}
			}

			case 0x05: {
				M68681IMR = d & 0xff;
				return;
			}

			case 0x06: {
				TaitoF3Counter = ((d & 0xff) << 8) | (TaitoF3Counter & 0xff);
				return;
			}

			case 0x07: {
				TaitoF3Counter = (TaitoF3Counter & 0xff00) | (d & 0xff);
				return;
			}

			case 0x08: return;
			case 0x09: return;
			case 0x0a: return;
			case 0x0b: return;

			case 0x0c: {
				TaitoF3VectorReg = d & 0xff;
				return;
			}

			default: {
//				bprintf(PRINT_NORMAL,_T("f3_68681_w byte %x -> %x\n"), Offset, d);
				return;
			}
		}
	}

	if (a >= 0x300000 && a <= 0x30003f) {
		UINT32 MaxBanks = (TaitoES5505RomSize / 0x200000) - 1;
		INT32 Offset = (a & 0x3f) >> 1;

		d &= MaxBanks;
		es5505_voice_bank_w(Offset, d << 20);
		return;
	}

	switch (a) {
		case 0x340000:
		case 0x340002: {
			// f3_volume_w
			return;
		}

		default: {
			bprintf(PRINT_NORMAL, _T("Sound 68K Write byte => %06X, %02X\n"), a, d);
		}
	}
}

UINT16 __fastcall TaitoF3Sound68KReadWord(UINT32 a)
{
	if (a >= 0x200000 && a <= 0x20001f) {
		INT32 Offset = (a & 0x1f) >> 1;

		return ES5505Read(Offset);
	}

	bprintf(PRINT_NORMAL, _T("Sound 68K Read word => %06X\n"), a);

	return 0;
}

void __fastcall TaitoF3Sound68KWriteWord(UINT32 a, UINT16 d)
{
	if (a >= 0x200000 && a <= 0x20001f) {
		INT32 Offset = (a & 0x1f) >> 1;
		ES5505Write(Offset, d);
		return;
	}

	bprintf(PRINT_NORMAL, _T("Sound 68K Write word => %06X, %04X\n"), a, d);
}

struct SpriteEntry {
	INT32 Code;
	INT32 x;
	INT32 y;
	INT32 Colour;
	INT32 xFlip;
	INT32 yFlip;
	INT32 xZoom;
	INT32 yZoom;
	INT32 Priority;
};

static struct SpriteEntry *SpriteList;

static UINT8 SuperchsCoinWord;
static UINT16 SuperchsCpuACtrl;
static UINT32 SuperchsSteer = 0;

static struct BurnInputInfo SuperchsInputList[] =
{
	{"Coin 1"            , BIT_DIGITAL   , TaitoInputPort2 + 7, "p1 coin"   },
	{"Start 1"           , BIT_DIGITAL   , TaitoInputPort1 + 7, "p1 start"  },
	{"Coin 2"            , BIT_DIGITAL   , TaitoInputPort2 + 6, "p2 coin"   },

	{"P1 Left"           , BIT_DIGITAL   , TaitoInputPort3 + 1, "p1 left"   },
	{"P1 Right"          , BIT_DIGITAL   , TaitoInputPort3 + 2, "p1 right"  },
	{"P1 Accelerate"     , BIT_DIGITAL   , TaitoInputPort3 + 0, "p1 fire 1" },
	{"P1 Fire 2 (Brake)" , BIT_DIGITAL   , TaitoInputPort1 + 6, "p1 fire 2" },
	{"P1 Fire 3 (Nitro)" , BIT_DIGITAL   , TaitoInputPort1 + 4, "p1 fire 3" },
	{"P1 Fire 4 (Shift)" , BIT_DIGITAL   , TaitoInputPort1 + 5, "p1 fire 4" },
	{"P1 Fire 5 (Brake?)", BIT_DIGITAL   , TaitoInputPort1 + 0, "p1 fire 5" },

	{"Reset"             , BIT_DIGITAL   , &TaitoReset        , "reset"     },
	{"Service"           , BIT_DIGITAL   , TaitoInputPort2 + 5, "service"   },
	{"Diagnosics"        , BIT_DIGITAL   , TaitoInputPort2 + 4, "diag"      },
};

STDINPUTINFO(Superchs)

static UINT8 shift_toggle(UINT8 shifter_input) // topspeed
{
	{ // gear shifter stuff
		static UINT8 prevshift = 0;

		if (prevshift != shifter_input && shifter_input) {
			gearshifter = !gearshifter;
		}

		prevshift = shifter_input;
	}
	return (gearshifter) ? 0x20 : 0x00;
}

static void SuperchsMakeInputs()
{
	TaitoInput[0] = 0x7f;// bit 7 is eeprom read
	TaitoInput[1] = 0xff;
	TaitoInput[2] = 0xf7;// bit 3 is freeze

	for (INT32 i = 0; i < 8; i++) {
		TaitoInput[0] -= (TaitoInputPort0[i] & 1) << i;
		TaitoInput[1] -= (TaitoInputPort1[i] & 1) << i;
		TaitoInput[2] -= (TaitoInputPort2[i] & 1) << i;
	}

	TaitoInput[1] = (TaitoInput[1] & ~0x20) | shift_toggle(TaitoInputPort1[5]);
}

static struct BurnRomInfo SuperchsRomDesc[] = {
	{ "d46-35+.ic27",       0x040000, 0x1575c9a7, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 }, // Actually labeled D46 35*
	{ "d46-34+.ic25",       0x040000, 0xc72a4d2b, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 }, // Actually labeled D46 34*
	{ "d46-33+.ic23",       0x040000, 0x3094bcd0, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 }, // Actually labeled D46 33*
	{ "d46-32+.ic21",       0x040000, 0x4fbeb335, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 }, // Actually labeled D46 32*

	{ "d46-24.ic127",       0x020000, 0xa006baa1, BRF_ESS | BRF_PRG | TAITO_68KROM2_BYTESWAP },
	{ "d46-23.ic112",       0x020000, 0x9a69dbd0, BRF_ESS | BRF_PRG | TAITO_68KROM2_BYTESWAP },

	{ "d46-37.ic8",         0x020000, 0x60b51b91, BRF_ESS | BRF_PRG | TAITO_68KROM3_BYTESWAP },
	{ "d46-36.ic7",         0x020000, 0x8f7aa276, BRF_ESS | BRF_PRG | TAITO_68KROM3_BYTESWAP },

	{ "d46-05.ic87",        0x100000, 0x150d0e4c, BRF_GRA | TAITO_CHARS_BYTESWAP },
	{ "d46-06.ic88",        0x100000, 0x321308be, BRF_GRA | TAITO_CHARS_BYTESWAP },

	{ "d46-04.ic67",        0x200000, 0x832769a9, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },
	{ "d46-03.ic66",        0x200000, 0xe0e9cbfd, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },
	{ "d46-02.ic65",        0x200000, 0xa83ca82e, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },
	{ "d46-01.ic64",        0x200000, 0x5c2ae92d, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },

	{ "d46-07.ic34",        0x080000, 0xc3b8b093, BRF_GRA | TAITO_SPRITEMAP },

	{ "d46-10.ic2",         0x200000, 0x306256be, BRF_SND | TAITO_ES5505_BYTESWAP }, //15
	{ "d46-12.ic4",         0x200000, 0xa24a53a8, BRF_SND | TAITO_ES5505_BYTESWAP }, //16
	{ "d46-11.ic5",         0x200000, 0xd4ea0f56, BRF_SND | TAITO_ES5505_BYTESWAP }, //17

	{ "eeprom-superchs.bin",0x000080, 0x230f0753, BRF_PRG | TAITO_DEFAULT_EEPROM },

	{ "pal16l8bcn-d46-13.ic82",    	0x000104, 0x2f32e889, BRF_OPT },
	{ "pal16l8bcn-d46-14.ic84",    	0x000104, 0x5ac8b5f8, BRF_OPT },
	{ "pal16l8bcn-d46-15.ic9",     	0x000104, 0x38ea9f36, BRF_OPT },
	{ "palce20v8h-d46-16.ic8",      0x000157, 0x64e1ff9f, BRF_OPT },
	{ "palce20v8h-d46-17.ic10",     0x000157, 0x5c9d94e1, BRF_OPT },
	{ "palce16v8h-d46-18.ic6",      0x000117, 0x7581b894, BRF_OPT },
	{ "palce16v8h-d46-19.ic7",      0x000117, 0xa5d863d0, BRF_OPT },
	{ "palce20v8h-d46-20.ic22",     0x000157, 0x838cbc11, BRF_OPT }, // Located on the sound board
	{ "palce20v8h-d46-21.ic23",     0x000157, 0x93c5aac2, BRF_OPT }, // Located on the sound board
	{ "palce20v8h-d46-22.ic24",     0x000157, 0xc6a10b06, BRF_OPT }, // Located on the sound board
};

STD_ROM_PICK(Superchs)
STD_ROM_FN(Superchs)

static struct BurnRomInfo SuperchsuRomDesc[] = {
	{ "d46-35+.ic27",       0x040000, 0x1575c9a7, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 }, // Actually labeled D46 35*
	{ "d46-34+.ic25",       0x040000, 0xc72a4d2b, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 }, // Actually labeled D46 34*
	{ "d46-33+.ic23",       0x040000, 0x3094bcd0, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 }, // Actually labeled D46 33*
	{ "d46-31+.ic21",       0x040000, 0x38b983a3, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 }, // Actually labeled D46 31*

	{ "d46-24.ic127",       0x020000, 0xa006baa1, BRF_ESS | BRF_PRG | TAITO_68KROM2_BYTESWAP },
	{ "d46-23.ic112",       0x020000, 0x9a69dbd0, BRF_ESS | BRF_PRG | TAITO_68KROM2_BYTESWAP },

	{ "d46-37.ic8",         0x020000, 0x60b51b91, BRF_ESS | BRF_PRG | TAITO_68KROM3_BYTESWAP },
	{ "d46-36.ic7",         0x020000, 0x8f7aa276, BRF_ESS | BRF_PRG | TAITO_68KROM3_BYTESWAP },

	{ "d46-05.ic87",        0x100000, 0x150d0e4c, BRF_GRA | TAITO_CHARS_BYTESWAP },
	{ "d46-06.ic88",        0x100000, 0x321308be, BRF_GRA | TAITO_CHARS_BYTESWAP },

	{ "d46-04.ic67",        0x200000, 0x832769a9, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },
	{ "d46-03.ic66",        0x200000, 0xe0e9cbfd, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },
	{ "d46-02.ic65",        0x200000, 0xa83ca82e, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },
	{ "d46-01.ic64",        0x200000, 0x5c2ae92d, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },

	{ "d46-07.ic34",        0x080000, 0xc3b8b093, BRF_GRA | TAITO_SPRITEMAP },

	{ "d46-10.ic2",         0x200000, 0x306256be, BRF_SND | TAITO_ES5505_BYTESWAP }, // 15
	{ "d46-12.ic4",         0x200000, 0xa24a53a8, BRF_SND | TAITO_ES5505_BYTESWAP },
	{ "d46-11.ic5",         0x200000, 0xd4ea0f56, BRF_SND | TAITO_ES5505_BYTESWAP },

	{ "eeprom-superchs.bin",0x000080, 0x230f0753, BRF_PRG | TAITO_DEFAULT_EEPROM },

	{ "pal16l8bcn-d46-13.ic82",    	0x000104, 0x2f32e889, BRF_OPT },
	{ "pal16l8bcn-d46-14.ic84",    	0x000104, 0x5ac8b5f8, BRF_OPT },
	{ "pal16l8bcn-d46-15.ic9",     	0x000104, 0x38ea9f36, BRF_OPT },
	{ "palce20v8h-d46-16.ic8",      0x000157, 0x64e1ff9f, BRF_OPT },
	{ "palce20v8h-d46-17.ic10",     0x000157, 0x5c9d94e1, BRF_OPT },
	{ "palce16v8h-d46-18.ic6",      0x000117, 0x7581b894, BRF_OPT },
	{ "palce16v8h-d46-19.ic7",      0x000117, 0xa5d863d0, BRF_OPT },
	{ "palce20v8h-d46-20.ic22",     0x000157, 0x838cbc11, BRF_OPT }, // Located on the sound board
	{ "palce20v8h-d46-21.ic23",     0x000157, 0x93c5aac2, BRF_OPT }, // Located on the sound board
	{ "palce20v8h-d46-22.ic24",     0x000157, 0xc6a10b06, BRF_OPT }, // Located on the sound board
};

STD_ROM_PICK(Superchsu)
STD_ROM_FN(Superchsu)

static struct BurnRomInfo SuperchsjRomDesc[] = {
	{ "d46-28+.ic27",       0x040000, 0x5c33784f, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 },
	{ "d46-27+.ic25",       0x040000, 0xe81125b8, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 },
	{ "d46-26+.ic23",       0x040000, 0x2aaba1b0, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 },
	{ "d46-25+.ic21",       0x040000, 0x4241e97a, BRF_ESS | BRF_PRG | TAITO_68KROM1_BYTESWAP32 },

	{ "d46-24.ic127",       0x020000, 0xa006baa1, BRF_ESS | BRF_PRG | TAITO_68KROM2_BYTESWAP },
	{ "d46-23.ic112",       0x020000, 0x9a69dbd0, BRF_ESS | BRF_PRG | TAITO_68KROM2_BYTESWAP },

	{ "d46-30.ic8",         0x020000, 0x88f8a421, BRF_ESS | BRF_PRG | TAITO_68KROM3_BYTESWAP },
	{ "d46-29.ic7",         0x020000, 0x04501fa5, BRF_ESS | BRF_PRG | TAITO_68KROM3_BYTESWAP },

	{ "d46-05.ic87",        0x100000, 0x150d0e4c, BRF_GRA | TAITO_CHARS_BYTESWAP },
	{ "d46-06.ic88",        0x100000, 0x321308be, BRF_GRA | TAITO_CHARS_BYTESWAP },

	{ "d46-04.ic67",        0x200000, 0x832769a9, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },
	{ "d46-03.ic66",        0x200000, 0xe0e9cbfd, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },
	{ "d46-02.ic65",        0x200000, 0xa83ca82e, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },
	{ "d46-01.ic64",        0x200000, 0x5c2ae92d, BRF_GRA | TAITO_SPRITESA_BYTESWAP32 },

	{ "d46-07.ic34",        0x080000, 0xc3b8b093, BRF_GRA | TAITO_SPRITEMAP },

	{ "d46-10.ic2",         0x200000, 0x306256be, BRF_SND | TAITO_ES5505_BYTESWAP },
	{ "d46-09.ic4",         0x200000, 0x0acb8bc7, BRF_SND | TAITO_ES5505_BYTESWAP },
	{ "d46-08.ic5",         0x200000, 0x4677e820, BRF_SND | TAITO_ES5505_BYTESWAP },

	{ "eeprom-superchs.bin",0x000080, 0x230f0753, BRF_PRG | TAITO_DEFAULT_EEPROM },

	{ "pal16l8bcn-d46-13.ic82",    	0x000104, 0x2f32e889, BRF_OPT },
	{ "pal16l8bcn-d46-14.ic84",    	0x000104, 0x5ac8b5f8, BRF_OPT },
	{ "pal16l8bcn-d46-15.ic9",     	0x000104, 0x38ea9f36, BRF_OPT },
	{ "palce20v8h-d46-16.ic8",      0x000157, 0x64e1ff9f, BRF_OPT },
	{ "palce20v8h-d46-17.ic10",     0x000157, 0x5c9d94e1, BRF_OPT },
	{ "palce16v8h-d46-18.ic6",      0x000117, 0x7581b894, BRF_OPT },
	{ "palce16v8h-d46-19.ic7",      0x000117, 0xa5d863d0, BRF_OPT },
	{ "palce20v8h-d46-20.ic22",     0x000157, 0x838cbc11, BRF_OPT }, // Located on the sound board
	{ "palce20v8h-d46-21.ic23",     0x000157, 0x93c5aac2, BRF_OPT }, // Located on the sound board
	{ "palce20v8h-d46-22.ic24",     0x000157, 0xc6a10b06, BRF_OPT }, // Located on the sound board
};

STD_ROM_PICK(Superchsj)
STD_ROM_FN(Superchsj)

static INT32 MemIndex()
{
	UINT8 *Next; Next = TaitoMem;

	Taito68KRom1                    = Next; Next += Taito68KRom1Size;
	Taito68KRom2                    = Next; Next += Taito68KRom2Size;
	Taito68KRom3                    = Next; Next += Taito68KRom3Size;
	TaitoSpriteMapRom               = Next; Next += TaitoSpriteMapRomSize;
	TaitoES5505Rom                  = Next; Next += TaitoES5505RomSize;
	TaitoDefaultEEProm              = Next; Next += TaitoDefaultEEPromSize;

	TaitoRamStart                   = Next;

	Taito68KRam1                    = Next; Next += 0x020000;
	Taito68KRam2                    = Next; Next += 0x010200;
	TaitoSpriteRam                  = Next; Next += 0x002000;
	TaitoPaletteRam                 = Next; Next += 0x008000;
	TaitoSharedRam                  = Next; Next += 0x010000;
	TaitoF3SoundRam                 = Next; Next += 0x010000;
	TaitoF3SharedRam                = Next; Next += 0x000800;
	TaitoES5510DSPRam               = Next; Next += 0x000200;
	TaitoES5510GPR                  = (UINT32*)Next; Next += 0x0000c0 * sizeof(UINT32);

	TaitoRamEnd                     = Next;

	TaitoChars                      = Next; Next += TaitoNumChar * TaitoCharWidth * TaitoCharHeight;
	TaitoSpritesA                   = Next; Next += TaitoNumSpriteA * TaitoSpriteAWidth * TaitoSpriteAHeight;
	TaitoPalette                    = (UINT32*)Next; Next += 0x02000 * sizeof(UINT32);
	SpriteList                      = (SpriteEntry*)Next; Next += 0x4000 * sizeof(SpriteEntry);

	TaitoMemEnd                     = Next;

	return 0;
}

static INT32 SuperchsDoReset()
{
	TaitoDoReset();

	SuperchsCoinWord = 0;
	SuperchsCpuACtrl = 0;
	SuperchsSteer = 0;
	gearshifter = 0;

	TaitoF3SoundReset();

	return 0;
}

UINT8 __fastcall Superchs68K1ReadByte(UINT32 a)
{
	switch (a) {
		case 0x300000: {
			return 0xff;
		}

		case 0x300001: {
			return TaitoInput[2] | TaitoDip[0];
		}

		case 0x300002: {
			return TaitoInput[1];
		}

		case 0x300003: {
			return TaitoInput[0] | ((EEPROMRead() & 1) ? 0x80 : 0x00);
		}

		case 0x300004: {
			return SuperchsCoinWord;
		}

		case 0x340000: {
			INT32 Delta;
			UINT32 Goal = 0x80;
			if (TaitoInputPort3[1]) Goal = 0xff;
			if (TaitoInputPort3[2]) Goal = 0x0;

			if (SuperchsSteer != Goal) {
				Delta = Goal - SuperchsSteer;
				if (SuperchsSteer < Goal) {
					if (Delta > 2) Delta = 2;
				} else {
					if (Delta < -2) Delta = -2;
				}
				SuperchsSteer += Delta;
			}
			return SuperchsSteer;
		}

		case 0x340001: {
			if (TaitoInputPort3[0]) return 0x00;
			return 0xff;
		}

		case 0x340002: {
			return 0x7f;
		}

		case 0x340003: {
			return 0x7f;
		}

	    default: {
		    bprintf(PRINT_NORMAL, _T("68K #1 Read byte => %06X\n"), a);
		}
	}

	return 0xff;
}

void __fastcall Superchs68K1WriteByte(UINT32 a, UINT8 d)
{
	switch (a) {
		case 0x300000: {
			// watchdog write
			return;
		}

		case 0x300003: {
			EEPROMWrite(d & 0x20, d & 0x10, (d & 0x40)>>6);
			return;
		}

		case 0x300005:
		case 0x300006: {
			// superchs_input_w
			return;
		}

		case 0x300004: {
			SuperchsCoinWord = d;
			return;
		}

		case 0x340000:
		case 0x340001:
		case 0x340002: {
			SekSetIRQLine(3, CPU_IRQSTATUS_AUTO);
			return;
		}

		case 0x340003: {
			// irq ack?
			return;
		}

		case 0x380000: {
			return;
		}

		default: {
			bprintf(PRINT_NORMAL, _T("68K #1 Write byte => %06X, %02X\n"), a, d);
		}
	}
}

UINT16 __fastcall Superchs68K1ReadWord(UINT32 a)
{
	switch (a) {
		default: {
			bprintf(PRINT_NORMAL, _T("68K #1 Read word => %06X\n"), a);
		}
	}

	return 0;
}

void __fastcall Superchs68K1WriteWord(UINT32 a, UINT16 d)
{
	if (a >= 0x140000 && a <= 0x141fff) {
		UINT16 *Ram = (UINT16*)TaitoSpriteRam;
		INT32 Offset = (a & 0x1fff) >> 1;

		Ram[Offset] = d;
		return;
	}

	TC0480SCPCtrlWordWrite_Map(0x1b0000)

	if ((a & 0xfff000) == 0x17f000) return; // unknown writes (lots)

	switch (a) {
		case 0x240002: {
			SuperchsCpuACtrl = d;
			if (!(SuperchsCpuACtrl & 0x200)) {
				SekClose();
				SekOpen(1);
				SekReset();
				SekClose();
				SekOpen(0);
			}
			return;
		}

		default: {
			bprintf(PRINT_NORMAL, _T("68K #1 Write word => %06X, %04X\n"), a, d);
		}
	}
}

UINT32 __fastcall Superchs68K1ReadLong(UINT32 a)
{
	switch (a) {
		default: {
			bprintf(PRINT_NORMAL, _T("68K #1 Read long => %06X\n"), a);
		}
	}

	return 0;
}

void __fastcall Superchs68K1WriteLong(UINT32 a, UINT32 d)
{
	if (a >= 0x140000 && a <= 0x141fff) {
		UINT16 *Ram = (UINT16*)TaitoSpriteRam;
		INT32 Offset = (a & 0x1fff) >> 1;

		Ram[Offset + 0] = BURN_ENDIAN_SWAP_INT32(d) & 0xffff;
		Ram[Offset + 1] = BURN_ENDIAN_SWAP_INT32(d) >> 16;
		return;
	}

	switch (a) {
		default: {
			bprintf(PRINT_NORMAL, _T("68K #1 Write long => %06X, %08X\n"), a, d);
		}
	}
}

UINT8 __fastcall Superchs68K2ReadByte(UINT32 a)
{
	if (a >= 0x800000 && a <= 0x80ffff) {
		INT32 Offset = (a & 0xffff);
		UINT32 *Ram = (UINT32*)TaitoSharedRam;
		if ((Offset&1)==0) return (Ram[(Offset/2)^1]&0xffff0000)>>16;
		return (Ram[(Offset/2)^1]&0x0000ffff);
	}

	switch (a) {
		default: {
			bprintf(PRINT_NORMAL, _T("68K #2 Read byte => %06X\n"), a);
		}
	}

	return 0;
}

void __fastcall Superchs68K2WriteByte(UINT32 a, UINT8 d)
{
	switch (a) {
		default: {
			bprintf(PRINT_NORMAL, _T("68K #2 Write byte => %06X, %02X\n"), a, d);
		}
	}
}

UINT16 __fastcall Superchs68K2ReadWord(UINT32 a)
{
	if (a >= 0x800000 && a <= 0x80ffff) {
		INT32 Offset = (a & 0xffff);
		UINT32 *Ram = (UINT32*)TaitoSharedRam;
		if ((Offset&1)==0) return (Ram[Offset/2]&0xffff0000)>>16;
		return (Ram[Offset/2]&0x0000ffff);
	}

	switch (a) {
		default: {
			bprintf(PRINT_NORMAL, _T("68K #2 Read word => %06X\n"), a);
		}
	}

	return 0;
}

void __fastcall Superchs68K2WriteWord(UINT32 a, UINT16 d)
{
	if (a >= 0x800000 && a <= 0x80ffff) {
		INT32 Offset = (a & 0xffff);
		UINT32 *Ram = (UINT32*)TaitoSharedRam;
		if ((Offset&1)==0) {
			Ram[Offset/2]=(Ram[Offset/2]&0x00ffffff)|((d&0xff00)<<16);
			Ram[Offset/2]=(Ram[Offset/2]&0xff00ffff)|((d&0x00ff)<<16);
		} else {
			Ram[Offset/2]=(Ram[Offset/2]&0xffff00ff)|((d&0xff00)<< 0);
			Ram[Offset/2]=(Ram[Offset/2]&0xffffff00)|((d&0x00ff)<< 0);
		}
		return;
	}

	switch (a) {
		default: {
			bprintf(PRINT_NORMAL, _T("68K #2 Write word => %06X, %04X\n"), a, d);
		}
	}
}

static INT32 CharPlaneOffsets[4]   = { 0, 1, 2, 3 };
static INT32 CharXOffsets[16]      = { 4, 0, 20, 16, 12, 8, 28, 24, 36, 32, 52, 48, 44, 40, 60, 56 };
static INT32 CharYOffsets[16]      = { 0, 64, 128, 192, 256, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960 };
static INT32 SpritePlaneOffsets[4] = { 0, 8, 16, 24 };
static INT32 SpriteXOffsets[16]    = { 32, 33, 34, 35, 36, 37, 38, 39, 0, 1, 2, 3, 4, 5, 6, 7 };
static INT32 SpriteYOffsets[16]    = { 0, 64, 128, 192, 256, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960 };

static const eeprom_interface superchs_eeprom_interface =
{
	6,				/* address bits */
	16,				/* data bits */
	"0110",			/* read command */
	"0101",			/* write command */
	"0111",			/* erase command */
	"0100000000",	/* unlock command */
	"0100110000",	/* lock command */
	0,
	0,
};

static INT32 SuperchsInit()
{
	INT32 nLen;

	GenericTilesInit();

	TaitoCharModulo = 0x400;
	TaitoCharNumPlanes = 4;
	TaitoCharWidth = 16;
	TaitoCharHeight = 16;
	TaitoCharPlaneOffsets = CharPlaneOffsets;
	TaitoCharXOffsets = CharXOffsets;
	TaitoCharYOffsets = CharYOffsets;
	TaitoNumChar = 0x4000;

	TaitoSpriteAModulo = 0x400;
	TaitoSpriteANumPlanes = 4;
	TaitoSpriteAWidth = 16;
	TaitoSpriteAHeight = 16;
	TaitoSpriteAPlaneOffsets = SpritePlaneOffsets;
	TaitoSpriteAXOffsets = SpriteXOffsets;
	TaitoSpriteAYOffsets = SpriteYOffsets;
	TaitoNumSpriteA = 0x10000;

	TaitoES5505RomSize = 0x2000000;

	TaitoNum68Ks = 3;
	TaitoNumES5505 = 1;
	TaitoNumEEPROM = 1;

	nTaitoCyclesTotal[0] = 20000000 / 60;
	nTaitoCyclesTotal[1] = 16000000 / 60;
	nTaitoCyclesTotal[2] = 16000000 / 60;

	TaitoLoadRoms(0);

	TaitoES5505RomSize = 0x2000000;

	TaitoMem = NULL;
	MemIndex();
	nLen = TaitoMemEnd - (UINT8 *)0;
	if ((TaitoMem = (UINT8 *)BurnMalloc(nLen)) == NULL) return 1;
	memset(TaitoMem, 0, nLen);
	MemIndex();

	if (TaitoLoadRoms(1)) return 1;

	TC0480SCPInit(TaitoNumChar, 0, 0x20, 8, -1, 0, 0);

#if 0
	ROM_REGION16_BE( 0x1000000, "ensoniq.0" , ROMREGION_ERASE00 )
	ROM_LOAD16_BYTE( "d46-10.ic2", 0xc00000, 0x200000, CRC(306256be) SHA1(e6e5d4a4c0b98470f2aff2e94624dd19af73ec5d) )
	ROM_LOAD16_BYTE( "d46-12.ic4", 0x000000, 0x200000, CRC(a24a53a8) SHA1(5d5fb87a94ceabda89360064d7d9b6d23c4c606b) )
	ROM_RELOAD     (               0x400000, 0x200000 )
	ROM_LOAD16_BYTE( "d46-11.ic5", 0x800000, 0x200000, CRC(d4ea0f56) SHA1(dc8d2ed3c11d0b6f9ebdfde805188884320235e6) )
#endif

	TaitoES5505RomSize = 0x2000000;

	memset(TaitoES5505Rom, 0, TaitoES5505RomSize);
	BurnLoadRom(TaitoES5505Rom + 0xc00000 + 1, 15, 2);
	BurnLoadRom(TaitoES5505Rom + 0x000000 + 1, 16, 2);
	BurnLoadRom(TaitoES5505Rom + 0x400000 + 1, 16, 2);
	BurnLoadRom(TaitoES5505Rom + 0x800000 + 1, 17, 2);

	SekInit(0, 0x68EC020);
	SekOpen(0);
	SekMapMemory(Taito68KRom1             , 0x000000, 0x0fffff, MAP_ROM);
	SekMapMemory(Taito68KRam1             , 0x100000, 0x11ffff, MAP_RAM);
	SekMapMemory(TC0480SCPRam             , 0x180000, 0x18ffff, MAP_RAM);
	SekMapMemory(TaitoSharedRam           , 0x200000, 0x20ffff, MAP_RAM);
	SekMapMemory(TaitoF3SharedRam         , 0x2c0000, 0x2c07ff, MAP_RAM);
	SekMapMemory(TaitoPaletteRam          , 0x280000, 0x287fff, MAP_RAM);
	SekSetReadByteHandler(0, Superchs68K1ReadByte);
	SekSetWriteByteHandler(0, Superchs68K1WriteByte);
	SekSetReadWordHandler(0, Superchs68K1ReadWord);
	SekSetWriteWordHandler(0, Superchs68K1WriteWord);
	SekSetReadLongHandler(0, Superchs68K1ReadLong);
	SekSetWriteLongHandler(0, Superchs68K1WriteLong);
	SekClose();

	SekInit(1, 0x68000);
	SekOpen(1);
	SekMapMemory(Taito68KRom2             , 0x000000, 0x03ffff, MAP_ROM);
	SekMapMemory(Taito68KRam2             , 0x200000, 0x20ffff, MAP_RAM);
	SekMapMemory(TaitoSharedRam           , 0x800000, 0x80ffff, MAP_RAM);
	SekMapMemory(Taito68KRam2 + 0x10000   , 0xa00000, 0xa001ff, MAP_RAM);
	SekSetReadByteHandler(0, Superchs68K2ReadByte);
	SekSetWriteByteHandler(0, Superchs68K2WriteByte);
	SekSetReadWordHandler(0, Superchs68K2ReadWord);
	SekSetWriteWordHandler(0, Superchs68K2WriteWord);
	SekClose();

	SekInit(2, 0x68000);
	SekOpen(2);
	SekMapMemory(TaitoF3SoundRam          , 0x000000, 0x00ffff, MAP_RAM);
	SekMapMemory(TaitoF3SoundRam          , 0x010000, 0x01ffff, MAP_RAM);
	SekMapMemory(TaitoF3SoundRam          , 0x020000, 0x02ffff, MAP_RAM);
	SekMapMemory(TaitoF3SoundRam          , 0x030000, 0x03ffff, MAP_RAM);
	SekMapMemory(Taito68KRom3             , 0xc00000, 0xcfffff, MAP_ROM);
//	SekMapMemory(Taito68KRom3 + 0x20000   , 0xc20000, 0xc3ffff, MAP_ROM);
	SekMapMemory(TaitoF3SoundRam          , 0xff0000, 0xffffff, MAP_RAM);
	SekSetReadByteHandler(0, TaitoF3Sound68KReadByte);
	SekSetWriteByteHandler(0, TaitoF3Sound68KWriteByte);
	SekSetReadWordHandler(0, TaitoF3Sound68KReadWord);
	SekSetWriteWordHandler(0, TaitoF3Sound68KWriteWord);
	SekSetIrqCallback(TaitoF3SoundIRQCallback);
	SekClose();

	EEPROMInit(&superchs_eeprom_interface);
	if (!EEPROMAvailable()) EEPROMFill(TaitoDefaultEEProm, 0, 128);

	ES5505Init(30476100/2, TaitoES5505Rom, TaitoES5505Rom, NULL);

	SuperchsDoReset();

	return 0;
}

static int SuperchsExit()
{
	TaitoExit();
	ES5506Exit();

	SuperchsCoinWord = 0;
	SuperchsCpuACtrl = 0;
	SuperchsSteer = 0;

	return 0;
}

inline static UINT32 CalcCol(UINT32 nColour)
{
	INT32 r, g, b;

	r = (BURN_ENDIAN_SWAP_INT32(nColour) & 0x000000ff) >> 0;
	b = (BURN_ENDIAN_SWAP_INT32(nColour) & 0x00ff0000) >> 16;
	g = (BURN_ENDIAN_SWAP_INT32(nColour) & 0xff000000) >> 24;

	return BurnHighCol(r, g, b, 0);
}

static void SuperchsCalcPalette()
{
	INT32 i;
	UINT32* ps;
	UINT32* pd;

	for (i = 0, ps = (UINT32*)TaitoPaletteRam, pd = TaitoPalette; i < 0x2000; i++, ps++, pd++) {
		*pd = CalcCol(*ps);
	}
}

static void RenderSpriteZoom(INT32 Code, INT32 sx, INT32 sy, INT32 Colour, INT32 xFlip, INT32 yFlip, INT32 xScale, INT32 yScale, UINT8* pSource)
{
	UINT8 *SourceBase = pSource + ((Code % TaitoNumSpriteA) * TaitoSpriteAWidth * TaitoSpriteAHeight);

	INT32 SpriteScreenHeight = (yScale * TaitoSpriteAHeight + 0x8000) >> 16;
	INT32 SpriteScreenWidth = (xScale * TaitoSpriteAWidth + 0x8000) >> 16;

	Colour = 0x10 * (Colour % 0x200);

	if (TaitoFlipScreenX) {
		xFlip = !xFlip;
		sx = 320 - sx - (xScale >> 12);
	}

	if (SpriteScreenWidth && SpriteScreenHeight) {
		INT32 dx = (TaitoSpriteAWidth << 16) / SpriteScreenWidth;
		INT32 dy = (TaitoSpriteAHeight << 16) / SpriteScreenHeight;

		INT32 ex = sx + SpriteScreenWidth;
		INT32 ey = sy + SpriteScreenHeight;

		INT32 xIndexBase;
		INT32 yIndex;

		if (xFlip) {
			xIndexBase = (SpriteScreenWidth - 1) * dx;
			dx = -dx;
		} else {
			xIndexBase = 0;
		}

		if (yFlip) {
			yIndex = (SpriteScreenHeight - 1) * dy;
			dy = -dy;
		} else {
			yIndex = 0;
		}

		if (sx < 0) {
			INT32 Pixels = 0 - sx;
			sx += Pixels;
			xIndexBase += Pixels * dx;
		}

		if (sy < 0) {
			INT32 Pixels = 0 - sy;
			sy += Pixels;
			yIndex += Pixels * dy;
		}

		if (ex > nScreenWidth) {
			INT32 Pixels = ex - nScreenWidth;
			ex -= Pixels;
		}

		if (ey > nScreenHeight) {
			INT32 Pixels = ey - nScreenHeight;
			ey -= Pixels;
		}

		if (ex > sx) {
			INT32 y;

			for (y = sy; y < ey; y++) {
				UINT8 *Source = SourceBase + ((yIndex >> 16) * TaitoSpriteAWidth);
				UINT16* pPixel = pTransDraw + (y * nScreenWidth);

				INT32 x, xIndex = xIndexBase;
				for (x = sx; x < ex; x++) {
					INT32 c = Source[xIndex >> 16];
					if (c != 0) {
						pPixel[x] = c | Colour;
					}
					xIndex += dx;
				}

				yIndex += dy;
			}
		}
	}
}

static void SuperchsMakeSpriteList(INT32 xOffset, INT32 yOffset)
{
	UINT32 *SpriteRam = (UINT32*)TaitoSpriteRam;
	UINT16 *SpriteMap = (UINT16*)TaitoSpriteMapRom;
	INT32 Offset, Data, TileNum, Colour, xFlip, yFlip;
	INT32 x, y, Priority, DblSize, xCur, yCur;
	INT32 SpritesFlipscreen = 0;
	INT32 xZoom, yZoom, zx, zy;
	INT32 SpriteChunk, MapOffset, Code, j, k, px, py;
	INT32 Dimension, TotalChunks, BadChunks;

	struct SpriteEntry *SpritePtr = SpriteList;

	memset(SpriteList, 0, 0x4000 * sizeof(SpriteEntry));

	for (Offset = ((0x2000 / 4) - 4); Offset >= 0; Offset -= 4) {
		Data     = BURN_ENDIAN_SWAP_INT32(SpriteRam[Offset + 0]);
		xFlip    = (Data & 0x00800000) >> 23;
		xZoom    = (Data & 0x007f0000) >> 16;
		TileNum  = (Data & 0x00007fff);

		Data     = BURN_ENDIAN_SWAP_INT32(SpriteRam[Offset + 2]);
		Priority = (Data & 0x000c0000) >> 18;
		Colour   = (Data & 0x0003fc00) >> 10;
		x        = (Data & 0x000003ff);

		Data     = BURN_ENDIAN_SWAP_INT32(SpriteRam[Offset + 3]);
		DblSize  = (Data & 0x00040000) >> 18;
		yFlip    = (Data & 0x00020000) >> 17;
		yZoom    = (Data & 0x0001fc00) >> 10;
		y        = (Data & 0x000003ff);

		Colour |= 0x100;

		if (!TileNum) continue;

		yFlip = !yFlip;
		xZoom += 1;
		yZoom += 1;

		y += yOffset;

		if (x > 0x340) x -= 0x400;
		if (y > 0x340) y -= 0x400;

		x -= xOffset;

		BadChunks = 0;
		Dimension = ((DblSize * 2) + 2);
		TotalChunks = ((DblSize * 3) + 1) << 2;
		MapOffset = TileNum << 2;

		for (SpriteChunk = 0; SpriteChunk < TotalChunks; SpriteChunk++) {
			j = SpriteChunk / Dimension;
			k = SpriteChunk % Dimension;

			px = k;
			py = j;

			if (xFlip)  px = Dimension - 1 - k;
			if (yFlip)  py = Dimension - 1 - j;

			Code = BURN_ENDIAN_SWAP_INT16(SpriteMap[MapOffset + px + (py << (DblSize + 1))]);

			if (Code == 0xffff) {
				BadChunks += 1;
				continue;
			}

			xCur = x + ((k * xZoom) / Dimension);
			yCur = y + ((j * yZoom) / Dimension);

			zx = x + (((k + 1) * xZoom) / Dimension) - xCur;
			zy = y + (((j + 1) * yZoom) / Dimension) - yCur;

			if (SpritesFlipscreen) {
				xCur = 320 - xCur - zx;
				yCur = 256 - yCur - zy;
				xFlip = !xFlip;
				yFlip = !yFlip;
			}

			SpritePtr->Code = Code;
			SpritePtr->Colour = Colour;
			SpritePtr->xFlip = !xFlip;
			SpritePtr->yFlip = yFlip;
			SpritePtr->x = xCur;
			SpritePtr->y = yCur;
			SpritePtr->xZoom = zx << 12;
			SpritePtr->yZoom = zy << 12;
			SpritePtr->Priority = Priority;

			SpritePtr++;
		}
	}
}

static void SuperchsRenderSpriteList(INT32 SpritePriorityLevel)
{
	for (INT32 i = 0; i < 0x4000; i++) {
		if (SpriteList[i].Priority == SpritePriorityLevel) RenderSpriteZoom(SpriteList[i].Code, SpriteList[i].x, SpriteList[i].y, SpriteList[i].Colour, SpriteList[i].xFlip, SpriteList[i].yFlip, SpriteList[i].xZoom, SpriteList[i].yZoom, TaitoSpritesA);
	}
}

static void SuperchsDraw()
{
	UINT8 Layer[4];
	UINT16 Priority = TC0480SCPGetBgPriority();

	Layer[0] = (Priority & 0xf000) >> 12;
	Layer[1] = (Priority & 0x0f00) >>  8;
	Layer[2] = (Priority & 0x00f0) >>  4;
	Layer[3] = (Priority & 0x000f) >>  0;

	SuperchsCalcPalette();
	BurnTransferClear();

	SuperchsMakeSpriteList(48, -116 - 16);

	TC0480SCPTilemapRender(Layer[0], 1, TaitoChars);
	TC0480SCPTilemapRender(Layer[1], 0, TaitoChars);
	SuperchsRenderSpriteList(0);
	TC0480SCPTilemapRender(Layer[2], 0, TaitoChars);
	TC0480SCPTilemapRender(Layer[3], 0, TaitoChars);
	SuperchsRenderSpriteList(1);
	SuperchsRenderSpriteList(2);
	TC0480SCPRenderCharLayer();
	SuperchsRenderSpriteList(3);
	BurnTransferCopy(TaitoPalette);
}

static void F3Sound_IRQ()
{
	if (M68681IMR & 0x08) {
		SekSetIRQLine(6, CPU_IRQSTATUS_ACK);
		IMRStatus |= 0x08;
	}
}

static INT32 SuperchsFrame()
{
	INT32 nInterleave = 16;

	if (TaitoReset) SuperchsDoReset();

	SuperchsMakeInputs();

	nTaitoCyclesDone[0] = nTaitoCyclesDone[1] = nTaitoCyclesDone[2] = 0;

	SekNewFrame();

	for (INT32 i = 0; i < nInterleave; i++) {
		INT32 nCurrentCPU, nNext;

		nCurrentCPU = 0;
		SekOpen(0);
		nNext = (i + 1) * nTaitoCyclesTotal[nCurrentCPU] / nInterleave;
		nTaitoCyclesSegment = nNext - nTaitoCyclesDone[nCurrentCPU];
		nTaitoCyclesDone[nCurrentCPU] += SekRun(nTaitoCyclesSegment);
		if (i == (nInterleave - 3)) SekSetIRQLine(3, CPU_IRQSTATUS_AUTO);
		if (i == (nInterleave - 1)) SekSetIRQLine(2, CPU_IRQSTATUS_AUTO);
		SekClose();

		if (SuperchsCpuACtrl & 0x200) {
			nCurrentCPU = 1;
			SekOpen(1);
			nNext = (i + 1) * nTaitoCyclesTotal[nCurrentCPU] / nInterleave;
			nTaitoCyclesSegment = nNext - nTaitoCyclesDone[nCurrentCPU];
			nTaitoCyclesDone[nCurrentCPU] += SekRun(nTaitoCyclesSegment);
			if (i == (nInterleave - 1)) SekSetIRQLine(4, CPU_IRQSTATUS_AUTO);
			SekClose();
		}

		nCurrentCPU = 2;
		SekOpen(2);
		nNext = (i + 1) * nTaitoCyclesTotal[nCurrentCPU] / nInterleave;
		nTaitoCyclesSegment = nNext - nTaitoCyclesDone[nCurrentCPU];

		nTaitoCyclesDone[nCurrentCPU] += SekRun(nTaitoCyclesSegment);
		if (TaitoF3SoundTriggerIRQCyclesMode == IRQ_TRIGGER_ONCE) {
			TaitoF3SoundTriggerIRQCycleCounter += nTaitoCyclesSegment;
			if (TaitoF3SoundTriggerIRQCycleCounter >= TaitoF3SoundTriggerIRQCycles) {
				TaitoF3SoundTriggerIRQCyclesMode = IRQ_TRIGGER_OFF;
				F3Sound_IRQ();
			}
		}
		if (TaitoF3SoundTriggerIRQCyclesMode == IRQ_TRIGGER_PULSE) {
			TaitoF3SoundTriggerIRQPulseCycleCounter += nTaitoCyclesSegment;
			if (TaitoF3SoundTriggerIRQPulseCycleCounter >= TaitoF3SoundTriggerIRQPulseCycles) {
				F3Sound_IRQ();
				TaitoF3SoundTriggerIRQPulseCycleCounter = 0;
			}
		}
		SekClose();

	}

	if (pBurnSoundOut) {
		ES5505Update(pBurnSoundOut, nBurnSoundLen);
	}

	if (pBurnDraw) SuperchsDraw();

	return 0;
}

static INT32 SuperchsScan(INT32 nAction, INT32 *pnMin)
{
	struct BurnArea ba;

	if (pnMin != NULL) {			// Return minimum compatible version
		*pnMin = 0x029740;
	}

	if (nAction & ACB_MEMORY_RAM) {
		memset(&ba, 0, sizeof(ba));
		ba.Data	  = TaitoRamStart;
		ba.nLen	  = TaitoRamEnd-TaitoRamStart;
		ba.szName = "All Ram";
		BurnAcb(&ba);
	}

	TaitoICScan(nAction);

	if (nAction & ACB_DRIVER_DATA) {
		SekScan(nAction);
		ES5506Scan(nAction, pnMin);

		SCAN_VAR(SuperchsCoinWord);
		SCAN_VAR(SuperchsCpuACtrl);
		SCAN_VAR(SuperchsSteer);
	}

	return 0;
}

struct BurnDriver BurnDrvSuperchs = {
	"superchs", NULL, NULL, NULL, "1992",
	"Super Chase - Criminal Termination (World)\0", NULL, "Taito Corporation Japan", "Taito Misc",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING, 2, HARDWARE_TAITO_MISC, GBF_RACING, 0,
	NULL, SuperchsRomInfo, SuperchsRomName, NULL, NULL, SuperchsInputInfo, NULL,
	SuperchsInit, SuperchsExit, SuperchsFrame, NULL, SuperchsScan,
	NULL, 0x2000, 320, 240, 4, 3
};

struct BurnDriver BurnDrvSuperchsu = {
	"superchsu", "superchs", NULL, NULL, "1992",
	"Super Chase - Criminal Termination (US)\0", NULL, "Taito America Corporation", "Taito Misc",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE, 2, HARDWARE_TAITO_MISC, GBF_RACING, 0,
	NULL, SuperchsuRomInfo, SuperchsuRomName, NULL, NULL, SuperchsInputInfo, NULL,
	SuperchsInit, SuperchsExit, SuperchsFrame, NULL, SuperchsScan,
	NULL, 0x2000, 320, 240, 4, 3
};

struct BurnDriver BurnDrvSuperchsj = {
	"superchsj", "superchs", NULL, NULL, "1992",
	"Super Chase - Criminal Termination (Japan)\0", NULL, "Taito Corporation", "Taito Misc",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE, 2, HARDWARE_TAITO_MISC, GBF_RACING, 0,
	NULL, SuperchsjRomInfo, SuperchsjRomName, NULL, NULL, SuperchsInputInfo, NULL,
	SuperchsInit, SuperchsExit, SuperchsFrame, NULL, SuperchsScan,
	NULL, 0x2000, 320, 240, 4, 3
};
