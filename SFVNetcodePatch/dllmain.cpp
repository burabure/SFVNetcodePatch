#include "stdafx.h"
#include <iostream>
#include "mem.h"
#include <Psapi.h>

namespace Proud
{
	class CHostBase {};

	class CRemotePeer : CHostBase
	{
	private:
		char pad154[0x150];
	public:
		unsigned int Ping; // One way latency
	};

	class CNetClient {};

	class CNetClientImpl
	{
	private:
		char pad2B8[0x2B8];
	public:
		struct
		{
			int Id;
			CHostBase* Pointer;
		} *Host;
	private:
		char pad540[0x540 - 0x2B8 - 8];
	public:
		CNetClient Base;
	};
}

class UInputUnit
{
private:
	char pad2180[0x2180];
public:
	unsigned int CurrentTimestamp;
	unsigned int OpponentTimestamp;
private:
	char pad21D8[0x21D8 - 0x2180 - 8];
public:
	unsigned int MaxFramesAhead; // Max frames ahead of opponent (will slow down to reach this)
	unsigned int IsPaused; // Stop simulating the game
private:
	char pad220C[0x220C - 0x21D8 - 8];
public:
	unsigned int TimeBase; // Set at round start, used to offset desired timestamp
private:
	char pad2214[0x2214 - 0x220C - 4];
public:
	unsigned int DesiredTimestamp; // Set based on real time, will speed up to reach this
private:
	char pad2220[0x2220 - 0x2214 - 4];
public:
	unsigned int FramesToSimulate; // Number of frames to simulate, used to speed up/slow down
};

class UnknownNetBullshit
{
private:
	char pad010[0x10];
public:
	Proud::CNetClient* Client;
};

class UnknownNetList
{
private:
	char pad070[0x70];
public:
	UnknownNetBullshit** List;
	UnknownNetBullshit** ListEnd;
};


UnknownNetList* NetList;

extern "C" uintptr_t UpdateTimestampsOrig;
uintptr_t UpdateTimestampsOrig;
extern "C" void UpdateTimestampsOrigWrapper(UInputUnit*);


bool GetPing(unsigned int* Ping)
{
	// Index 1 is opponent
	if (NetList->List == nullptr ||
		NetList->List + 1 >= NetList->ListEnd ||
		NetList->List[1] == nullptr ||
		NetList->List[1]->Client == nullptr)
	{
		return false;
	}

	// dynamic_cast to CNetClientImpl
	const auto* Client = NetList->List[1]->Client;
	const auto* ClientImpl = (Proud::CNetClientImpl*)((char*)Client - offsetof(Proud::CNetClientImpl, Base));

	if (ClientImpl->Host == nullptr || ClientImpl->Host->Pointer == nullptr)
		return false;

	const auto* Peer = (Proud::CRemotePeer*)ClientImpl->Host->Pointer;
	*Ping = Peer->Ping;
	return true;
}

// Called after UInputUnit::UpdateTimestamps
extern "C" void UpdateTimestampsHook(UInputUnit * Input)
{
	const auto OldTimeBase = Input->TimeBase;

	std::cout << "OldTimeBase:\n";
	std::cout << OldTimeBase;
	std::cout << "\n";

	UpdateTimestampsOrigWrapper(Input);

	static unsigned int LastPingFrames = 0;

	// Game hasn't started yet if TimeBase is still updating
	if (Input->TimeBase != OldTimeBase)
	{
		LastPingFrames = 0;
		return;
	}

	unsigned int Ping;
	if (!GetPing(&Ping))
	{
		LastPingFrames = 0;
		return;
	}

	std::cout << "Ping:\n";
	std::cout << Ping;
	std::cout << "\n";

	auto PingFrames = (unsigned int)((float)Ping * 60.f / 1000.f + .5f);

	std::cout << "PingFrames:\n";
	std::cout << PingFrames;
	std::cout << "\n";

	// Don't hitch from small ping fluctuations
	if (PingFrames == LastPingFrames - 1)
		PingFrames = LastPingFrames;
	else
		LastPingFrames = PingFrames;

	// Don't get farther ahead than normal for compatibility, even with high ping
	Input->MaxFramesAhead = min(PingFrames + 1, 15);

	if (Input->CurrentTimestamp >= Input->OpponentTimestamp + Input->MaxFramesAhead)
	{
		// Don't speed up after waiting for the opponent if ping increases
		Input->TimeBase--;
	}

	// Never get ahead of where we should be based on real time
	const auto TargetTimestamp = min(Input->DesiredTimestamp, Input->OpponentTimestamp + Input->MaxFramesAhead);
	if (Input->CurrentTimestamp < TargetTimestamp)
	{
		// Speed up to correct for hitch
		Input->FramesToSimulate = TargetTimestamp - Input->CurrentTimestamp;
	}

	std::cout << "TargetTimestamp:\n";
	std::cout << TargetTimestamp;
	std::cout << "\n";
}


bool GetModuleBounds(LPCWSTR Name, uintptr_t* Start, uintptr_t* End)
{
	const auto Module = GetModuleHandle(Name);
	if (Module == nullptr)
		return false;

	MODULEINFO Info;
	GetModuleInformation(GetCurrentProcess(), Module, &Info, sizeof(Info));
	*Start = (uintptr_t)(Info.lpBaseOfDll);
	*End = *Start + Info.SizeOfImage;
	return true;
}

uintptr_t Sigscan(const uintptr_t Start, const uintptr_t End, const char* Sig, const char* Mask)
{
	const auto ScanEnd = End - strlen(Mask) + 1;
	for (auto Address = Start; Address < ScanEnd; Address++) {
		for (size_t i = 0;; i++) {
			if (Mask[i] == '\0')
				return Address;
			if (Mask[i] != '?' && Sig[i] != *(char*)(Address + i))
				break;
		}
	}

	return 0;
}

uintptr_t GetRel32(const uintptr_t Address)
{
	return Address + *(int*)(Address)+4;
}

void JmpHook(const uintptr_t Address, void* Target)
{
	constexpr auto PatchSize = 12;

	DWORD OldProtect;
	VirtualProtect((void*)Address, PatchSize, PAGE_EXECUTE_READWRITE, &OldProtect);

	*(WORD*)Address = 0xB848; // mov rax, Target
	*(void**)(Address + 2) = Target;
	*(WORD*)(Address + 10) = 0xE0FF; // jmp rax

	VirtualProtect((void*)Address, PatchSize, OldProtect, &OldProtect);
}

DWORD WINAPI HackThread(HMODULE hModule)
{
	// Create Console
	AllocConsole();
	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stdout);

	std::cout << "Hooked!\n";

	// Get module bounds
	uintptr_t Start, End;
	if (!GetModuleBounds(L"StreetFighterV.exe", &Start, &End))
		return FALSE;

	std::cout << "Module Start!\n";
	std::cout << Start;
	std::cout << "\n";

	std::cout << "Module End!\n";
	std::cout << End;
	std::cout << "\n";

	NetList = (UnknownNetList*)GetRel32(Sigscan(Start, End, "\x9B\x00\x00\x00\x83\xC8\x01", "xxxxxxx") + 0xA);

	std::cout << "NetList Address:\n";
	std::cout << NetList;
	std::cout << "\n";

	// I believe this signature is not correct
	// might need to adjust all the hooking offsets/sigs/code?
	UpdateTimestampsOrig = Sigscan(Start, End, "\x83\xBB\x04\x22\x00\x00\x78", "xxxxxxx") - 0x29;

	std::cout << "UpdateTimestampsOrig Address:\n";
	std::cout << UpdateTimestampsOrig;
	std::cout << "\n";

	// the jmphook causes the game to crash when an online match starts
	// probably because the patching is not on the correct memory addr
	// JmpHook(UpdateTimestampsOrig, UpdateTimestampsHook);

	// debug stuff
	unsigned int Ping;

	while (true)
	{
		if (GetAsyncKeyState(VK_END) & 1)
		{
			break;
		}

		if (GetPing(&Ping))
		{
			auto PingFrames = (unsigned int)((float)Ping * 60.f / 1000.f + .5f);

			std::cout << "Ping:\n";
			std::cout << Ping;
			std::cout << "\n";

			std::cout << "PingFrames:\n";
			std::cout << PingFrames;
			std::cout << "\n";
		}

		Sleep(5);
	}

	fclose(f);
	FreeConsole();
	FreeLibraryAndExitThread(hModule, 0);
	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		CloseHandle(CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)HackThread, hModule, 0, nullptr));
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}