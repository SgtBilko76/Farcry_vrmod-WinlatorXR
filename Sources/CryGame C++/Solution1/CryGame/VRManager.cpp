#include "StdAfx.h"
#include "VRManager.h"
#include "Cry_Camera.h"
#include "xplayer.h"
#include "ComPtr.h"
#include <vulkan/vulkan.h>

//#include <d3d9_interfaces.h>
#include <d3d9.h>
#include <openvr.h>

#include "UISystem.h"
#include "VRRenderer.h"
#include "WeaponClass.h"
#include "XVehicle.h"
#include "WinlatorXR.h"
#include "Hooks.h"
#include <tlhelp32.h>
#include <stdlib.h>

// STLport shadows <signal.h> with a broken wrapper, so declare the one UCRT function we need ourselves
typedef void (__cdecl *crt_signal_handler_t)(int);
extern "C" __declspec(dllimport) crt_signal_handler_t __cdecl signal(int sig, crt_signal_handler_t handler);
#ifndef SIGABRT
#define SIGABRT 22
#endif


HMODULE GetCurrentModule()
{
	HMODULE module = nullptr;
	GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)GetCurrentModule, &module);
	return module;
}


VRManager s_VRManager;
VRManager* gVR = &s_VRManager;

extern "C" void dxvkLockSubmissionQueue(IDirect3DDevice9Ex * device, bool flush);
extern "C" void dxvkReleaseSubmissionQueue(IDirect3DDevice9Ex * device);
extern "C" HRESULT dxvkFillVulkanTextureInfo(IDirect3DDevice9Ex * device, IDirect3DTexture9 * texture, vr::VRVulkanTextureData_t & data, VkImageLayout & layout);
extern "C" void dxvkTransitionImageLayout(IDirect3DDevice9Ex * device, IDirect3DTexture9 * texture, VkImageLayout from, VkImageLayout to);

const float BinocularWidth = 0.5f;

// OpenVR: x = right, y = up, -z = forward
// FarCry: x = left, -y = forward, z = up
Matrix34 OpenVRToFarCry(const vr::HmdMatrix34_t &mat)
{
	Matrix34 m;
	m.m00 = mat.m[0][0];
	m.m01 = -mat.m[0][2];
	m.m02 = -mat.m[0][1];
	m.m03 = -mat.m[0][3];
	m.m10 = -mat.m[2][0];
	m.m11 = mat.m[2][2];
	m.m12 = mat.m[2][1];
	m.m13 = mat.m[2][3];
	m.m20 = -mat.m[1][0];
	m.m21 = mat.m[1][2];
	m.m22 = mat.m[1][1];
	m.m23 = mat.m[1][3];
	return m;
}

vr::HmdMatrix34_t FarCryToOpenVR(const Matrix34& mat)
{
	vr::HmdMatrix34_t res;
	res.m[0][0] = mat.m00;
	res.m[0][1] = -mat.m02;
	res.m[0][2] = -mat.m01;
	res.m[0][3] = -mat.m03;
	res.m[1][0] = -mat.m20;
	res.m[1][1] = mat.m22;
	res.m[1][2] = mat.m21;
	res.m[1][3] = mat.m23;
	res.m[2][0] = -mat.m10;
	res.m[2][1] = mat.m12;
	res.m[2][2] = mat.m11;
	res.m[2][3] = mat.m13;
	return res;
}

// Builds an OpenVR-style device-to-absolute matrix from a unit quaternion and position that use the
// OpenXR/OpenVR convention (x = right, y = up, -z = forward). Used to feed the WinlatorXR pose into the
// same m_headPose structure the OpenVR path fills, so the rest of the pose pipeline stays shared.
static vr::HmdMatrix34_t HmdMatrixFromQuatPos(float qx, float qy, float qz, float qw, float x, float y, float z)
{
	vr::HmdMatrix34_t m;
	m.m[0][0] = 1.f - 2.f * (qy * qy + qz * qz);
	m.m[0][1] = 2.f * (qx * qy - qw * qz);
	m.m[0][2] = 2.f * (qx * qz + qw * qy);
	m.m[0][3] = x;
	m.m[1][0] = 2.f * (qx * qy + qw * qz);
	m.m[1][1] = 1.f - 2.f * (qx * qx + qz * qz);
	m.m[1][2] = 2.f * (qy * qz - qw * qx);
	m.m[1][3] = y;
	m.m[2][0] = 2.f * (qx * qz - qw * qy);
	m.m[2][1] = 2.f * (qy * qz + qw * qx);
	m.m[2][2] = 1.f - 2.f * (qx * qx + qy * qy);
	m.m[2][3] = z;
	return m;
}

// --- CRT failure diagnostics (WinlatorXR) ----------------------------------------------------------
// A "Microsoft Visual C++ Runtime Library - Runtime Error" box on the headset means some module using
// the modern CRT called abort() (uncaught C++ exception, invalid parameter, pure virtual call ...).
// That includes CryGame.dll itself and dxvk's d3d9.dll. Far Cry's own crash handler never sees those,
// so log a module-resolved call stack before the CRT puts up its dialog.
// Everything is additionally appended, unbuffered, to vr_crash.txt next to the executable: the game's
// log is buffered and the process usually dies right after abort(), before that buffer is flushed.
static void WriteCrashFile(const char* line)
{
	HANDLE file = CreateFileA("vr_crash.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	WriteFile(file, line, (DWORD)strlen(line), &written, nullptr);
	WriteFile(file, "\r\n", 2, &written, nullptr);
	FlushFileBuffers(file);
	CloseHandle(file);
}

static void LogCallStack(const char* reason)
{
	void* frames[32];
	USHORT count = CaptureStackBackTrace(1, 32, frames, nullptr);
	char line[512];
	SYSTEMTIME st;
	GetLocalTime(&st);
	sprintf(line, "[%02d:%02d:%02d] [WinlatorXR] %s - call stack (%u frames):", st.wHour, st.wMinute, st.wSecond, reason, (unsigned)count);
	CryLogAlways("%s", line);
	WriteCrashFile(line);
	for (USHORT i = 0; i < count; ++i)
	{
		HMODULE module = nullptr;
		char moduleName[MAX_PATH] = "?";
		uintptr_t base = 0;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &module) && module)
		{
			GetModuleFileNameA(module, moduleName, MAX_PATH);
			base = (uintptr_t)module;
		}
		const char* shortName = strrchr(moduleName, '\\');
		shortName = shortName ? shortName + 1 : moduleName;
		sprintf(line, "  %2u) 0x%08X  %s+0x%X", (unsigned)i, (unsigned)(uintptr_t)frames[i], shortName, (unsigned)((uintptr_t)frames[i] - base));
		CryLogAlways("%s", line);
		WriteCrashFile(line);
	}
}

static void OnCrtAbort(int)
{
	LogCallStack("SIGABRT raised");
}

static void OnCrtInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
	LogCallStack("CRT invalid parameter");
}

static void OnCrtPureCall()
{
	LogCallStack("pure virtual function call");
}

// NOTE: earlier builds also MinHook-patched abort()/_amsg_exit() in ucrtbase/msvcrt/msvcr71 to grab a
// stack before the CRT's "Runtime Error" box. Those hooks never fired (MinHook can't reliably patch
// Wine's builtin CRTs under Box64's dynarec) and patching those functions is risky, so only the
// passive, in-process CRT handlers are installed now - they cost nothing and catch a purecall /
// invalid-parameter abort raised through CryGame's own CRT.
static void InstallCrtDiagnostics()
{
	signal(SIGABRT, OnCrtAbort);
	_set_invalid_parameter_handler(OnCrtInvalidParameter);
	_set_purecall_handler(OnCrtPureCall);
	CryLogAlways("[WinlatorXR] CRT diagnostics installed (SIGABRT/invalid-parameter/purecall handlers; stacks go to vr_crash.txt)");
}
// ---------------------------------------------------------------------------------------------------

struct VRManager::D3DResources
{
	ComPtr<IDirect3DDevice9Ex> device;
	ComPtr<IDirect3DTexture9> hudTexture;
	ComPtr<IDirect3DTexture9> stereoTexture;
	ComPtr<IDirect3DTexture9> eyeTextures[2];
};

VRManager::VRManager()
{
	m_d3d = new D3DResources;
	m_hmdTransform = Matrix34::CreateIdentity();
	// start out with an explicitly invalid pose: the WinlatorXR path only fills this once the first
	// packet has arrived, and ProcessInput checks bPoseIsValid before AwaitFrame has necessarily run
	memset(&m_headPose, 0, sizeof(m_headPose));
}


VRManager::~VRManager()
{
	// if Shutdown isn't properly called, we will get an infinite hang when trying to dispose of our D3D resources after
	// the game already shut down. So just let go here to avoid that
	m_d3d->device.Detach();
	delete m_d3d;
}

bool VRManager::Init(CXGame *game)
{
	if (m_initialized)
		return true;

	HMODULE module = GetCurrentModule();
	CryLogAlways("Initializing CryVR, base module address: 0x%x", module);

	m_pGame = game;

	m_usingWinlatorXR = WinlatorXR::IsLikelyPresent();

	if (m_usingWinlatorXR)
	{
		// Running under WinlatorXR (Wine on a standalone Quest/Pico headset) - use its XrAPI UDP
		// protocol instead of OpenVR/SteamVR, which isn't available in that environment.
		// NOTE: this is currently a minimal, log-only integration (detection + packet parsing only).
		// Pose/rendering/input/haptics wiring land in later changes; until then, VR behaves as if
		// running in the existing "not initialized" flat/mono fallback mode.
		CryLogAlways("Detected WinlatorXR environment (Z:\\ drive present) - using WinlatorXR XrAPI backend instead of OpenVR");
		WinlatorXR::Init();

		// Far Cry's crash handler can't attribute addresses to modules under Wine ("Exception Module:
		// <Unknown>"), so dump the module map once - it makes the call stacks in log.txt readable.
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
		if (snapshot != INVALID_HANDLE_VALUE)
		{
			MODULEENTRY32 me;
			me.dwSize = sizeof(me);
			if (Module32First(snapshot, &me))
			{
				do
				{
					CryLogAlways("[WinlatorXR] module %-28s base 0x%08X size 0x%08X", me.szModule, (unsigned)(uintptr_t)me.modBaseAddr, (unsigned)me.modBaseSize);
				} while (Module32Next(snapshot, &me));
			}
			CloseHandle(snapshot);
		}

		m_verticalFov = tanf(DEG2RAD(90.f) / 2.f);
		m_horizontalFov = tanf(DEG2RAD(90.f) / 2.f);
		m_vertRenderScale = 1.f;
	}
	else
	{
		vr::EVRInitError error;
		vr::VR_Init(&error, vr::VRApplication_Scene);
		if (error != vr::VRInitError_None)
		{
			CryError("Failed to initialize OpenVR: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
			return false;
		}

		vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseStanding);

		vr::VROverlay()->CreateOverlay("FarCryHud", "FarCry HUD", &m_hudOverlay);
		vr::VROverlay()->SetOverlaySortOrder(m_hudOverlay, 1);
		vr::VROverlay()->CreateOverlay("FarCry3D", "FarCry 3D", &m_3DOverlay);
		vr::VROverlay()->SetOverlayWidthInMeters(m_hudOverlay, 2.f);
		vr::VROverlay()->ShowOverlay(m_hudOverlay);

		vr::VROverlay()->CreateOverlay("FarCry3D", "FarCry 3D", &m_3DOverlay);
		vr::VROverlay()->SetOverlayFlag(m_3DOverlay, vr::VROverlayFlags_SideBySide_Parallel, true);
		vr::VROverlay()->HideOverlay(m_3DOverlay);

		float ll, lr, lt, lb, rl, rr, rt, rb;
		vr::VRSystem()->GetProjectionRaw(vr::Eye_Left, &ll, &lr, &lt, &lb);
		vr::VRSystem()->GetProjectionRaw(vr::Eye_Right, &rl, &rr, &rt, &rb);
		CryLogAlways(" Left eye - l: %.2f  r: %.2f  t: %.2f  b: %.2f", ll, lr, lt, lb);
		CryLogAlways("Right eye - l: %.2f  r: %.2f  t: %.2f  b: %.2f", rl, rr, rt, rb);
		m_verticalFov = max(max(fabsf(lt), fabsf(lb)), max(fabsf(rt), fabsf(rb)));
		m_horizontalFov = max(max(fabsf(ll), fabsf(lr)), max(fabsf(rl), fabsf(rr)));
		m_vertRenderScale = 2.f * m_verticalFov / min(fabsf(lt) + fabsf(lb), fabsf(rt) + fabsf(rb));
		CryLogAlways("VR vert fov: %.2f  horz fov: %.2f  vert scale: %.2f", m_verticalFov, m_horizontalFov, m_vertRenderScale);
	}

	RegisterCVars();

	if (m_usingWinlatorXR)
	{
		// WinlatorXR launches the game with DXVK_FRAME_RATE=72 (dxvk's frame limiter). Under Wine/Box64
		// that limiter's sleeps are coarse enough that the game locks to 72/2 or 72/4 fps as soon as a
		// frame takes slightly longer than a refresh. dxvk re-reads the variable whenever it recreates
		// its presenter, which happens on the device reset our first render-resolution change causes -
		// so overriding it here is early enough.
		char maxFps[16];
		sprintf(maxFps, "%d", max(vr_winlatorxr_max_fps, 0));
		SetEnvironmentVariableA("DXVK_FRAME_RATE", maxFps);
		CryLogAlways("[WinlatorXR] DXVK_FRAME_RATE overridden to %s (vr_winlatorxr_max_fps)", maxFps);

		InstallCrtDiagnostics();
	}

	if (m_usingWinlatorXR)
	{
		// controller input comes from the XrAPI packet; only controller vibration is available as
		// haptics on a standalone headset (no bHaptics vest / ProTubeVR there)
		m_inputReady = m_input.InitWinlatorXR(game);
		m_vrHaptics.InitControllerHaptics(game, &m_input);
	}
	else
	{
		m_inputReady = m_input.Init(game);
		m_vrHaptics.Init(game, &m_input);
	}

	m_hmdTransform = Matrix34::CreateIdentity();
	m_referencePosition = Vec3(0, 0, 0);
	m_referenceYaw = 0;
	m_uncommittedReferenceYaw = 0;
	m_uncommittedReferencePosition = Vec3(0, 0, 0);

	m_initialized = true;
	return true;
}

void VRManager::Shutdown()
{
	m_d3d->device.Reset();

	if (!m_initialized)
		return;

	if (m_usingWinlatorXR)
	{
		WinlatorXR::Shutdown();
	}
	else
	{
		vr::VROverlay()->DestroyOverlay(m_hudOverlay);
		vr::VR_Shutdown();
	}
	m_initialized = false;
}

void VRManager::Update()
{
	if (!m_initialized)
		return;

	// (VRHaptics itself no-ops whatever wasn't initialised for the current backend)
	m_vrHaptics.Update();

	HandleEvents();

	int wantedWindowWidth = vr_window_width;
	int wantedWindowHeight = vr_window_height;
	if (m_usingWinlatorXR)
	{
		// WinlatorXR grabs the whole X screen and splits it into the two eyes, so the game window must
		// cover exactly that screen - a smaller window (e.g. after the in-game video options wrote a
		// resolution back to the config) shows up as a misaligned, blurry crop in the headset.
		int screenWidth = GetSystemMetrics(SM_CXSCREEN);
		int screenHeight = GetSystemMetrics(SM_CYSCREEN);
		if (screenWidth > 0 && screenHeight > 0)
		{
			wantedWindowWidth = screenWidth;
			wantedWindowHeight = screenHeight;
		}
	}
	if (wantedWindowWidth != m_curWindowWidth || wantedWindowHeight != m_curWindowHeight)
	{
		if (m_usingWinlatorXR)
			CryLogAlways("[WinlatorXR] sizing game window to the X screen: %d x %d", wantedWindowWidth, wantedWindowHeight);
		m_pGame->m_pRenderer->ChangeResolution(wantedWindowWidth, wantedWindowHeight, 32, 0, false);
		m_curWindowWidth = wantedWindowWidth;
		m_curWindowHeight = wantedWindowHeight;
	}
}

void VRManager::AwaitFrame()
{
	if (!m_initialized || !m_d3d->device)
		return;

	if (m_usingWinlatorXR)
	{
		// WinlatorXR has no blocking WaitGetPoses equivalent - it streams poses over UDP at the
		// headset's refresh rate, so just pick up the most recent one for this frame
		UpdateWinlatorXRPose();
	}
	else
	{
		dxvkLockSubmissionQueue(m_d3d->device.Get(), false);
		vr::VRCompositor()->WaitGetPoses(&m_headPose, 1, nullptr, 0);
		dxvkReleaseSubmissionQueue(m_d3d->device.Get());
	}

	UpdateHmdTransform();
}

void VRManager::HandleEvents()
{
	if (m_usingWinlatorXR)
	{
		// WinlatorXR has no event queue: there is no quit/dashboard/recenter notification to handle.
		// (Poses are consumed in AwaitFrame via UpdateWinlatorXRPose.)
		return;
	}

	vr::VREvent_t event;
	while (vr::VRSystem()->PollNextEvent(&event, sizeof(vr::VREvent_t)))
	{
		if (event.eventType == vr::VREvent_SeatedZeroPoseReset)
		{
			vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &m_headPose, 1);
			RecalibrateView();
		}
		if (event.eventType == vr::VREvent_Quit)
		{
			vr::VRSystem()->AcknowledgeQuit_Exiting();
			m_pGame->GetSystem()->Quit();
		}
		if (event.eventType == vr::VREvent_DashboardActivated)
		{
			m_pGame->GotoMenu(false);
		}
	}
}

void VRManager::CaptureEye(int eye)
{
	if (!m_d3d->device)
		return;

	if (!m_d3d->eyeTextures[eye])
	{
		CreateEyeTexture(eye);
		if (!m_d3d->eyeTextures[eye])
			return;
	}

	D3DSURFACE_DESC desc;
	m_d3d->eyeTextures[eye]->GetLevelDesc(0, &desc);
	vector2di expectedSize = GetRenderSize();
	if (desc.Width != expectedSize.x || desc.Height != expectedSize.y)
	{
		// recreate with new resolution
		CreateEyeTexture(eye);
		if (!m_d3d->eyeTextures[eye])
			return;
	}

	// acquire and copy the current swap chain buffer to the eye texture. Under WinlatorXR the eye was
	// rendered into its own half of the full-screen back buffer (see RenderSingleEye's viewport), so
	// copy exactly that half (1:1, no scale).
	ComPtr<IDirect3DSurface9> backBuffer;
	m_d3d->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer.GetAddressOf());
	ComPtr<IDirect3DSurface9> texSurface;
	m_d3d->eyeTextures[eye]->GetSurfaceLevel(0, texSurface.GetAddressOf());
	vector2di rs = GetRenderSize();
	RECT srcRect = { 0, 0, rs.x, rs.y };
	HRESULT hr = m_d3d->device->StretchRect(backBuffer.Get(), m_usingWinlatorXR ? &srcRect : nullptr, texSurface.Get(), nullptr, D3DTEXF_POINT);
	if (hr != S_OK)
	{
		CryLogAlways("ERROR: Capturing eye failed: %i", hr);
	}
}

void VRManager::CaptureStereo(int eye)
{
	if (!m_d3d->device)
		return;

	if (!m_d3d->stereoTexture)
	{
		CreateStereoTexture();
		if (!m_d3d->stereoTexture)
			return;
	}

	D3DSURFACE_DESC desc;
	m_d3d->stereoTexture->GetLevelDesc(0, &desc);
	vector2di expectedSize = GetRenderSize();
	expectedSize.x *= 2;
	if (desc.Width != expectedSize.x || desc.Height != expectedSize.y)
	{
		// recreate with new resolution
		CreateStereoTexture();
		if (!m_d3d->stereoTexture)
			return;
	}

	// acquire and copy the current back buffer to the right part of the stereo texture
	ComPtr<IDirect3DSurface9> backBuffer;
	m_d3d->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer.GetAddressOf());
	ComPtr<IDirect3DSurface9> texSurface;
	m_d3d->stereoTexture->GetSurfaceLevel(0, texSurface.GetAddressOf());
	RECT dst;
	dst.top = 0;
	dst.bottom = expectedSize.y;
	if (eye == 0)
	{
		dst.left = 0;
		dst.right = expectedSize.x / 2;
	}
	else
	{
		dst.left = expectedSize.x / 2;
		dst.right = expectedSize.x;
	}
	HRESULT hr = m_d3d->device->StretchRect(backBuffer.Get(), nullptr, texSurface.Get(), &dst, D3DTEXF_POINT);
	if (hr != S_OK)
	{
		CryLogAlways("ERROR: Capturing stereo failed: %i", hr);
	}
}

void VRManager::CaptureHUD()
{
	if (!m_d3d->device)
		return;

	if (!m_d3d->hudTexture)
	{
		CreateHUDTexture();
		if (!m_d3d->hudTexture)
			return;
	}

	D3DSURFACE_DESC desc;
	m_d3d->hudTexture->GetLevelDesc(0, &desc);
	vector2di expectedSize = GetRenderSize();
	if (desc.Width != expectedSize.x || desc.Height != expectedSize.y)
	{
		// recreate with new resolution
		CreateHUDTexture();
		if (!m_d3d->hudTexture)
			return;
	}

	// acquire and copy the current back buffer to the HUD texture. Under WinlatorXR the HUD/2D pass is
	// scissored to the top-left GetRenderSize region (one eye), so copy just that region.
	ComPtr<IDirect3DSurface9> backBuffer;
	m_d3d->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer.GetAddressOf());
	ComPtr<IDirect3DSurface9> texSurface;
	m_d3d->hudTexture->GetSurfaceLevel(0, texSurface.GetAddressOf());
	vector2di rsHud = GetRenderSize();
	RECT srcRectHud = { 0, 0, rsHud.x, rsHud.y };
	HRESULT hr = m_d3d->device->StretchRect(backBuffer.Get(), m_usingWinlatorXR ? &srcRectHud : nullptr, texSurface.Get(), nullptr, D3DTEXF_POINT);
	if (hr != S_OK)
	{
		CryLogAlways("ERROR: Capturing HUD failed: %i", hr);
	}
}

void VRManager::MirrorEyeToBackBuffer()
{
	if (m_usingWinlatorXR)
	{
		// the back buffer *is* the headset image under WinlatorXR, so instead of a desktop mirror we
		// compose the actual stereo frame here
		ComposeWinlatorXRFrame();
		return;
	}

	if (!gVRRenderer->ShouldRenderVR() || gVRRenderer->ShouldRender2D())
		return;

	int eye = clamp_tpl(vr_mirrored_eye, 0, 1);

	if (!m_d3d->device || !m_d3d->eyeTextures[eye] || m_pGame->IsInMenu())
		return;

	// figure out aspect ratio correction
	float windowAspect = (float)vr_window_width / vr_window_height;
	float vrAspect = (float)m_pGame->m_pRenderer->GetWidth() / m_pGame->m_pRenderer->GetHeight();
	float scale = vrAspect / windowAspect;

	Vec2 size;
	if (scale < 1.f)
	{
		// mirror view is wider than rendered eye
		size.x = 1.f;
		size.y = scale;
	} else
	{
		// rendered eye is wider than mirror view
		size.x = scale;
		size.y = 1.f;
	}
	Vec2 offset(.5f - .5f * size.x, .5f - .5f * size.y);

	struct Vertex
	{
		float x, y, z, w;
		float u, v;
	};
	Vertex vertices[4] =
	{
		{ -0.5f, -0.5f, 0.0f, 1.0f, offset.x, offset.y },
		{ m_pGame->m_pRenderer->GetWidth() - 0.5f, -0.5f, 0.0f, 1.0f, offset.x + size.x, offset.y },
		{ m_pGame->m_pRenderer->GetWidth() - 0.5f, m_pGame->m_pRenderer->GetHeight() - 0.5f, 0.0f, 1.0f, offset.x + size.x, offset.y + size.y },
		{ -0.5f, m_pGame->m_pRenderer->GetHeight() - 0.5f, 0.0f, 1.0f, offset.x, offset.y + size.y },
	};

	m_pGame->m_pRenderer->ResetToDefault();

	// save current render state
	IDirect3DStateBlock9* stateBlock = nullptr;
	m_d3d->device->CreateStateBlock(D3DSBT_ALL, &stateBlock);

	// set state for fullscreen quad
	m_d3d->device->SetRenderState(D3DRS_LIGHTING, FALSE);
	m_d3d->device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	m_d3d->device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
	m_d3d->device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	m_d3d->device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTALPHA);
	m_d3d->device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_DESTALPHA);
	m_d3d->device->SetRenderState(D3DRS_VERTEXBLEND, FALSE);
	m_d3d->device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	m_d3d->device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
	m_d3d->device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	m_d3d->device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	m_d3d->device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	m_d3d->device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	m_d3d->device->SetTexture(0, m_d3d->eyeTextures[eye].Get());
	m_d3d->device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	m_d3d->device->SetVertexShader(nullptr);
	m_d3d->device->SetPixelShader(nullptr);

	// draw quad
	m_d3d->device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, vertices, sizeof(Vertex));

	// restore state
	if (stateBlock)
	{
		stateBlock->Apply();
		stateBlock->Release();
	}
}

void VRManager::SetDevice(IDirect3DDevice9Ex *device)
{
	if (device != m_d3d->device.Get())
		InitDevice(device);
}

void VRManager::FinishFrame()
{
	if (!m_initialized)
		return;

	if (m_usingWinlatorXR)
	{
		// the frame itself was already composed into the back buffer by ComposeWinlatorXRFrame (called
		// from the pre-present hook); all that is left is telling WinlatorXR how to display it.
		// fov 0/0 = "keep the headset's native FOV", which WinlatorXR then reports back to us in every
		// packet and which is what our cameras render with (see UpdateWinlatorXRPose).
		// controller vibration: WinlatorXR treats the values as a per-frame level (with its own decay),
		// so just forward whatever amplitude VRHaptics last asked for
		float lHaptics = m_inputReady ? m_input.GetWinlatorHapticAmplitude(0) : 0.f;
		float rHaptics = m_inputReady ? m_input.GetWinlatorHapticAmplitude(1) : 0.f;
		WinlatorXR::SendState(lHaptics, rHaptics, m_winlatorModeVr, m_winlatorMode3d, 0.f, 0.f);
		m_wasBinocular = m_pGame->AreBinocularsActive();
		// alternate-eye rendering: next frame renders and carries the other eye
		if (UseWinlatorAER())
			m_winlatorAerEye ^= 1;

		// periodic frame-rate report: there is no overlay/tooling on the headset, and the frame rate
		// decides how much WinlatorXR's reprojection has to warp our (stale) frames
		static int s_frames = 0;
		static float s_lastReport = 0.f;
		float now = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime();
		++s_frames;
		if (s_lastReport == 0.f)
			s_lastReport = now;
		else if (now - s_lastReport >= 10.f)
		{
			CryLogAlways("[WinlatorXR] %.1f fps (%s, %dx%d per eye)", s_frames / (now - s_lastReport), UseWinlatorAER() ? "AER" : "SBS", GetRenderSize().x, GetRenderSize().y);
			s_frames = 0;
			s_lastReport = now;
		}
		return;
	}

	if (!m_d3d->device || !m_d3d->eyeTextures[0] || !m_d3d->eyeTextures[1])
		return;

	vr::VRVulkanTextureData_t vkTexData[4];
	VkImageLayout origLayout[4];

	PrepareTextureForSubmission(m_d3d->eyeTextures[0].Get(), vkTexData[0], origLayout[0]);
	PrepareTextureForSubmission(m_d3d->eyeTextures[1].Get(), vkTexData[1], origLayout[1]);
	PrepareTextureForSubmission(m_d3d->hudTexture.Get(), vkTexData[2], origLayout[2]);
	PrepareTextureForSubmission(m_d3d->stereoTexture.Get(), vkTexData[3], origLayout[3]);
	dxvkLockSubmissionQueue(m_d3d->device.Get(), true);

	for (int eye = 0; eye < 2; ++eye) 
	{
		// game is currently using symmetric projection, we need to cut off the texture accordingly
		vr::VRTextureBounds_t bounds;
		GetEffectiveRenderLimits(eye, &bounds.uMin, &bounds.uMax, &bounds.vMin, &bounds.vMax);

		vr::Texture_t vrTexData;
		vrTexData.eColorSpace = vr::ColorSpace_Auto;
		vrTexData.eType = vr::TextureType_Vulkan;
		vrTexData.handle = &vkTexData[eye];

		auto error = vr::VRCompositor()->Submit(eye == 0 ? vr::Eye_Left : vr::Eye_Right, &vrTexData, &bounds);
		if (error != vr::VRCompositorError_None && error != vr::VRCompositorError_AlreadySubmitted)
		{
			CryLogAlways("Submitting eye texture failed: %i", error);
		}
	}

	vr::Texture_t texInfo;
	texInfo.eColorSpace = vr::ColorSpace_Auto;
	texInfo.eType = vr::TextureType_Vulkan;
	texInfo.handle = (void*)&vkTexData[2];
	vr::VROverlay()->SetOverlayTexture(m_hudOverlay, &texInfo);

	texInfo.handle = (void*)&vkTexData[3];
	vr::VROverlay()->SetOverlayTexture(m_3DOverlay, &texInfo);

	// apparently we need to set the overlay mouse scale to some values with the proper aspect ratio, otherwise it just won't work
	vr::HmdVector2_t mouseScale;
	mouseScale.v[0] = m_pGame->m_pRenderer->GetWidth();
	mouseScale.v[1] = m_pGame->m_pRenderer->GetHeight();
	vr::VROverlay()->SetOverlayMouseScale(m_hudOverlay, &mouseScale);

	vr::VRCompositor()->PostPresentHandoff();
	dxvkReleaseSubmissionQueue(m_d3d->device.Get());

	PostSubmissionTransitionTexture(m_d3d->eyeTextures[0].Get(), origLayout[0]);
	PostSubmissionTransitionTexture(m_d3d->eyeTextures[1].Get(), origLayout[1]);
	PostSubmissionTransitionTexture(m_d3d->hudTexture.Get(), origLayout[2]);
	PostSubmissionTransitionTexture(m_d3d->stereoTexture.Get(), origLayout[3]);

	m_wasBinocular = m_pGame->AreBinocularsActive();
}

vector2di VRManager::GetWinlatorBackbufferSize() const
{
	int w = GetSystemMetrics(SM_CXSCREEN);
	int h = GetSystemMetrics(SM_CYSCREEN);
	if (w <= 0 || h <= 0)
	{
		w = vr_window_width;
		h = vr_window_height;
	}
	return vector2di(w, h);
}

vector2di VRManager::GetRenderSize() const
{
	if (!m_initialized)
		return vector2di(1280, 800);

	if (m_usingWinlatorXR)
	{
		// The eye is rendered at its true FOV aspect (~1.10) into the top-left of the full-screen back
		// buffer via a viewport, captured, then anamorphically fit into its side-by-side half by the
		// composite. Rendering at the FOV aspect keeps the horizontal/vertical FOV correct (a half's
		// 0.89 aspect would render a too-narrow "scope" FOV). Height is the resolution knob; clamp so
		// the eye fits in the back buffer.
		vector2di backbuffer = GetWinlatorBackbufferSize();
		int height = max(vr_winlatorxr_render_height, 240);
		int width = (int)(height * m_horizontalFov / m_verticalFov);
		if (width > backbuffer.x) { height = height * backbuffer.x / width; width = backbuffer.x; }
		if (height > backbuffer.y) { width = width * backbuffer.y / height; height = backbuffer.y; }
		return vector2di(width, height);
	}

	uint32_t width, height;
	vr::VRSystem()->GetRecommendedRenderTargetSize(&width, &height);
	height *= m_vertRenderScale;
	width = height * m_horizontalFov / m_verticalFov;
	return vector2di(width, height);
}

void VRManager::ModifyViewCamera(int eye, CCamera& cam)
{
	if (IsEquivalent(cam.GetPos(), Vec3(0, 0, 0), VEC_EPSILON))
	{
		// no valid camera set, leave it
		return;
	}

	if (!m_initialized || (m_usingWinlatorXR && !m_headPose.bPoseIsValid))
	{
		// no tracking available (yet): fall back to a simple flat stereo offset. Under WinlatorXR
		// this covers the frames before the first UDP pose packet has arrived.
		if (eye == 1)
		{
			Vec3 pos = cam.GetPos();
			pos.x += 0.1f;
			cam.SetPos(pos);
		}
		return;
	}

	if (m_pGame->AreBinocularsActive() || m_wasBinocular)
	{
		cam = m_binocularOriginalPlayerCam;
	}

	Ang3 angles = cam.GetAngles();
	Vec3 position = cam.GetPos();
	position.z -= GetEffectiveReferenceHeight();

	CPlayer* player = m_pGame->GetLocalPlayer();
	if (player && !m_pGame->IsCutSceneActive())
	{
		position = player->GetVRBasePos();
	}

	angles = Deg2Rad(angles);
	// eliminate pitch and roll
	angles.y = 0;
	angles.x = 0;

	if (eye == 0)
	{
		// manage the aiming deadzone in which the camera should not be rotated
		float yawAngle = DEG2RAD(AngleMod(RAD2DEG(angles.z)));
		float yawDiff = yawAngle - m_prevViewYaw;
		if (yawDiff < -gf_PI)
			yawDiff += 2 * gf_PI;
		else if (yawDiff > gf_PI)
			yawDiff -= 2 * gf_PI;

		float maxDiff = vr_yaw_deadzone_angle * gf_PI / 180.f;
		if (yawDiff > maxDiff)
			m_prevViewYaw += yawDiff - maxDiff;
		if (yawDiff < -maxDiff)
			m_prevViewYaw += yawDiff + maxDiff;
		if (m_prevViewYaw > gf_PI)
			m_prevViewYaw -= 2*gf_PI;
		if (m_prevViewYaw < -gf_PI)
			m_prevViewYaw += 2*gf_PI;

		CPlayer *pPlayer = 0;
		if (m_pGame->GetMyPlayer())
		{
			m_pGame->GetMyPlayer()->GetContainer()->QueryContainerInterface(CIT_IPLAYER,(void **)&pPlayer);
		}
		if (pPlayer && pPlayer->GetVehicle())
		{
			// don't use this while in a vehicle, it feels off
			m_prevViewYaw = angles.z;
		}
	}
	if (!UseMotionControllers())
		angles.z = m_prevViewYaw;

	Matrix34 viewMat;
	viewMat.SetRotationXYZ(angles, position);

	Matrix34 eyeMat = GetEyeToHeadTransform(eye);
	Matrix34 headMat = m_hmdTransform;
	viewMat = viewMat * headMat * eyeMat;

	position = viewMat.GetTranslation();
	cam.SetPos(position);
	angles.SetAnglesXYZ(Matrix33(viewMat));
	angles.Rad2Deg();
	cam.SetAngle(angles);

	// we don't have obvious access to the projection matrix, and the camera code is written with symmetric projection in mind
	// for now, set up a symmetric FOV and cut off parts of the image during submission
	vector2di renderSize = GetRenderSize();
	// CCamera semantics: vertical fov = fov * projectionRatio (ratio defaults to height/width), and the
	// horizontal extent follows from square pixels. For the anamorphic WinlatorXR render the pixel
	// width is scaled up but the projection must stay that of the logical eye, so pin the ratio to the
	// logical aspect explicitly.
	float logicalWidth = renderSize.x / (float)WinlatorRenderScaleX();
	float vertFovAngle = atanf(m_verticalFov) * 2;
	float horzFovAngle = vertFovAngle * logicalWidth / (float)renderSize.y;
	float projectionRatio = WinlatorRenderScaleX() > 1 ? renderSize.y / logicalWidth : 0.f;
	cam.Init(renderSize.x, renderSize.y, horzFovAngle, cam.GetZMax(), projectionRatio, cam.GetZMin());
	cam.Update();

	// but we can set up frustum planes for our asymmetric projection, which should help culling accuracy.
	if (!m_usingWinlatorXR)
	{
		// (WinlatorXR only ever has a symmetric FOV, so there is nothing to do there)
		float tanl, tanr, tant, tanb;
		vr::VRSystem()->GetProjectionRaw(eye == 0 ? vr::Eye_Left : vr::Eye_Right, &tanl, &tanr, &tant, &tanb);
		//cam.UpdateFrustumFromVRRaw(tanl, tanr, -tanb, -tant);
	}
}

void VRManager::Modify2DCamera(CCamera& cam)
{
	// in some instances (e.g. binoculars, weapon zoom) we still want to include head movements in the camera orientation

	if (IsEquivalent(cam.GetPos(), Vec3(0, 0, 0), VEC_EPSILON))
	{
		// no valid camera set, leave it
		return;
	}

	if (m_pGame->IsCutSceneActive() || IsDrivingVehicleInCinemaMode())
		return;

	if (m_pGame->AreBinocularsActive())
	{
		// already corrected in player cam by necessity - otherwise, the motion tracking markers just don't display at the right position
		return;
	}

	Ang3 angles = cam.GetAngles();
	Vec3 position = cam.GetPos();
	position.z -= GetEffectiveReferenceHeight();

	CPlayer* player = m_pGame->GetLocalPlayer();
	if (player && !m_pGame->IsCutSceneActive())
	{
		position = player->GetVRBasePos();
	}

	angles = Deg2Rad(angles);
	// eliminate pitch and roll
	angles.y = 0;
	angles.x = 0;

	Matrix34 viewMat = Matrix34::CreateRotationXYZ(angles, position);

	Matrix34 headMat = m_hmdTransform;
	Matrix34 modifiedViewMat = viewMat * headMat;

	position = modifiedViewMat.GetTranslation();
	cam.SetPos(position);
	angles.SetAnglesXYZ(Matrix33(modifiedViewMat));
	angles.Rad2Deg();
	cam.SetAngle(angles);

	if (player && player->IsWeaponZoomActive())
	{
		// set camera to weapon firing pos, instead
		Vec3 muzzlePos, muzzleAngles;
		player->GetFirePosAngles(muzzlePos, muzzleAngles);
		cam.SetPos(muzzlePos);
		cam.SetAngle(muzzleAngles);
	}
}

void VRManager::Modify3DCamera(int eye, CCamera& cam)
{
	// start from the 2D camera setup
	Modify2DCamera(cam);

	float eyeShift = 0.025f;
	//cam.SetZMin(0.75f);
	cam.Update();

	// shift position slightly based on eye
	Matrix34 camTransform = Matrix34::CreateRotationXYZ(Deg2Rad(cam.GetAngles()), cam.GetPos());
	Matrix34 shift = Matrix34::CreateTranslationMat(Vec3(eye == 0 ? eyeShift : -eyeShift, 0, 0));
	camTransform = camTransform * shift;

	cam.SetPos(camTransform.GetTranslation() - 0.f * camTransform.GetForward());
	cam.SetAngle(ToAnglesDeg(camTransform));
}

void VRManager::ModifyBinocularCamera(IEntityCamera* cam)
{
	m_binocularOriginalPlayerCam = cam->GetCamera();

	if (!cam || !UseMotionControllers() || !m_pGame->AreBinocularsActive())
		return;

	Ang3 angles = cam->GetAngles();
	Vec3 position = cam->GetPos();
	position.z -= GetEffectiveReferenceHeight();

	CPlayer* player = m_pGame->GetLocalPlayer();
	if (player && !m_pGame->IsCutSceneActive())
	{
		position = player->GetVRBasePos();
	}

	angles = Deg2Rad(angles);
	// eliminate pitch and roll
	angles.y = 0;
	angles.x = 0;
	Matrix34 viewMat = Matrix34::CreateRotationXYZ(angles, position);

	// set camera to off hand position, instead
	Matrix34 offset = Matrix34::CreateTranslationMat(Vec3(-vr_binocular_size / 2, 0, vr_binocular_size / 2));
	Matrix34 controllerTransform = GetControllerTransform(m_pGame->g_LeftHanded->GetIVal() == 1 ? 1 : 0);
	Matrix34 modifiedViewMat = viewMat * controllerTransform * offset;
	m_curBinocularPos = modifiedViewMat.GetTranslation();
	cam->SetPos(m_curBinocularPos);
	angles.SetAnglesXYZ(Matrix33(modifiedViewMat));
	angles.Rad2Deg();

	// smooth rotation for a more stable zoom
	Vec3 smoothedAngles = angles;
	float factor = 0.025 * (DEFAULT_FOV / cam->GetFov());
	float yawPitchDecay = powf(2.f, -m_pGame->GetSystem()->GetITimer()->GetFrameTime() / factor);
	smoothedAngles.z = angles.z + GetAngleDifference360(m_curBinocularAngles.z, angles.z) * yawPitchDecay;
	smoothedAngles.x = angles.x + GetAngleDifference360(m_curBinocularAngles.x, angles.x) * yawPitchDecay;
	m_curBinocularAngles = smoothedAngles;

	cam->SetAngles(smoothedAngles);
}

void VRManager::GetEffectiveRenderLimits(int eye, float* left, float* right, float* top, float* bottom)
{
	if (m_usingWinlatorXR)
	{
		// WinlatorXR presents a symmetric projection (fovH x fovV), and that is exactly what the
		// camera renders (see ModifyViewCamera), so the whole eye image is used - no crop needed
		*left = 0.f; *right = 1.f; *top = 0.f; *bottom = 1.f;
		return;
	}

	float l, r, t, b;
	vr::VRSystem()->GetProjectionRaw(eye == 0 ? vr::Eye_Left : vr::Eye_Right, &l, &r, &t, &b);
	*left = 0.5f + 0.5f * l / m_horizontalFov;
	*right = 0.5f + 0.5f * r / m_horizontalFov;
	*top = 0.5f - 0.5f * b / m_verticalFov;
	*bottom = 0.5f - 0.5f * t / m_verticalFov;
}

void VRManager::ProcessInput()
{
	bool firstValidPose = !m_referenceCalibrated && m_headPose.bPoseIsValid;
	if (firstValidPose)
	{
		// establishes the reference yaw / floor height once tracking is available (both backends)
		RecalibrateView();
	}

	UpdateDesktopInputBlock();

	// Everything touching vr::VROverlay() below is SteamVR-only. Under WinlatorXR the HUD is composed
	// into the frame ourselves (ComposeWinlatorXRFrame) and the menu is a flat screen that WinlatorXR's
	// own controller-as-mouse emulation operates on, so there is no overlay/laser handling to do.
	if (!m_usingWinlatorXR && !gVRRenderer->ShouldRenderStereo())
		vr::VROverlay()->HideOverlay(m_3DOverlay);

	if ((m_pGame->IsInMenu() || m_pGame->GetSystem()->GetIConsole()->IsOpened()) && UseMotionControllers())
	{
		if (!m_wasInMenu)
		{
			CryLogAlways("Entering menu...");
			m_wasInMenu = true;
			m_buttonPressed = false;
			if (!m_usingWinlatorXR)
			{
				vr::VROverlay()->SetOverlayInputMethod(m_hudOverlay, vr::VROverlayInputMethod_Mouse);
				vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
				vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_HideLaserIntersection, true);
			}
			RecalibrateView();

			m_vrHaptics.StopAllEffects();
		}
		if (!m_usingWinlatorXR)
		{
			SetHudInFrontOfPlayer();
			ProcessMenuInput();
		}
		return;
	}

	if (m_wasInMenu)
	{
		m_wasInMenu = false;
		m_buttonPressed = false;
		if (!m_usingWinlatorXR)
		{
			vr::VROverlay()->SetOverlayInputMethod(m_hudOverlay, vr::VROverlayInputMethod_None);
			vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
		}
		RecalibrateView();
	}

	if (!UseMotionControllers())
		return;

	if (!m_usingWinlatorXR)
	{
		CPlayer* player = m_pGame->GetLocalPlayer();
		if (player && player->IsWeaponZoomActive())
			SetHudAsWeaponZoom();
		else if (m_pGame->AreBinocularsActive())
			SetHudAsBinoculars();
		else if (m_pGame->IsCutSceneActive() && gVR->vr_cutscenes_cinema_mode > 0)
			SetHudInFrontOfPlayer();
		else if (IsDrivingVehicleInCinemaMode())
			SetHudInFrontOfPlayer();
		else
			SetHudAttachedToHead();
	}

	m_input.ProcessInput();
	ProcessRoomscale();
}

void VRManager::ProcessMenuInput()
{
	m_mousePressed = false;
	m_mouseReleased = false;
	m_pGame->RequestStopVideo(false);

	vr::VREvent_t event;
	while (vr::VROverlay()->PollNextOverlayEvent(m_hudOverlay, &event, sizeof(vr::VREvent_t)))
	{
		if (event.eventType == vr::VREvent_MouseMove)
		{
			IMouse* mouse = m_pGame->GetSystem()->GetIInput()->GetIMouse();
			mouse->SetVScreenX(800.f * event.data.mouse.x / m_pGame->m_pRenderer->GetWidth());
			mouse->SetVScreenY(600.f * (1.f - event.data.mouse.y / m_pGame->m_pRenderer->GetHeight()));
		}
		if (event.eventType == vr::VREvent_MouseButtonDown)
		{
			if (event.data.mouse.button == vr::VRMouseButton_Left)
				m_mousePressed = true;
			m_buttonPressed = true;
			m_lastTimeButtonPressed = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime();
		}
		if (event.eventType == vr::VREvent_MouseButtonUp)
		{
			if (event.data.mouse.button == vr::VRMouseButton_Left)
				m_mouseReleased = true;
			m_buttonPressed = false;
		}
		if (event.eventType == vr::VREvent_ButtonPress)
		{
			m_buttonPressed = true;
			m_lastTimeButtonPressed = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime();
		}
		if (event.eventType == vr::VREvent_ButtonUnpress)
		{
			m_buttonPressed = false;
		}
	}

	if (m_buttonPressed && m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime() - m_lastTimeButtonPressed >= 0.5f)
	{
		m_pGame->RequestStopVideo(true);
		m_buttonPressed = false;
	}
}

bool VRManager::UseMotionControllers() const
{
	return (m_inputReady && vr_enable_motion_controllers);
}

Matrix34 VRManager::GetControllerTransform(int hand)
{
	if (!m_inputReady)
	{
		// called from gameplay code (weapon positioning etc.) independent of our own state; without a
		// working input backend there is no controller pose to give
		return Matrix34::CreateIdentity();
	}

	Ang3 refAngles(0, 0, m_referenceYaw);
	Matrix33 refTransform;
	refTransform.SetRotationXYZ(refAngles);
	refTransform.Transpose();
	Matrix34 rawControllerTransform = m_input.GetControllerTransform(hand);
	rawControllerTransform.SetTranslation(rawControllerTransform.GetTranslation() - m_referencePosition);
	return refTransform * rawControllerTransform;
}

void VRManager::UpdatePlayerTurnOffset(float yawDeltaDeg)
{
	m_uncommittedReferenceYaw += DEG2RAD(yawDeltaDeg);
	//UpdateHmdTransform();
}

void VRManager::UpdatePlayerMoveOffset(const Vec3& offset, const Ang3& hmdAnglesDeg)
{
	// transform offset back into raw HMD space
	Ang3 refAngles(0, 0, m_uncommittedReferenceYaw - DEG2RAD(hmdAnglesDeg.z));
	Matrix33 refTransform = Matrix33::CreateRotationXYZ(refAngles);

	Vec3 rawOffset = refTransform * offset;
	rawOffset.z = 0;
	m_uncommittedReferencePosition += rawOffset;
	UpdateHmdTransform();
}

void VRManager::OnPostPlayerCameraUpdate() 
{
	CommitYawAndOffsetChanges();
}

void VRManager::CommitYawAndOffsetChanges() 
{
	m_referenceYaw = m_uncommittedReferenceYaw;
	m_referencePosition = m_uncommittedReferencePosition;
	m_referencePosition.z = 0;
	UpdateHmdTransform();
}

bool VRManager::IsDrivingVehicleInCinemaMode()
{
	if (CPlayer* player = m_pGame->GetLocalPlayer())
	{
		return player->GetVehicle() && player->GetVehicle()->GetUserInState(CPlayer::PVS_DRIVER) == player && vr_vehicles_cinema_mode != 0;
	}

	return false;
}

void VRManager::ProcessRoomscale()
{
	CPlayer* player = m_pGame->GetLocalPlayer();
	if (!player || m_pGame->IsCutSceneActive() || m_pGame->IsInMenu())
	{
		m_skippedRoomscaleMovement = true;
		return;
	}

	if (m_skippedRoomscaleMovement)
	{
		// if we previously skipped roomscale movement, reset our offsets to not accidentally move way too much
		RecalibrateView();
		m_skippedRoomscaleMovement = false;
	}

	m_referenceHeight = max(m_referenceHeight, m_hmdTransform.GetTranslation().z);

	if (m_pGame->GetClient())
	{
		m_pGame->GetClient()->EnableMotionControls(m_pGame->g_LeftHanded->GetIVal() == 0);
		Vec3 hmdPos = m_hmdTransform.GetTranslation();
		Ang3 hmdAngles = ToAnglesDeg(m_hmdTransform);
		m_pGame->GetClient()->UpdateHmdTransform(hmdPos, hmdAngles, GetEffectiveReferenceHeight());

		for (int i = 0; i < 2; ++i)
		{
			Matrix34 controllerTransform = GetControllerTransform(i);
			Vec3 controllerPos = controllerTransform.GetTranslation();
			Ang3 controllerAngles = ToAnglesDeg(controllerTransform);
			m_pGame->GetClient()->UpdateControllerTransform(i, controllerPos, controllerAngles);
		}
	}
}

void VRManager::RecalibrateView()
{
	if (!m_headPose.bPoseIsValid)
		return;

	CryLogAlways("Recalibrating view");
	Matrix34 rawHmdTransform = OpenVRToFarCry(m_headPose.mDeviceToAbsoluteTracking);
	Ang3 rawAngles;
	rawAngles.SetAnglesXYZ((Matrix33)rawHmdTransform);
	m_referencePosition = rawHmdTransform.GetTranslation();
	m_referenceHeight = m_referencePosition.z;
	m_referencePosition.z = 0;
	m_referenceYaw = rawAngles.z;
	m_referenceCalibrated = true;
	UpdateHmdTransform();

	// recalibrate menu HUD positioning
	m_fixedHudTransform = OpenVRToFarCry(m_headPose.mDeviceToAbsoluteTracking);
	// erase pitch and roll
	Ang3 angles;
	angles.SetAnglesXYZ((Matrix33)m_fixedHudTransform);
	angles.x = angles.y = 0;
	m_fixedHudTransform.SetRotationXYZ(angles, m_fixedHudTransform.GetTranslation());
	Vec3 dir = -((Matrix33)m_fixedHudTransform).GetColumn(1);
	Vec3 pos = m_fixedHudTransform.GetTranslation() + vr_menu_distance * dir;
	m_fixedHudTransform.SetTranslation(pos);
}


void VRManager::SetHudAttachedToHead()
{
	m_fixedPositionInitialized = false;
	vr::HmdMatrix34_t hudTransform;
	memset(&hudTransform, 0, sizeof(vr::HmdMatrix34_t));
	hudTransform.m[0][0] = hudTransform.m[1][1] = hudTransform.m[2][2] = 1;
	hudTransform.m[2][3] = -vr_hud_distance;
	vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_IgnoreTextureAlpha, false);
	vr::VROverlay()->SetOverlayWidthInMeters(m_hudOverlay, vr_hud_width);
	vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_hudOverlay, vr::k_unTrackedDeviceIndex_Hmd, &hudTransform);
}

void VRManager::SetHudInFrontOfPlayer()
{
	if (!m_fixedPositionInitialized)
	{
		RecalibrateView();
		m_fixedPositionInitialized = true;
	}

	vr::HmdMatrix34_t hudTransform = FarCryToOpenVR(m_fixedHudTransform);
	vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_IgnoreTextureAlpha, false);
	vr::VROverlay()->SetOverlayWidthInMeters(m_hudOverlay, vr_menu_width);
	vr::VROverlay()->SetOverlayTransformAbsolute(m_hudOverlay, vr::TrackingUniverseStanding, &hudTransform);
	if (gVRRenderer->ShouldRenderStereo())
	{
		vr::VROverlay()->ShowOverlay(m_3DOverlay);
		vr::VROverlay()->SetOverlayWidthInMeters(m_3DOverlay, vr_menu_width);
		vr::VROverlay()->SetOverlayTransformAbsolute(m_3DOverlay, vr::TrackingUniverseStanding, &hudTransform);
	}
}

void VRManager::SetHudAsBinoculars()
{
	m_fixedPositionInitialized = false;
	bool leftHanded = m_pGame->g_LeftHanded->GetIVal() == 1;
	Matrix34 transform = m_input.GetControllerTransform(leftHanded ? 1 : 0);
	transform = transform * Matrix34::CreateTranslationMat(Vec3((leftHanded ? 1 : -1) * vr_binocular_size / 2, 0, vr_binocular_size / 2));
	vr::HmdMatrix34_t hudTransform = FarCryToOpenVR(transform);
	vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
	vr::VROverlay()->SetOverlayWidthInMeters(m_hudOverlay, vr_binocular_size);
	vr::VROverlay()->SetOverlayTransformAbsolute(m_hudOverlay, vr::TrackingUniverseStanding, &hudTransform);
}

void VRManager::SetHudAsWeaponZoom()
{
	m_fixedPositionInitialized = false;
	Matrix34 transform = m_input.GetControllerTransform(m_pGame->g_LeftHanded->GetIVal() == 1 ? 1 : 0);
	Matrix34 rawHmdTransform = OpenVRToFarCry(m_headPose.mDeviceToAbsoluteTracking);
	Vec3 headPos = rawHmdTransform.GetTranslation() - Vec3(0, 0, vr_scope_size / 2);
	Vec3 fwd = transform.GetTranslation() - headPos;
	Vec3 up(0, 0, 1);
	Vec3 left = -fwd.Cross(up).GetNormalized();
	up = left.Cross(-fwd).GetNormalized();
	transform.SetMatFromVectors(left, -fwd, up, transform.GetTranslation());
	Ang3 angles = ToAnglesDeg(transform);
	angles.y = 0;
	transform.SetRotationXYZ(Deg2Rad(angles), transform.GetTranslation());
	transform = transform * Matrix34::CreateTranslationMat(Vec3(0, 0, vr_scope_size / 2));
	vr::HmdMatrix34_t hudTransform = FarCryToOpenVR(transform);
	vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
	vr::VROverlay()->SetOverlayWidthInMeters(m_hudOverlay, vr_scope_size);
	vr::VROverlay()->SetOverlayTransformAbsolute(m_hudOverlay, vr::TrackingUniverseStanding, &hudTransform);
}

void VRManager::InitDevice(IDirect3DDevice9Ex* device)
{
	m_d3d->hudTexture.Reset();
	m_d3d->eyeTextures[0].Reset();
	m_d3d->eyeTextures[1].Reset();

	CryLogAlways("Acquiring device...");
	m_d3d->device = device;

	if (m_usingWinlatorXR && device)
		InstallWinlatorXRWindowHook(device);
}

void VRManager::CreateEyeTexture(int eye)
{
	if (!m_d3d->device)
		return;

	vector2di size = GetRenderSize();
	CryLogAlways("Creating eye texture %i: %i x %i", eye, size.x, size.y);
	HRESULT hr = m_d3d->device->CreateTexture(size.x, size.y, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, m_d3d->eyeTextures[eye].ReleaseAndGetAddressOf(), nullptr);
	CryLogAlways("CreateTexture2D return code: %i", hr);
}

void VRManager::CreateHUDTexture()
{
	if (!m_d3d->device)
		return;

	vector2di size = GetRenderSize();
	CryLogAlways("Creating HUD texture: %i x %i", size.x, size.y);
	HRESULT hr = m_d3d->device->CreateTexture(size.x, size.y, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, m_d3d->hudTexture.ReleaseAndGetAddressOf(), nullptr);
	CryLogAlways("CreateRenderTarget return code: %i", hr);
}

void VRManager::CreateStereoTexture()
{
	if (!m_d3d->device)
		return;

	vector2di size = GetRenderSize();
	size.x *= 2;
	CryLogAlways("Creating stereo texture: %i x %i", size.x, size.y);
	HRESULT hr = m_d3d->device->CreateTexture(size.x, size.y, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, m_d3d->stereoTexture.ReleaseAndGetAddressOf(), nullptr);
	CryLogAlways("CreateRenderTarget return code: %i", hr);
}

void VRManager::PrepareTextureForSubmission(IDirect3DTexture9* tex, vr::VRVulkanTextureData_t& vkTexData, VkImageLayout& origLayout)
{
	if (!tex)
		return;

	HRESULT hr = dxvkFillVulkanTextureInfo(m_d3d->device.Get(), tex, vkTexData, origLayout);
	if (hr != S_OK)
	{
		CryLogAlways("Fetching vulkan image info failed: %i", hr);
	}
	dxvkTransitionImageLayout(m_d3d->device.Get(), tex, origLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

void VRManager::PostSubmissionTransitionTexture(IDirect3DTexture9* tex, VkImageLayout origLayout)
{
	if (!tex)
		return;

	dxvkTransitionImageLayout(m_d3d->device.Get(), tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, origLayout);
}

void VRManager::RegisterCVars()
{
	IConsole* console = m_pGame->GetSystem()->GetIConsole();
	console->Register("vr_yaw_deadzone_angle", &vr_yaw_deadzone_angle, 30, VF_DUMPTODISK, "Controls the deadzone angle in front of the player where weapon aim does not rotate the camera");
	console->Register("vr_enable_motion_controllers", &vr_enable_motion_controllers, 1, VF_DUMPTODISK, "Enable this to use VR motion controllers instead of keyboard+mouse");
	console->Register("vr_render_force_max_terrain_detail", &vr_render_force_max_terrain_detail, 1, VF_DUMPTODISK, "If enabled, will force terrain to render at max detail even in the distance");
	console->Register("vr_render_force_obj_draw_dist", &vr_render_force_obj_draw_dist, 0, VF_DUMPTODISK, "If enabled, will force objects and enemies to be drawn at much further distances (might result in rendering issues in some instances)");
	console->Register("vr_window_width", &vr_window_width, 1920, VF_DUMPTODISK, "Configures the Far Cry desktop window width");
	console->Register("vr_window_height", &vr_window_height, 1080, VF_DUMPTODISK, "Configures the Far Cry desktop window height");
	console->Register("vr_winlatorxr_render_height", &vr_winlatorxr_render_height, 1200, VF_DUMPTODISK, "Per-eye render target height when running under WinlatorXR (Quest/Pico); width follows the headset FOV aspect. The game is CPU-bound on these devices, so a tall render (it is squeezed 2:1 into the side-by-side frame) is affordable and much sharper");
	// NOTE: anamorphic rendering deadlocked the game on the Quest 3 in testing (first frame never presents), so it is off by default until that is understood
	console->Register("vr_winlatorxr_anamorphic", &vr_winlatorxr_anamorphic, 0, VF_DUMPTODISK, "Under WinlatorXR, render each eye at double horizontal resolution so the side-by-side frame keeps full per-eye detail (experimental, costs GPU time; 0 = off)");
	console->Register("vr_winlatorxr_max_fps", &vr_winlatorxr_max_fps, 0, VF_DUMPTODISK, "Under WinlatorXR, frame-rate cap applied via dxvk's limiter (0 = uncapped; WinlatorXR's own 72 fps cap quantises the game to 36/18 fps)");
	console->Register("vr_winlatorxr_aer", &vr_winlatorxr_aer, 0, VF_DUMPTODISK, "Under WinlatorXR, use alternate-eye rendering: one full-resolution eye per frame instead of side-by-side (sharper and cheaper per frame, but each eye updates at half rate)");
	console->Register("vr_winlatorxr_block_desktop_input", &vr_winlatorxr_block_desktop_input, 1, VF_DUMPTODISK, "Under WinlatorXR, ignore the mouse/keyboard that WinlatorXR emulates from the controllers while motion controls are active (they would double-trigger actions)");
	console->Register("vr_mirrored_eye", &vr_mirrored_eye, 1, VF_DUMPTODISK, "Which eye view is mirrored to the desktop window. 0 - left, 1 - right");
	console->Register("vr_melee_swing_threshold", &vr_melee_swing_threshold, 2.f, VF_CHEAT, "Configures speed threshold for physical swings to register as melee attacks");
	console->Register("vr_debug_draw_grip", &vr_debug_draw_grip, 0, 0, "If enabled, highlights the position of the current weapon's grip positions");
	console->Register("vr_debug_override_grip", &vr_debug_override_grip, 0, VF_CHEAT, "If enabled, overrides the weapon grip transform offsets");
	console->Register("vr_snap_turn_amount", &vr_snap_turn_amount, 0, VF_DUMPTODISK, "The amount of degrees to snap turn (set to 0 to disable snap turn)");
	console->Register("vr_smooth_turn_speed", &vr_smooth_turn_speed, 1.0f, VF_DUMPTODISK, "Determines speed of smooth turn.");
	console->Register("vr_button_long_press_time", &vr_button_long_press_time, 0.35f, VF_DUMPTODISK, "How long you need to hold a button down to register as a long press");
	console->Register("vr_haptics_effect_strength", &vr_haptics_effect_strength, 1.0f, VF_DUMPTODISK, "Modify the strength of controller haptic events. Set to 0 to disable haptics");
	console->Register("vr_weapon_pitch_offset", &vr_weapon_pitch_offset, 15.0f, VF_DUMPTODISK, "Modify the weapon grip vertical angle.");
	console->Register("vr_weapon_yaw_offset", &vr_weapon_yaw_offset, 0.0f, VF_DUMPTODISK, "Modify the weapon grip horizontal angle.");
	console->Register("vr_crosshair", &vr_crosshair, 1, VF_DUMPTODISK, "VR crosshair type. 0 - none, 1 - ball, 2 - laser");
	console->Register("vr_movement_dir", &vr_movement_dir, -1, VF_DUMPTODISK, "Movement direction reference: -1 = head, 0 = left hand, 1 = right hand");
	console->Register("vr_show_empty_hands", &vr_show_empty_hands, 1, VF_DUMPTODISK, "If enabled, draws empty player hands when appropriate");
	console->Register("vr_immersive_ladders", &vr_immersive_ladders, 1, VF_DUMPTODISK, "Climb ladders by grabbing with your hands");
	console->Register("vr_render_world_while_zoomed", &vr_render_world_while_zoomed, 1, VF_DUMPTODISK, "Keep rendering the world in VR while binoculars or weapon scopes are active - costs performance!");
	console->Register("vr_binocular_size", &vr_binocular_size, 0.4f, VF_DUMPTODISK, "Width of the binocular overlay (in meters)");
	console->Register("vr_scope_size", &vr_scope_size, 0.3f, VF_DUMPTODISK, "Width of the weapon scope overlay (in meters)");
	console->Register("vr_seated_mode", &vr_seated_mode, 0, VF_DUMPTODISK, "If enabled, will fix VR camera at player head height and disable physical crouching");
	console->Register("vr_height_offset", &vr_height_offset, 0.0f, VF_DUMPTODISK, "Raises (positive) or lowers (negative) the in-game eye height by this many metres");
	console->Register("vr_cutscenes_cinema_mode", &vr_cutscenes_cinema_mode, 0, VF_DUMPTODISK, "Determines how cutscenes are played. 0 - full VR, 1 - 2D cinema, 2 - 3D cinema");
	console->Register("vr_vehicles_cinema_mode", &vr_vehicles_cinema_mode, 0, VF_DUMPTODISK, "Determines how vehicles are played. 0 - full VR, 1 - 2D cinema, 2 - 3D cinema");
	console->Register("vr_hud_distance", &vr_hud_distance, 2.5f, VF_DUMPTODISK, "Determines how far away from the player the ingame HUD is placed");
	console->Register("vr_hud_width", &vr_hud_width, 2, VF_DUMPTODISK, "Determines how large the ingame HUD is");
	console->Register("vr_menu_distance", &vr_menu_distance, 4, VF_DUMPTODISK, "Determines how far away from the player the menu and theater mode is placed");
	console->Register("vr_menu_width", &vr_menu_width, 4, VF_DUMPTODISK, "Determines how large the menu and theater mode is");
	console->Register("vr_skip_vehicle_transitions", &vr_skip_vehicle_transitions, 1, VF_DUMPTODISK, "If enabled, skip camera transitions when entering/exiting vehicles");
	console->Register("vr_decouple_vehicle_rotations", &vr_decouple_vehicle_rotations, 0, VF_DUMPTODISK, "If enabled, vehicle rotations do not automatically transfer to the player camera");
	vr_debug_override_rh_offset = console->CreateVariable("vr_debug_override_rh_offset", "0.0 -0.1 -0.018", VF_CHEAT);
	vr_debug_override_lh_offset = console->CreateVariable("vr_debug_override_lh_offset", "0.0 -0.1 -0.018", VF_CHEAT);
	vr_debug_override_rh_angles = console->CreateVariable("vr_debug_override_rh_angles", "0.0 0.0 0.0", VF_CHEAT);

	e_terrain_lod_ratio = console->GetCVar("e_terrain_lod_ratio");
	e_detail_texture_min_fov = console->GetCVar("e_detail_texture_min_fov");
	e_obj_view_dist_ratio = console->GetCVar("e_obj_view_dist_ratio");

	// disable motion blur, as it does not work properly in VR
	console->GetCVar("r_MotionBlur")->ForceSet("0");

	console->Update();
}

void VRManager::UpdateHmdTransform()
{
	if (!m_headPose.bPoseIsValid)
		return;

	Ang3 refAngles(0, 0, m_referenceYaw);
	Matrix33 refTransform;
	refTransform.SetRotationXYZ(refAngles);
	refTransform.Transpose();

	Matrix34 rawHmdTransform = OpenVRToFarCry(m_headPose.mDeviceToAbsoluteTracking);
	rawHmdTransform.SetTranslation(rawHmdTransform.GetTranslation() - m_referencePosition);
	m_hmdTransform = refTransform * rawHmdTransform;
}

void VRManager::UpdateWinlatorXRPose()
{
	WinlatorXR::InputState state = WinlatorXR::GetLatestState();
	if (!state.valid)
	{
		// keep whatever we had (initially: invalid) until the first packet arrives
		return;
	}

	// WinlatorXR forwards the raw OpenXR view pose: the quaternion/position use the same convention as
	// OpenVR (x = right, y = up, -z = forward, metres, floor-level STAGE space), so it can be dropped
	// straight into the OpenVR pose struct and go through OpenVRToFarCry like a SteamVR pose would.
	float qx = state.hmdQx, qy = state.hmdQy, qz = state.hmdQz, qw = state.hmdQw;
	float len = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
	if (len < 1e-4f)
	{
		// degenerate (all-zero) quaternion, e.g. before the runtime has produced its first real pose
		return;
	}
	qx /= len; qy /= len; qz /= len; qw /= len;

	// Lift LOCAL-space poses to floor level: protocol 0.5 reports the head's height above the floor
	// (STAGE space), so offset = altitude - local y. Kept as a running value (it only changes if
	// WinlatorXR recentres) and shared with the controller poses via GetWinlatorFloorOffset().
	if (state.hmdAltitude > 0.3f && state.hmdAltitude < 3.f)
	{
		float offset = state.hmdAltitude - state.hmdY;
		if (!m_winlatorFloorOffsetValid)
			CryLogAlways("[WinlatorXR] floor offset from HMD altitude: %.3f m (head %.3f m above floor)", offset, state.hmdAltitude);
		m_winlatorFloorOffset = offset;
		m_winlatorFloorOffsetValid = true;
	}
	float floorY = state.hmdY + GetWinlatorFloorOffset();

	m_headPose.mDeviceToAbsoluteTracking = HmdMatrixFromQuatPos(qx, qy, qz, qw, state.hmdX, floorY, state.hmdZ);
	m_headPose.bPoseIsValid = true;
	m_headPose.bDeviceIsConnected = true;
	m_headPose.eTrackingResult = vr::TrackingResult_Running_OK;
	// remember which of WinlatorXR's pose slots this frame is rendered with (0..252 in steps of 12)
	m_winlatorFrameSync = clamp_tpl(state.frameId, 0, 255);

	// eye separation: WinlatorXR reports the distance between the two OpenXR eye views in metres.
	// Be lenient about units in case a future protocol version switches to millimetres.
	float ipd = state.ipd;
	if (ipd > 1.f)
		ipd *= 0.001f;
	if (ipd >= 0.045f && ipd <= 0.085f)
		m_winlatorEyeSeparation = ipd;

	// symmetric FOV as reported by the headset runtime (degrees); ignore obviously bogus values
	if (state.fovH >= 40.f && state.fovH <= 150.f && state.fovV >= 40.f && state.fovV <= 150.f)
	{
		float horz = tanf(DEG2RAD(state.fovH) / 2.f);
		float vert = tanf(DEG2RAD(state.fovV) / 2.f);
		if (fabsf(horz - m_horizontalFov) > 1e-3f || fabsf(vert - m_verticalFov) > 1e-3f)
		{
			m_horizontalFov = horz;
			m_verticalFov = vert;
			CryLogAlways("[WinlatorXR] FOV updated: horz %.1f deg  vert %.1f deg", state.fovH, state.fovV);
		}
	}

	if (!m_winlatorPoseLogged)
	{
		m_winlatorPoseLogged = true;
		CryLogAlways("[WinlatorXR] Received first head pose: pos=(%.3f, %.3f, %.3f) ipd=%.4f m fov=%.1fx%.1f", state.hmdX, state.hmdY, state.hmdZ, m_winlatorEyeSeparation, state.fovH, state.fovV);
	}
}

Matrix34 VRManager::GetEyeToHeadTransform(int eye)
{
	if (m_usingWinlatorXR)
	{
		// WinlatorXR only gives us the centre eye pose plus the IPD, so the eye offset is a plain
		// horizontal shift: left eye towards -x, right eye towards +x (OpenVR/OpenXR convention)
		float shift = (eye == 0 ? -0.5f : 0.5f) * m_winlatorEyeSeparation;
		return OpenVRToFarCry(HmdMatrixFromQuatPos(0.f, 0.f, 0.f, 1.f, shift, 0.f, 0.f));
	}

	return OpenVRToFarCry(vr::VRSystem()->GetEyeToHeadTransform(eye == 0 ? vr::Eye_Left : vr::Eye_Right));
}

void VRManager::DrawTexturedQuad(IDirect3DTexture9* texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, bool alphaBlend)
{
	struct Vertex
	{
		float x, y, z, w;
		float u, v;
	};
	// -0.5 offsets: D3D9 pixel centre convention for pre-transformed vertices
	Vertex vertices[4] =
	{
		{ x0 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, u0, v0 },
		{ x1 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, u1, v0 },
		{ x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, u1, v1 },
		{ x0 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, u0, v1 },
	};

	IDirect3DDevice9Ex* dev = m_d3d->device.Get();
	if (alphaBlend)
	{
		// classic "over" blend for the colour channels, but keep the destination alpha at 1: WinlatorXR
		// composites our frame with source-alpha blending, so any alpha < 1 would let passthrough shine through
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
		dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	}
	else
	{
		// opaque copy; force alpha to 1 regardless of what the engine left in the eye texture's alpha channel
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
	}
	dev->SetTexture(0, texture);
	dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, vertices, sizeof(Vertex));
}

void VRManager::DrawHud(int eye, int x0Region, int halfWidth, int height)
{
	if (!m_d3d->hudTexture)
		return;

	D3DSURFACE_DESC desc;
	m_d3d->hudTexture->GetLevelDesc(0, &desc);
	if (desc.Width == 0 || desc.Height == 0 || m_horizontalFov <= 0.f || m_verticalFov <= 0.f)
		return;

	// Mimic the SteamVR path, where the HUD is a head-locked overlay of vr_hud_width metres at
	// vr_hud_distance metres (keeping the texture's aspect ratio). Everything below is done in
	// tangent space: the eye image spans [-m_horizontalFov, +m_horizontalFov] horizontally and
	// [+m_verticalFov, -m_verticalFov] vertically (both are tan(fov/2)).
	float distance = max(vr_hud_distance, 0.1f);
	float halfWidthTan = 0.5f * vr_hud_width / distance;
	// the HUD texture is captured at the (possibly anamorphic) render size; its logical aspect is
	// what the OpenVR overlay would show
	float logicalTexWidth = desc.Width / (float)WinlatorRenderScaleX();
	float halfHeightTan = halfWidthTan * desc.Height / logicalTexWidth;
	// stereo convergence: an object straight ahead at 'distance' is seen shifted towards the nose in
	// each eye, i.e. to the right in the left eye's image and to the left in the right eye's image
	float shiftTan = (eye == 0 ? 1.f : -1.f) * 0.5f * m_winlatorEyeSeparation / distance;

	float x0 = x0Region + halfWidth * (0.5f + (shiftTan - halfWidthTan) / (2.f * m_horizontalFov));
	float x1 = x0Region + halfWidth * (0.5f + (shiftTan + halfWidthTan) / (2.f * m_horizontalFov));
	float y0 = height * (0.5f - halfHeightTan / (2.f * m_verticalFov));
	float y1 = height * (0.5f + halfHeightTan / (2.f * m_verticalFov));

	// never bleed outside this eye's region (matters for side-by-side)
	RECT scissor = { x0Region, 0, x0Region + halfWidth, height };
	m_d3d->device->SetScissorRect(&scissor);
	m_d3d->device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
	DrawTexturedQuad(m_d3d->hudTexture.Get(), x0, y0, x1, y1, 0.f, 0.f, 1.f, 1.f, true);
	m_d3d->device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
}

void VRManager::ComposeWinlatorXRFrame()
{
	if (!m_d3d->device)
		return;

	// Decide what this frame is. The back buffer currently holds whatever the engine rendered last:
	// in VR mode that is the HUD over a transparent clear (the eyes live in m_d3d->eyeTextures),
	// in the 2D modes (binoculars, scopes, cinema) it is the flat game image plus HUD.
	bool inMenu = m_pGame->IsInMenu();
	bool haveEyes = m_d3d->eyeTextures[0].Get() != nullptr && m_d3d->eyeTextures[1].Get() != nullptr;
	bool vrWorld = !inMenu && haveEyes && gVRRenderer->ShouldRenderVR() && !gVRRenderer->ShouldRender2D();
	bool stereoPlane = !inMenu && !vrWorld && gVRRenderer->ShouldRenderStereo() && m_d3d->stereoTexture.Get() != nullptr;

	if (!vrWorld && !stereoPlane)
	{
		// flat frame (menu, binoculars, weapon scope, 2D cinema): let WinlatorXR show the back buffer
		// as-is on its virtual screen
		m_winlatorModeVr = 2;
		m_winlatorMode3d = 0;
		return;
	}

	ComPtr<IDirect3DSurface9> backBuffer;
	m_d3d->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer.GetAddressOf());
	if (!backBuffer)
		return;
	D3DSURFACE_DESC bbDesc;
	backBuffer->GetDesc(&bbDesc);
	int width = bbDesc.Width;
	int height = bbDesc.Height;
	int halfWidth = width / 2;

	m_pGame->m_pRenderer->ResetToDefault();

	IDirect3DStateBlock9* stateBlock = nullptr;
	m_d3d->device->CreateStateBlock(D3DSBT_ALL, &stateBlock);

	IDirect3DDevice9Ex* dev = m_d3d->device.Get();
	D3DVIEWPORT9 viewport = { 0, 0, (DWORD)width, (DWORD)height, 0.f, 1.f };
	dev->SetViewport(&viewport);
	dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
	dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	dev->SetRenderState(D3DRS_VERTEXBLEND, FALSE);
	dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
	dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	dev->SetVertexShader(nullptr);
	dev->SetPixelShader(nullptr);

	if (vrWorld && UseWinlatorAER())
	{
		// Alternate-eye: the whole frame is the eye rendered this frame, at full resolution. The
		// frame-sync marker's blue channel tells WinlatorXR which of its two eye framebuffers to
		// update (B > 0 = right), R selects the pose the frame was rendered with.
		int eye = m_winlatorAerEye;
		DrawTexturedQuad(m_d3d->eyeTextures[eye].Get(), 0.f, 0.f, (float)width, (float)height, 0.f, 0.f, 1.f, 1.f, false);
		DrawHud(eye, 0, width, height);

		D3DRECT syncRect = { 0, 0, 8, 8 };
		dev->Clear(1, &syncRect, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, m_winlatorFrameSync, 0, eye == 1 ? 255 : 0), 1.f, 0);

		m_winlatorModeVr = 1;
		m_winlatorMode3d = 2;
	}
	else if (vrWorld)
	{
		// Side-by-side: left eye in the left half, right eye in the right half. WinlatorXR stretches
		// each half over the FOV we render with, so squeezing the square eye images into the halves
		// is exactly undone on the headset.
		for (int eye = 0; eye < 2; ++eye)
		{
			DrawTexturedQuad(m_d3d->eyeTextures[eye].Get(), (float)(eye * halfWidth), 0.f, (float)((eye + 1) * halfWidth), (float)height, 0.f, 0.f, 1.f, 1.f, false);
		}
		for (int eye = 0; eye < 2; ++eye)
		{
			DrawHud(eye, eye * halfWidth, halfWidth, height);
		}

		// Frame-sync marker: WinlatorXR samples screen pixel (0,0) and, if G == 0 and A > 0, uses R as
		// the index of the head pose our frame was rendered with. A small block (rather than a single
		// pixel) survives the scaling from back buffer to X screen.
		D3DRECT syncRect = { 0, 0, 8, 8 };
		dev->Clear(1, &syncRect, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, m_winlatorFrameSync, 0, 0), 1.f, 0);

		m_winlatorModeVr = 1;
		m_winlatorMode3d = 1;
	}
	else
	{
		// 3D cinema plane (cutscenes/vehicles): the stereo texture is already side-by-side, show it on
		// WinlatorXR's virtual screen in SBS mode with the HUD on top of each half
		DrawTexturedQuad(m_d3d->stereoTexture.Get(), 0.f, 0.f, (float)width, (float)height, 0.f, 0.f, 1.f, 1.f, false);
		for (int eye = 0; eye < 2; ++eye)
		{
			DrawHud(eye, eye * halfWidth, halfWidth, height);
		}
		m_winlatorModeVr = 2;
		m_winlatorMode3d = 1;
	}

	if (stateBlock)
	{
		stateBlock->Apply();
		stateBlock->Release();
	}
}

#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

static WNDPROC s_winlatorOrigWndProc = nullptr;
static HWND s_winlatorHookedWnd = nullptr;

static LRESULT CALLBACK WinlatorXRWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// FarCry.exe's own window procedure dereferences an invalid mouse object for WM_MOUSEWHEEL under
	// Wine (crash observed at FarCry.exe+0x1376), and WinlatorXR generates wheel events from the
	// primary thumbstick. The wheel has no use in VR anyway, so swallow it.
	if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
		return 0;

	return CallWindowProcA(s_winlatorOrigWndProc, hWnd, msg, wParam, lParam);
}

void VRManager::InstallWinlatorXRWindowHook(IDirect3DDevice9Ex* device)
{
	D3DDEVICE_CREATION_PARAMETERS params;
	memset(&params, 0, sizeof(params));
	HWND hWnd = nullptr;
	if (SUCCEEDED(device->GetCreationParameters(&params)))
		hWnd = params.hFocusWindow;
	if (!hWnd)
		hWnd = GetActiveWindow();
	if (!hWnd || hWnd == s_winlatorHookedWnd)
		return;

	WNDPROC prev = (WNDPROC)SetWindowLongPtrA(hWnd, GWLP_WNDPROC, (LONG_PTR)WinlatorXRWndProc);
	if (!prev)
	{
		CryLogAlways("[WinlatorXR] failed to subclass game window 0x%p (error %u)", hWnd, GetLastError());
		return;
	}
	s_winlatorOrigWndProc = prev;
	s_winlatorHookedWnd = hWnd;
	CryLogAlways("[WinlatorXR] subclassed game window 0x%p - mouse wheel messages are dropped", hWnd);
}

void VRManager::UpdateDesktopInputBlock()
{
	if (!m_usingWinlatorXR)
		return;
	IActionMapManager* actionMaps = m_pGame->GetActionMapManager();
	if (!actionMaps)
		return;

	bool inMenu = m_pGame->IsInMenu() || m_pGame->GetSystem()->GetIConsole()->IsOpened();
	bool shouldBlock = vr_winlatorxr_block_desktop_input != 0 && UseMotionControllers() && !inMenu;

	if (shouldBlock)
	{
		// re-applied every frame: the game re-enables the action maps itself whenever an exclusive
		// UI overlay (dialogs, menu) closes
		if (actionMaps->IsEnabled())
			actionMaps->Disable();
	}
	else if (m_desktopInputBlocked && !actionMaps->IsEnabled() && !m_pGame->m_bUIExclusiveInput)
	{
		// give the keyboard/mouse back, but only if it was us who took it away
		actionMaps->Enable();
	}
	m_desktopInputBlocked = shouldBlock;
}
