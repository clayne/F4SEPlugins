#include "f4se_common/Relocation.h"
#include "f4se_common/BranchTrampoline.h"
#include "f4se_common/SafeWrite.h"
#include "f4se/Serialization.h"
#include "f4se/GameData.h"

#include "xbyak/xbyak.h"
#include <tlhelp32.h>


/*
	This module exists to address some of the deficiencies in F4SEVR and that prevent this plugin from working
	- The RelocAddr for GameDataReady is wrong, we patch it so that the event fires correctly
	- The serialization of the modlist is outdated in F4SEVR compared to F4SE, 
		so plugins cannot relocate saved formIds or handles. The workaround involves patching the F4SEVR dll's load routines 
		to recognize the newer format.

*/


const UInt64 o_savefileIndexMap = 0xca8a0;				// Production dll offsets
const UInt64 o_numSavefileMods = 0xc7c8a;
const UInt64 o_savefileLightIndexMap = 0xca9a0;
const UInt64 o_numSavefileLightMods = 0xc7c8c;
const UInt64 o_switchDefault = 0x18e81;

static UInt8* F4SEbaseAddr;				// Base addr of the F4SE dll

UInt8* s_savefileIndexMap;				// s_savefileIndexMap[x] gives the new index of mod x
UInt8* s_numSavefileMods;

UInt16* s_savefileLightIndexMap;
UInt16* s_numSavefileLightMods;

// Fix invalid RelocAddr for GameDataReadyOriginal in F4SE/HooksGameData.cpp
// 
void patchGameDataReadyOriginal() {
	UInt8* adrPtr = (UInt8*)F4SEbaseAddr + 0xca290;				// Relocated address of GameDataReadyOriginal is stored here
	UInt64* adlPtr = (UInt64*)adrPtr;							// Get the address

	if (*adlPtr == (RelocationManager::s_baseAddr + 0x05AB9614)) {	// Is it the known bad address?
		*adlPtr = RelocationManager::s_baseAddr + 0x00820130;		// Yes, put in the right one
		}
	}

// Get the load address for main F4SEVR dll
UInt8 *getF4SEbaseAddr() {
	DWORD procID = GetCurrentProcessId();
	HANDLE	snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, procID);
	UInt8* ret = nullptr;

	if (snap != INVALID_HANDLE_VALUE) {
		MODULEENTRY32	module;
		bool modIsValid;

		module.dwSize = sizeof(module);

		for (modIsValid = Module32First(snap, &module); modIsValid; modIsValid = Module32Next(snap, &module)) {
			//_MESSAGE("%16s base: %p : hModule %p", module.szModule, ret, module.hModule);
			if (strcmp(module.szModule, "f4sevr_1_2_72.dll") == 0) {		// Get base address for the DLL
				ret = module.modBaseAddr;
				break;
				}
			}

		CloseHandle(snap);
		}
	return ret;
	}

// Recent versions of F4SE store the mod list in a new format, which F4SEVR cannot read
// So the module list never gets read, leading to plugins that don't work
// F4SE can read the old modlist from F4SEVR, though, so roundtripping is possible

void LoadPluginList(const F4SESerializationInterface* intfc) {
	DataHandler* dhand = *g_dataHandler;

	_MESSAGE("Loading plugin list:");

	char name[0x104] = { 0 };
	UInt16 nameLen = 0;

	UInt16 modCount = 0;
	intfc->ReadRecordData(&modCount, sizeof(modCount));		// Number of mods in the save file

	for (UInt32 i = 0; i < modCount; i++) {
		UInt8 modIndex = 0xFF;
		UInt16 lightModIndex = 0xFFFF;

		intfc->ReadRecordData(&modIndex, sizeof(modIndex));
		if (modIndex == 0xFE) {
			intfc->ReadRecordData(&lightModIndex, sizeof(lightModIndex));		// FO4VR does not support light mods, but we still need to
			}																	// check in case the mod is present in FO4VR as a normal mod

		intfc->ReadRecordData(&nameLen, sizeof(nameLen));
		intfc->ReadRecordData(&name, nameLen);
		name[nameLen] = 0;


		UInt8 newIndex = dhand->GetLoadedModIndex(name);						// Mods might now be in a different position in the list
		if (lightModIndex == 0xFFFF) {											// Not light
			s_savefileIndexMap[modIndex] = newIndex;
			if (*s_numSavefileMods <= modIndex) *s_numSavefileMods = modIndex + 1;	
			_MESSAGE("\t(%d  : %d-> %d)\t%s", i, modIndex, newIndex, name);
			}
		else {
			s_savefileLightIndexMap[lightModIndex] = newIndex;
			if (*s_numSavefileLightMods <= lightModIndex) *s_numSavefileLightMods = lightModIndex + 1;
			_MESSAGE("\t(%dL : %d-> %d)\t%s", i, lightModIndex, newIndex, name);
			}
		}
	}

// Redirect here from the 'default' case of the Core_LoadCallback switch, 
// so we can handle the otherwise unknown plugin record type

void switchDefault_hook(const F4SESerializationInterface* intfc, UInt32 type) {
	if (type == 'PLGN') {
		LoadPluginList(intfc);
		}
	else {
		_MESSAGE("Unhandled chunk type in Core_LoadCallback: %08X (%.4s)", type, &type);
		}
	}


void patchSerialization() {
#pragma pack(push, 1)

	// Need to patch the 'default' clause in the F4SE CoreLoadCallback switch statement,
	// as the 'PLGN' type record used by newer version of F4SE is not handled by F4SEVR
	// So we just patch the code to call our own routine
	// This requires a 64bit displacement, hence custom code here

	struct {
		UInt8 movcxdx[3];
		UInt8 nop[2];
		UInt8 movaxval[2];
		UInt8* addr;
		UInt8 callax[2];
		} opcodes;

	opcodes.movcxdx[0] = 0x48;				// Move intfc from RDX into RCX
	opcodes.movcxdx[1] = 0x89;
	opcodes.movcxdx[2] = 0xd9;
	opcodes.nop[0] = 0x90;					// Just a pair of NOPs, makes the address fall on an 8-byte boundary
	opcodes.nop[1] = 0x90;
	opcodes.movaxval[0] = 0x48;			// MOV AX, 'address of hook'
	opcodes.movaxval[1] = 0xb8;
	opcodes.addr = (UInt8 *) switchDefault_hook;
	opcodes.callax[0] = 0xff;				// CALL AX
	opcodes.callax[1] = 0xd0;

	SafeWriteBuf((uintptr_t) (F4SEbaseAddr + o_switchDefault), &opcodes, sizeof(opcodes));

#pragma pack( pop)
	}


void do_patch() {
	F4SEbaseAddr = getF4SEbaseAddr();
	_MESSAGE("F4SEVR base addr: %p", F4SEbaseAddr);

	if (F4SEbaseAddr != nullptr) {
		s_savefileIndexMap = F4SEbaseAddr + o_savefileIndexMap;				// s_savefileIndexMap[x] gives the new index of mod x
		s_numSavefileMods = F4SEbaseAddr + o_numSavefileMods;

		s_savefileLightIndexMap = (UInt16*)(F4SEbaseAddr + o_savefileLightIndexMap);
		s_numSavefileLightMods = (UInt16*)(F4SEbaseAddr + o_numSavefileLightMods);
		patchGameDataReadyOriginal();
		}
	}
