#include "StdAfx.h"
#include "VRRenderer.h"

#include "Cry_Camera.h"
#include "Game.h"
#include "Hooks.h"
#include "VRManager.h"
#include <d3d9.h>

#include "WeaponClass.h"
#include "WeaponSystemEx.h"
#include "xplayer.h"
#include "XVehicle.h"

namespace
{
	VRRenderer g_vrRendererImpl;
}

VRRenderer* gVRRenderer = &g_vrRendererImpl;

BOOL __stdcall Hook_SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int  X, int  Y, int  cx, int  cy, UINT uFlags)
{
	if (!gVRRenderer->ShouldIgnoreWindowSizeChanges())
	{
		return hooks::CallOriginal(Hook_SetWindowPos)(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
	}

	return TRUE;
}

// Cached raw pointer to the current swap-chain back buffer surface, used by Hook_D3D9SetViewport to tell
// the main back-buffer pass from equally-sized offscreen render targets without a per-call GetBackBuffer
// (a guest->dxvk COM round-trip that is expensive under Box64). dxvk hands back a stable wrapper for the
// back buffer; refreshed once per frame in Present so a resolution change / device reset is picked up on
// the next frame. Only ever compared, never dereferenced or kept referenced.
static IDirect3DSurface9* s_backBufferForClamp = nullptr;

HRESULT __stdcall Hook_D3D9Present(IDirect3DDevice9Ex* pSelf, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion)
{
	{
		IDirect3DSurface9* bb = nullptr;
		if (SUCCEEDED(pSelf->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
		{
			s_backBufferForClamp = bb;
			bb->Release();
		}
	}
	gVRRenderer->OnPrePresent();
	HRESULT result = hooks::CallOriginal(Hook_D3D9Present)(pSelf, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
	gVRRenderer->OnPostPresent();
	return result;
}

// Under WinlatorXR the "proper" per-eye path renders each eye into an eye-shaped offscreen render target
// (so Far Cry's render-target-driven FOV comes out correct), then composites both into the wide back
// buffer. Two hooks make the engine cooperate without engine source:
//  - Hook_D3D9SetRenderTarget redirects the engine's back-buffer binding to the current eye/HUD target.
//  - Hook_D3D9SetViewport clamps the engine's full-screen viewport down to fit that eye-shaped target.
// Both no-op on the PC/OpenVR path and whenever no redirect is active (e.g. the composite, the menu).
HRESULT __stdcall Hook_D3D9SetRenderTarget(IDirect3DDevice9Ex* pSelf, DWORD renderTargetIndex, IDirect3DSurface9* pRenderTarget)
{
	if (renderTargetIndex == 0 && pRenderTarget != nullptr && gVR && gVR->IsUsingWinlatorXR())
	{
		IDirect3DSurface9* redirect = (IDirect3DSurface9*)gVR->GetRenderRedirect();
		if (redirect != nullptr && s_backBufferForClamp != nullptr && pRenderTarget == s_backBufferForClamp)
		{
			return hooks::CallOriginal(Hook_D3D9SetRenderTarget)(pSelf, renderTargetIndex, redirect);
		}
	}
	return hooks::CallOriginal(Hook_D3D9SetRenderTarget)(pSelf, renderTargetIndex, pRenderTarget);
}

HRESULT __stdcall Hook_D3D9SetViewport(IDirect3DDevice9Ex* pSelf, const D3DVIEWPORT9* pViewport)
{
	int cw = 0, ch = 0, fullW = 0, fullH = 0;
	if (pViewport && gVR && gVR->IsUsingWinlatorXR() && gVR->GetMainPassClamp(&cw, &ch, &fullW, &fullH)
		&& cw > 0 && ch > 0 && (int)pViewport->Width > cw)
	{
		// the eye/HUD offscreen target is the current render target during those passes; a viewport wider
		// than it (the engine still thinks it is drawing full-screen) must be clamped down to fit it
		IDirect3DSurface9* redirect = (IDirect3DSurface9*)gVR->GetRenderRedirect();
		IDirect3DSurface9* rt = nullptr;
		bool onEyeTarget = false;
		if (redirect && SUCCEEDED(pSelf->GetRenderTarget(0, &rt)) && rt)
		{
			onEyeTarget = (rt == redirect);
			rt->Release();
		}
		if (onEyeTarget)
		{
			D3DVIEWPORT9 vp = *pViewport;
			vp.X = 0; vp.Y = 0;
			vp.Width = (DWORD)cw;
			vp.Height = (DWORD)ch;
			return hooks::CallOriginal(Hook_D3D9SetViewport)(pSelf, &vp);
		}
	}
	return hooks::CallOriginal(Hook_D3D9SetViewport)(pSelf, pViewport);
}

void __fastcall Hook_Renderer_SetCamera(IRenderer* pSelf, void* notUsed, const CCamera& cam)
{
	CCamera cc = cam;
	const CCamera& vc = pSelf->GetCamera();
	// try to detect if this is the DRAW_NEAR camera, and if so, restore proper FOV as the FOV reduction does not work in VR
	if (cc.GetZMin() == 0.01f && cc.GetZMax() == 40.0f)
	{
		cc.SetFov(vc.GetFov());
	}
	hooks::CallOriginal(Hook_Renderer_SetCamera)(pSelf, notUsed, cc);
}

extern "C" {
  __declspec(dllimport) IDirect3DDevice9Ex* dxvkGetCreatedDevice();
}

void VRRenderer::Init(CXGame *game)
{
	m_pGame = game;

	IDirect3DDevice9Ex* device = dxvkGetCreatedDevice();
	if (!device)
	{
		CryLogAlways("Could not get d3d9 device from dxvk");
		return;
	}

	CryLogAlways("Initializing rendering function hooks");
	hooks::InstallHook("SetWindowPos", &SetWindowPos, &Hook_SetWindowPos);
	hooks::InstallVirtualFunctionHook("IDirect3DDevice9Ex::Present", device, 17, &Hook_D3D9Present);
	// IDirect3DDevice9 vtable: SetRenderTarget is slot 37, SetViewport is slot 47. Together they route the
	// engine's main scene + HUD passes into the eye-shaped offscreen targets under WinlatorXR.
	hooks::InstallVirtualFunctionHook("IDirect3DDevice9Ex::SetRenderTarget", device, 37, &Hook_D3D9SetRenderTarget);
	hooks::InstallVirtualFunctionHook("IDirect3DDevice9Ex::SetViewport", device, 47, &Hook_D3D9SetViewport);
	hooks::InstallVirtualFunctionHook("IRenderer::SetCamera", m_pGame->m_pRenderer, 36, &Hook_Renderer_SetCamera);
}

void VRRenderer::Shutdown()
{
}

void VRRenderer::Render(ISystem* pSystem)
{
	m_originalViewCamera = pSystem->GetViewCamera();

	gVR->SetDevice(dxvkGetCreatedDevice());
	gVR->AwaitFrame();

	if (CPlayer* player = m_pGame->GetLocalPlayer())
	{
		player->UpdateVRTransformsPreRender();
	}

	// Per-eye path: arm the viewport clamp so the SetViewport hook can shrink the engine's full-screen
	// viewport to fit the eye-shaped render targets we bind in RenderSingleEye / the HUD phase.
	// ComposeWinlatorXRFrame disarms it (and the redirect) before compositing.
	gVR->SetMainPassClamp(UsePerEyeRenderTargets());

	if (gVR->UseWinlatorAER())
	{
		// alternate-eye rendering: only the eye this frame carries (see VRManager::ComposeWinlatorXRFrame)
		RenderSingleEye(gVR->CurrentAerEye(), pSystem);
	}
	else
	{
		for (int eye = 0; eye < 2; ++eye)
		{
			RenderSingleEye(eye, pSystem);
		}
	}

	// Per-eye path: route the upcoming HUD/2D pass into the eye-shaped HUD texture (same reason as the
	// eyes: correct shape) so it composites cleanly into each half. The engine draws the actual HUD after
	// this function returns; the redirect stays active until ComposeWinlatorXRFrame clears it.
	if (UsePerEyeRenderTargets())
	{
		if (IDirect3DSurface9* hudSurf = (IDirect3DSurface9*)gVR->GetHudRenderSurface())
		{
			gVR->SetRenderRedirect(hudSurf);
			dxvkGetCreatedDevice()->SetRenderTarget(0, hudSurf);
		}
	}

	vector2di renderSize = gVR->GetRenderSize();
	m_pGame->m_pRenderer->SetScissor(0, 0, renderSize.x, renderSize.y);
	// clear render target to fully transparent for HUD render
	dxvkGetCreatedDevice()->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 0, 0);

	if (ShouldRender2D())
	{
		// some things just can't properly be rendered in VR, specifically anything that has to do with zoom (binoculars, scopes, ...)
		// in such instances, we switch to putting the game on a plane in front of the player.
		// depending on the setup, though, we might still want to do stereo 3D rendering for the plane, in particular for
		// the binoculars in motion control mode.

		if (ShouldRenderStereo())
		{
			for (int eye = 0; eye < 2; ++eye)
			{
				m_pGame->m_pRenderer->ClearColorBuffer(Vec3(0, 0, 0));
				CCamera cam = m_originalViewCamera;
				gVR->Modify3DCamera(eye, cam);
				pSystem->SetViewCamera(cam);
				m_viewCamOverridden = true;

				pSystem->RenderBegin();
				pSystem->Render();
				if (gVR->IsDrivingVehicleInCinemaMode())
					DrawCrosshair();
				gVR->CaptureStereo(eye);
			}
			// clear render target to fully transparent for HUD render
			dxvkGetCreatedDevice()->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 0, 0);
		}
		else
		{
			if (gVR->UseMotionControllers())
			{
				// still want to incorporate head or controller movements into camera
				CCamera cam = m_originalViewCamera;
				gVR->Modify2DCamera(cam);
				pSystem->SetViewCamera(cam);
				m_viewCamOverridden = true;
			}
			pSystem->RenderBegin();
			pSystem->Render();
			if (gVR->IsDrivingVehicleInCinemaMode())
				DrawCrosshair();
		}

		pSystem->SetViewCamera(m_originalViewCamera);
		m_viewCamOverridden = false;
	}

	m_pGame->GetSystem()->GetITimer()->Enable(true);
}

void VRRenderer::OnPrePresent()
{
	// In the per-eye path the HUD was rendered straight into the HUD texture (see Render()), so there is
	// nothing to capture; otherwise grab it from the back buffer as before.
	if (!UsePerEyeRenderTargets())
		gVR->CaptureHUD();
	gVR->MirrorEyeToBackBuffer();
}

void VRRenderer::OnPostPresent()
{
	gVR->FinishFrame();
}

const CCamera& VRRenderer::GetCurrentViewCamera() const
{
	if (m_viewCamOverridden)
		return m_originalViewCamera;

	return m_pGame->m_pSystem->GetViewCamera();
}

void VRRenderer::ProjectToScreenPlayerCam(float ptx, float pty, float ptz, float* sx, float* sy, float* sz)
{
	const CCamera &currentCam = m_pGame->m_pRenderer->GetCamera();
	m_pGame->m_pRenderer->SetCamera(GetCurrentViewCamera());
	m_pGame->m_pRenderer->ProjectToScreen(ptx, pty, ptz, sx, sy, sz);
	m_pGame->m_pRenderer->SetCamera(currentCam);
}

void VRRenderer::ChangeRenderResolution(int width, int height)
{
	CryLogAlways("Changing render resolution to %i x %i", width, height);

	// Far Cry's renderer has a safeguard where it checks the window size does not exceed the desktop size
	// this is no good for VR, so we temporarily change the memory where the game stores the desktop size (as found with the debugger)
	int* desktopWidth = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(m_pGame->m_pRenderer) + 0x016808);
	int* desktopHeight = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(m_pGame->m_pRenderer) + 0x01680C);
	int oldDeskWidth = *desktopWidth;
	int oldDeskHeight = *desktopHeight;
	*desktopWidth = width + 16;
	*desktopHeight = height + 32;

	m_ignoreWindowSizeChanges = true;
	m_pGame->m_pRenderer->ChangeResolution(width, height, 32, 0, false);
	m_pGame->m_pRenderer->EnableVSync(false);
	m_ignoreWindowSizeChanges = false;

	*desktopWidth = oldDeskWidth;
	*desktopHeight = oldDeskHeight;
}

bool VRRenderer::ShouldRenderVR() const
{
	if (m_pGame->IsCutSceneActive() && gVR->vr_cutscenes_cinema_mode != 0)
		return false;

	if (gVR->IsDrivingVehicleInCinemaMode())
		return false;

	if (!gVR->vr_render_world_while_zoomed)
	{
		return !ShouldRender2D();
	}

	return true;
}

bool VRRenderer::ShouldRender2D() const
{
	if (m_pGame->AreBinocularsActive())
		return true;

	CPlayer *player = m_pGame->GetLocalPlayer();
	if (player && player->IsWeaponZoomActive())
		return true;

	if (gVR->IsDrivingVehicleInCinemaMode())
		return true;

	return m_pGame->IsCutSceneActive() && gVR->vr_cutscenes_cinema_mode != 0;
}

bool VRRenderer::ShouldRenderStereo() const
{
	if (gVR->IsDrivingVehicleInCinemaMode() && gVR->vr_vehicles_cinema_mode == 2)
		return true;

	return m_pGame->IsCutSceneActive() && gVR->vr_cutscenes_cinema_mode == 2;
}

bool VRRenderer::UsePerEyeRenderTargets() const
{
	return gVR->IsUsingWinlatorXR() && !gVR->UseWinlatorAER()
		&& ShouldRenderVR() && !ShouldRender2D() && !gVR->IsMenuActive();
}

void VRRenderer::RenderSingleEye(int eye, ISystem* pSystem)
{
	CCamera eyeCam = m_originalViewCamera;
	gVR->ModifyViewCamera(eye, eyeCam);
	pSystem->SetViewCamera(eyeCam);
	m_viewCamOverridden = true;
	//m_pGame->m_pRenderer->EF_Query(EFQ_DrawNearFov, (INT_PTR)&fov);

	// Per-eye path: bind this eye's eye-shaped offscreen texture as the engine's render target so its
	// FOV (which follows the render-target shape) comes out correct, and the engine renders straight into
	// the texture we later composite - no separate capture needed. The redirect stays active so the
	// engine's own back-buffer re-binds during the scene pass are also routed here.
	bool perEyeRT = UsePerEyeRenderTargets();
	IDirect3DSurface9* eyeSurf = perEyeRT ? (IDirect3DSurface9*)gVR->GetEyeRenderSurface(eye) : nullptr;
	if (perEyeRT && eyeSurf)
	{
		gVR->SetRenderRedirect(eyeSurf);
		dxvkGetCreatedDevice()->SetRenderTarget(0, eyeSurf);
	}
	else
	{
		perEyeRT = false;
	}

	m_pGame->m_pRenderer->ClearColorBuffer(Vec3(0, 0, 0));

	if (ShouldRenderVR())
	{
		if (eye == 1)
			m_pGame->GetSystem()->GetITimer()->Enable(false);

		pSystem->RenderBegin();
		pSystem->Render();
		DrawCrosshair();
		if (gVR->vr_debug_draw_grip)
		{
			if (CPlayer* player = m_pGame->GetLocalPlayer())
			{
				if (CWeaponClass* weapon = player->GetSelectedWeapon())
					weapon->DebugDrawGripPositions(m_pGame->m_pRenderer);
			}
		}
	}

	pSystem->SetViewCamera(m_originalViewCamera);
	m_viewCamOverridden = false;

	if (!perEyeRT)
		gVR->CaptureEye(eye);
	// else: the eye was rendered directly into its texture (m_d3d->eyeTextures[eye])
}

void VRRenderer::DrawCrosshair()
{
	if (gVR->vr_crosshair == 0)
		return;

	// don't show crosshair if HUD is disabled (e.g. during cutscenes
	if (m_pGame->cl_display_hud->GetIVal() == 0 || gVR->IsMenuActive())
		return;

	CPlayer* pPlayer = m_pGame->GetLocalPlayer();
	if (!pPlayer || !pPlayer->GetSelectedWeapon())
		return;

	if (pPlayer->m_stats.reloading || pPlayer->IsSwimming())
		return;

	WeaponParams wp;
	pPlayer->GetCurrentWeaponParams(wp);
	if (wp.iFireModeType == FireMode_Melee)
		return;

	const CCamera& cam = m_originalViewCamera;
	Vec3 muzzlePos, crosshairPos;
	Matrix33 transform;
	transform.SetRotationXYZ(Deg2Rad(cam.GetAngles()));
	Vec3 crosshairAngles;
	pPlayer->GetFirePosAngles(muzzlePos, crosshairAngles);
	transform.SetRotationXYZ(Deg2Rad(crosshairAngles));
	Vec3 dir = -transform.GetColumn(1);
	dir.Normalize();
	float maxDistance = 16.f;
	float crosshairSize = 0.03f;

	IPhysicalEntity* skipPlayer = pPlayer->GetEntity()->GetPhysics();
	IPhysicalEntity* skipVehicle = nullptr;
	if (pPlayer->GetVehicle())
	{
		skipVehicle = pPlayer->GetVehicle()->GetEntity()->GetPhysics();
		maxDistance = 24.f;
		crosshairSize = 0.06f;
	}
	else if (gVR->vr_crosshair == 2)
		maxDistance = 100.f;
	const int objects = ent_all;
	const int flags = rwi_separate_important_hits;

	ray_hit hit;
	IPhysicalWorld *physicalWorld = m_pGame->GetSystem()->GetIPhysicalWorld();
	if (physicalWorld->RayWorldIntersection(muzzlePos, dir*maxDistance, objects, flags, &hit, 1, skipPlayer, skipVehicle))
	{
		crosshairPos = hit.pt;
	}
	else
	{
		crosshairPos = muzzlePos + dir * maxDistance;
	}

	// for the moment, draw something primitive with the debug tools. Maybe later we can find something more elegant...
	if (gVR->vr_crosshair == 1 || pPlayer->GetVehicle())
	{
		m_pGame->m_pRenderer->SetState(GS_NODEPTHTEST);
		m_pGame->m_pRenderer->DrawBall(crosshairPos - dir * 0.06f, crosshairSize);
	}
	else
	{
		CFColor laserColor(1, 0, 0, 0.5f);
		m_pGame->m_pRenderer->DrawLineColor(muzzlePos, laserColor, crosshairPos, laserColor);
	}
}
