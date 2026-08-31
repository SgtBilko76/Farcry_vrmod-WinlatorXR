#include "StdAfx.h"
#include "WinlatorXR.h"

// NOTE: StdAfx.h (via platform.h) already pulls in <windows.h> without WIN32_LEAN_AND_MEAN, which
// transitively includes the legacy <winsock.h>. Including <winsock2.h>/<ws2tcpip.h> afterwards would
// conflict with that (duplicate socket type/macro definitions). Since StdAfx.h must remain the first
// include in every translation unit (precompiled header requirement), we deliberately stick to the
// legacy Winsock 1.1 API that's already available transitively - it's functionally identical for the
// handful of calls we need (socket/bind/recvfrom/sendto/WSAStartup) and links against the same
// ws2_32.lib already used by this project.
//
// Similarly, this project builds against STLport (see ..\STLPORT\stlport in the include path and the
// _NOTHREADS/_STLP_NO_THREADS defines in StdAfx.h) with its own iostreams disabled
// (_STLP_NO_OWN_IOSTREAMS), which leaves std::thread/mutex/istringstream/ofstream etc. all
// unavailable or conflicting with modern MSVC STL headers pulled in as a fallback. So this file
// deliberately sticks to plain C stdio/string.h and Win32 threading primitives instead, consistent
// with the rest of this vintage codebase.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

namespace WinlatorXR
{
	static const u_short kListenPort = 7872;
	static const u_short kListenPortFallback = 7873;
	static const u_short kSendPort = 7278;

	static HANDLE s_recvThread = nullptr;
	static SOCKET s_recvSocket = INVALID_SOCKET;
	static SOCKET s_sendSocket = INVALID_SOCKET;
	static CRITICAL_SECTION s_cs;
	static bool s_csInitialized = false;
	static InputState s_latest;
	static volatile bool s_running = false;
	static bool s_wsaStarted = false;

	// text must be a mutable, null-terminated buffer - strtok_s writes into it
	static bool ParsePacket(char* text, InputState& out)
	{
		char* context = nullptr;
		char* token = strtok_s(text, " \r\n", &context);
		if (!token)
			return false; // client id token, value not used

		float floats[28];
		for (float& f : floats)
		{
			token = strtok_s(nullptr, " \r\n", &context);
			if (!token)
				return false;
			f = (float)atof(token);
		}

		token = strtok_s(nullptr, " \r\n", &context);
		if (!token)
			return false;
		int frameId = atoi(token);

		token = strtok_s(nullptr, " \r\n", &context);
		if (!token || strlen(token) < 19)
			return false;

		out.left.qx = floats[0]; out.left.qy = floats[1]; out.left.qz = floats[2]; out.left.qw = floats[3];
		out.left.thumbX = floats[4]; out.left.thumbY = floats[5];
		out.left.posX = floats[6]; out.left.posY = floats[7]; out.left.posZ = floats[8];

		out.right.qx = floats[9]; out.right.qy = floats[10]; out.right.qz = floats[11]; out.right.qw = floats[12];
		out.right.thumbX = floats[13]; out.right.thumbY = floats[14];
		out.right.posX = floats[15]; out.right.posY = floats[16]; out.right.posZ = floats[17];

		out.hmdQx = floats[18]; out.hmdQy = floats[19]; out.hmdQz = floats[20]; out.hmdQw = floats[21];
		out.hmdX = floats[22]; out.hmdY = floats[23]; out.hmdZ = floats[24];

		out.ipd = floats[25];
		out.fovH = floats[26];
		out.fovV = floats[27];

		out.frameId = frameId;

		auto b = [&](size_t i) { return token[i] == 'T'; };
		out.lGrip = b(0); out.lMenu = b(1); out.lThumbstickPress = b(2); out.lThumbLeft = b(3); out.lThumbRight = b(4);
		out.lThumbUp = b(5); out.lThumbDown = b(6); out.lTrigger = b(7); out.lButtonX = b(8); out.lButtonY = b(9);
		out.rButtonA = b(10); out.rButtonB = b(11); out.rGrip = b(12); out.rThumbstickPress = b(13); out.rThumbLeft = b(14);
		out.rThumbRight = b(15); out.rThumbUp = b(16); out.rThumbDown = b(17); out.rTrigger = b(18);

		// Protocol 0.5 appends: HMD_ALTITUDE, left grip quaternion (4), right grip quaternion (4).
		// (Protocol 0.3+ additionally appends a "TF"-style flags token at the very end, which we skip.)
		// All of this is optional so that older WinlatorXR builds keep working.
		float extras[9];
		int numExtras = 0;
		while (numExtras < 9)
		{
			token = strtok_s(nullptr, " \r\n", &context);
			if (!token)
				break;
			char* end = nullptr;
			float value = (float)strtod(token, &end);
			if (end == token || (*end != '\0'))
				break; // not a number (e.g. the flags token)
			extras[numExtras++] = value;
		}
		if (numExtras >= 1)
			out.hmdAltitude = extras[0];
		if (numExtras >= 9)
		{
			out.left.gripQx = extras[1]; out.left.gripQy = extras[2]; out.left.gripQz = extras[3]; out.left.gripQw = extras[4];
			out.right.gripQx = extras[5]; out.right.gripQy = extras[6]; out.right.gripQz = extras[7]; out.right.gripQw = extras[8];
			float ll = out.left.gripQx * out.left.gripQx + out.left.gripQy * out.left.gripQy + out.left.gripQz * out.left.gripQz + out.left.gripQw * out.left.gripQw;
			float rl = out.right.gripQx * out.right.gripQx + out.right.gripQy * out.right.gripQy + out.right.gripQz * out.right.gripQz + out.right.gripQw * out.right.gripQw;
			out.hasGripOrientation = ll > 0.5f && rl > 0.5f;
		}

		out.valid = true;
		return true;
	}

	static SOCKET BindListenSocket()
	{
		SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sock == INVALID_SOCKET)
			return INVALID_SOCKET;

		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = INADDR_ANY;
		addr.sin_port = htons(kListenPort);

		if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
		{
			CryLogAlways("[WinlatorXR] Failed to bind UDP port %d (error %d), trying fallback port %d", kListenPort, WSAGetLastError(), kListenPortFallback);
			closesocket(sock);

			sock = socket(AF_INET, SOCK_DGRAM, 0);
			if (sock == INVALID_SOCKET)
				return INVALID_SOCKET;

			addr.sin_port = htons(kListenPortFallback);
			if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
			{
				CryLogAlways("[WinlatorXR] Failed to bind fallback UDP port %d as well (error %d)", kListenPortFallback, WSAGetLastError());
				closesocket(sock);
				return INVALID_SOCKET;
			}
		}

		return sock;
	}

	static void ReceiveLoop()
	{
		s_recvSocket = BindListenSocket();
		if (s_recvSocket == INVALID_SOCKET)
		{
			CryLogAlways("[WinlatorXR] Giving up on UDP receiver - no port could be bound");
			return;
		}

		CryLogAlways("[WinlatorXR] UDP receiver listening");

		char buffer[1024];
		while (s_running)
		{
			int received = recvfrom(s_recvSocket, buffer, sizeof(buffer) - 1, 0, nullptr, nullptr);
			if (!s_running)
				break;

			if (received > 0)
			{
				buffer[received] = '\0';
				InputState parsed;
				if (ParsePacket(buffer, parsed))
				{
					EnterCriticalSection(&s_cs);
					s_latest = parsed;
					LeaveCriticalSection(&s_cs);
				}
			}
		}
	}

	static unsigned __stdcall ReceiveThreadProc(void*)
	{
		ReceiveLoop();
		return 0;
	}

	static void WriteMarkerFiles()
	{
		bool haveZDrive = GetFileAttributesA("Z:\\") != INVALID_FILE_ATTRIBUTES;
		const char* dir = haveZDrive ? "Z:\\tmp\\xr" : "D:\\xrtemp";

		// best-effort directory creation; ignore failures if it already exists
		if (haveZDrive)
			CreateDirectoryA("Z:\\tmp", nullptr);
		CreateDirectoryA(dir, nullptr);

		char versionPath[MAX_PATH];
		char vrPath[MAX_PATH];
		sprintf(versionPath, "%s\\version", dir);
		sprintf(vrPath, "%s\\vr", dir);

		// Requested XrAPI protocol version. 0.5 = 0.2 wire format + HMD altitude + grip orientations
		// (parsed in ParsePacket) + an X server input channel on UDP 7728 we don't use yet. WinlatorXR
		// picks the implementation from this file once at startup.
		FILE* versionFile = fopen(versionPath, "w");
		if (versionFile)
		{
			fputs("0.5", versionFile);
			fclose(versionFile);
		}
		else
		{
			CryLogAlways("[WinlatorXR] Failed to write marker file %s", versionPath);
		}

		FILE* vrFile = fopen(vrPath, "w");
		if (vrFile)
		{
			fputs("VR", vrFile);
			fclose(vrFile);
		}
		else
		{
			CryLogAlways("[WinlatorXR] Failed to write marker file %s", vrPath);
		}
	}

	bool IsLikelyPresent()
	{
		DWORD attrs = GetFileAttributesA("Z:\\");
		return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	void Init()
	{
		if (s_running)
			return;

		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		{
			CryLogAlways("[WinlatorXR] WSAStartup failed");
			return;
		}
		s_wsaStarted = true;

		if (!s_csInitialized)
		{
			InitializeCriticalSection(&s_cs);
			s_csInitialized = true;
		}

		WriteMarkerFiles();

		s_running = true;
		s_recvThread = (HANDLE)_beginthreadex(nullptr, 0, ReceiveThreadProc, nullptr, 0, nullptr);
		if (!s_recvThread)
		{
			CryLogAlways("[WinlatorXR] Failed to start UDP receiver thread");
			s_running = false;
			return;
		}

		// Announce ourselves immediately so WinlatorXR starts forwarding pose/input data to us. Start in
		// flat "screen" mode (the intro videos and menu are 2D); VRManager switches to modeVr=1/mode3d=1
		// per frame once it composes real side-by-side frames. fov 0/0 keeps the headset's native FOV,
		// which WinlatorXR then reports back to us in every packet.
		SendState(0.f, 0.f, 2, 0, 0.f, 0.f);
	}

	void Shutdown()
	{
		if (!s_running)
			return;

		s_running = false;
		if (s_recvSocket != INVALID_SOCKET)
		{
			// unblocks the recvfrom() call in ReceiveLoop so the thread can exit
			closesocket(s_recvSocket);
			s_recvSocket = INVALID_SOCKET;
		}
		if (s_recvThread)
		{
			WaitForSingleObject(s_recvThread, 2000);
			CloseHandle(s_recvThread);
			s_recvThread = nullptr;
		}
		if (s_sendSocket != INVALID_SOCKET)
		{
			closesocket(s_sendSocket);
			s_sendSocket = INVALID_SOCKET;
		}

		if (s_wsaStarted)
		{
			WSACleanup();
			s_wsaStarted = false;
		}
	}

	InputState GetLatestState()
	{
		InputState copy;
		EnterCriticalSection(&s_cs);
		copy = s_latest;
		LeaveCriticalSection(&s_cs);
		return copy;
	}

	void SendState(float lHaptics, float rHaptics, int modeVr, int mode3d, float fovX, float fovY)
	{
		// this is called once per rendered frame, so keep one socket around instead of creating one per call
		if (s_sendSocket == INVALID_SOCKET)
		{
			s_sendSocket = socket(AF_INET, SOCK_DGRAM, 0);
			if (s_sendSocket == INVALID_SOCKET)
				return;
		}

		sockaddr_in target;
		memset(&target, 0, sizeof(target));
		target.sin_family = AF_INET;
		target.sin_port = htons(kSendPort);
		target.sin_addr.s_addr = inet_addr("127.0.0.1");

		char msg[256];
		sprintf(msg, "%g %g %d %d %g %g", lHaptics, rHaptics, modeVr, mode3d, fovX, fovY);

		sendto(s_sendSocket, msg, (int)strlen(msg), 0, (sockaddr*)&target, sizeof(target));
	}
}
