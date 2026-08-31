#pragma once
#include <openvr.h>
#include <vulkan/vulkan_core.h>

#include "VRHaptics.h"
#include "VRInput.h"

#undef GetUserName

class CWeaponClass;
class CXGame;
class IDirect3DDevice9Ex;
class IDirect3DTexture9;

Matrix34 OpenVRToFarCry(const vr::HmdMatrix34_t& mat);

class VRManager
{
public:
	VRManager();
	~VRManager();

	bool Init(CXGame *game);
	void Shutdown();

	void Update();

	void AwaitFrame();
	void HandleEvents();

	void CaptureEye(int eye);
	void CaptureStereo(int eye);
	void CaptureHUD();

	void MirrorEyeToBackBuffer();

	void SetDevice(IDirect3DDevice9Ex *device);
	void FinishFrame();

	vector2di GetRenderSize() const;

	void ModifyViewCamera(int eye, CCamera& cam);
	void Modify2DCamera(CCamera& cam);
	void Modify3DCamera(int eye, CCamera& cam);
	void ModifyBinocularCamera(IEntityCamera* cam);

	void GetEffectiveRenderLimits(int eye, float* left, float* right, float* top, float* bottom);

	void ProcessInput();
	void ProcessMenuInput();

	bool MousePressed() const { return m_mousePressed; }
	bool MouseReleased() const { return m_mouseReleased; }

	bool UseMotionControllers() const;
	Matrix34 GetControllerTransform(int hand);
	Matrix34 GetHmdTransform() { return m_hmdTransform; }

	void UpdatePlayerTurnOffset(float yawDeltaDeg);
	void UpdatePlayerMoveOffset(const Vec3& offset, const Ang3& hmdAnglesDeg);

	void OnPostPlayerCameraUpdate();
	void CommitYawAndOffsetChanges();

	VRHaptics* GetHaptics() { return &m_vrHaptics; }

	const Vec3& GetBinocularAngles() const { return m_curBinocularAngles; }
	const Vec3& GetBinocularPos() const { return m_curBinocularPos; }

	bool IsDrivingVehicleInCinemaMode();

private:
	struct D3DResources;

	CXGame* m_pGame;
	bool m_initialized = false;
	bool m_inputReady = false;
	// true if we detected WinlatorXR (Quest/Pico Wine layer) and are using its XrAPI UDP protocol
	// instead of OpenVR/SteamVR. See WinlatorXR.h.
	bool m_usingWinlatorXR = false;
	D3DResources* m_d3d = nullptr;
	vr::TrackedDevicePose_t m_headPose;
	vr::VROverlayHandle_t m_hudOverlay;
	vr::VROverlayHandle_t m_3DOverlay;
	float m_verticalFov;
	float m_horizontalFov;
	float m_vertRenderScale;
	float m_horzRenderScale;
	float m_prevViewYaw = 0;

	int m_curWindowWidth = 0;
	int m_curWindowHeight = 0;

	void SetHudAttachedToHead();
	void SetHudInFrontOfPlayer();
	void SetHudAsBinoculars();
	void SetHudAsWeaponZoom();

	void InitDevice(IDirect3DDevice9Ex* device);
	void CreateEyeTexture(int eye);
	void CreateHUDTexture();
	void CreateStereoTexture();

	void PrepareTextureForSubmission(IDirect3DTexture9* tex, vr::VRVulkanTextureData_t& vrTexData, VkImageLayout& origLayout);
	void PostSubmissionTransitionTexture(IDirect3DTexture9* tex, VkImageLayout origLayout);

	// Pulls the latest WinlatorXR packet and converts it into m_headPose (OpenVR pose format), so the
	// rest of the pose pipeline (UpdateHmdTransform, RecalibrateView, ...) is backend-agnostic.
	void UpdateWinlatorXRPose();
	// Eye-to-head transform (already converted to Far Cry space) for either backend.
	Matrix34 GetEyeToHeadTransform(int eye);
	// distance between the eyes as reported by WinlatorXR, in metres
	float m_winlatorEyeSeparation = 0.064f;
	bool m_winlatorPoseLogged = false;
	// HMD_SYNC id of the packet whose pose was used for the current frame; painted into the frame-sync
	// pixel so WinlatorXR can pick the matching pose when it reprojects our image (see ComposeWinlatorXRFrame)
	int m_winlatorFrameSync = 0;
	// what we tell WinlatorXR to do with the frame we just composed (see WinlatorXR::SendState)
	int m_winlatorModeVr = 2;
	int m_winlatorMode3d = 0;
	// WinlatorXR poses live in OpenXR LOCAL space (origin = headset at session start, not the floor).
	// Protocol 0.5 also reports the head's height above the floor, from which we derive this offset
	// to lift all poses (head and controllers) into a floor-relative frame like SteamVR's standing space.
	float m_winlatorFloorOffset = 0.f;
	bool m_winlatorFloorOffsetValid = false;
	// true once RecalibrateView() has run on a valid pose (don't use the sign of m_referenceHeight as
	// a sentinel for this: in LOCAL space the head can legitimately be below the origin)
	bool m_referenceCalibrated = false;

public:
	// y offset (metres) to add to any raw WinlatorXR pose position to make it floor-relative
	float GetWinlatorFloorOffset() const { return m_winlatorFloorOffsetValid ? m_winlatorFloorOffset : 0.f; }
private:

	// WinlatorXR has no compositor API: it just grabs the game window. So instead of submitting eye
	// textures we compose the final side-by-side (or flat) frame into the back buffer ourselves.
	void ComposeWinlatorXRFrame();
	void DrawTexturedQuad(IDirect3DTexture9* texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, bool alphaBlend);
	// draws the captured HUD for 'eye' into the frame region [x0, x0+width) x [0, height)
	void DrawHud(int eye, int x0, int width, int height);
	int m_winlatorAerEye = 0;

	// WinlatorXR unconditionally emulates a mouse/keyboard from the controllers (trigger = left click,
	// grip = right click, thumbstick up/down = mouse wheel, menu button = Esc, ...). Those events reach
	// the game in addition to our own XrAPI-driven input, and the mouse wheel even crashes Far Cry's
	// window procedure under Wine. So: subclass the game window to drop wheel messages, and disable
	// keyboard/mouse gameplay actions while motion controls are in charge.
	void InstallWinlatorXRWindowHook(IDirect3DDevice9Ex* device);
	void UpdateDesktopInputBlock();
	bool m_desktopInputBlocked = false;

	// calibrated standing head height minus the user's vr_height_offset (see there)
	float GetEffectiveReferenceHeight() const { return m_referenceHeight - vr_height_offset; }

public:
	// VR-specific cvars
	float vr_yaw_deadzone_angle;
	int vr_render_force_max_terrain_detail;
	int vr_render_force_obj_draw_dist;
	int vr_enable_motion_controllers;
	int vr_window_width;
	int vr_window_height;
	int vr_mirrored_eye;
	int vr_debug_draw_grip;
	int vr_debug_override_grip;
	float vr_melee_swing_threshold;
	int vr_snap_turn_amount;
	float vr_smooth_turn_speed;
	float vr_button_long_press_time;
	float vr_haptics_effect_strength;
	float vr_weapon_pitch_offset;
	float vr_weapon_yaw_offset;
	int vr_crosshair;
	int vr_movement_dir;
	int vr_show_empty_hands;
	int vr_immersive_ladders;
	int vr_render_world_while_zoomed;
	float vr_binocular_size;
	float vr_scope_size;
	int vr_seated_mode;
	// extra metres added to the player's in-game eye height (positive = taller). Applied to the
	// calibrated reference height, so physical crouching/leaning keep working unchanged.
	float vr_height_offset;
	int vr_cutscenes_cinema_mode;
	int vr_vehicles_cinema_mode;
	float vr_hud_distance;
	float vr_hud_width;
	float vr_menu_distance;
	float vr_menu_width;
	int vr_skip_vehicle_transitions;
	int vr_decouple_vehicle_rotations;
	int vr_winlatorxr_render_height;
	int vr_winlatorxr_block_desktop_input;
	int vr_winlatorxr_anamorphic;
	int vr_winlatorxr_max_fps;

	// Under WinlatorXR the final frame is side-by-side in a back buffer that is also the engine's
	// render target, so a plain composite halves the horizontal resolution of each eye. With
	// vr_winlatorxr_anamorphic the engine renders each eye 2x wider (same FOV, i.e. horizontally
	// oversampled) and the squeeze into the half-frame brings it back to full per-eye detail.
	// Returns that horizontal render scale (1 or 2); the "logical" eye size is GetRenderSize().x / scale.
	int WinlatorRenderScaleX() const { return (m_usingWinlatorXR && vr_winlatorxr_anamorphic != 0) ? 2 : 1; }

	bool IsUsingWinlatorXR() const { return m_usingWinlatorXR; }
	// The game renders the world into a single shared back buffer that is also the frame WinlatorXR
	// grabs and splits into two eyes. If that back buffer were sized to one eye (GetRenderSize), the
	// side-by-side composite would have to squeeze both eyes into it and each eye would lose half its
	// width. So the back buffer is kept at the full X screen; the eyes are still captured at eye size.
	vector2di GetWinlatorBackbufferSize() const;

	// Alternate-eye rendering (WinlatorXR mode3d=2): every frame carries ONE eye at the full frame
	// resolution and WinlatorXR keeps a framebuffer + pose per eye. Doubles per-eye pixels compared
	// to side-by-side and halves the render work per frame, at the cost of each eye updating at half
	// the frame rate.
	int vr_winlatorxr_aer;
	bool UseWinlatorAER() const { return m_usingWinlatorXR && vr_winlatorxr_aer != 0; }
	// which eye the current frame renders/carries in AER mode (0 = left, 1 = right)
	int CurrentAerEye() const { return m_winlatorAerEye; }
	ICVar* vr_debug_override_rh_offset = nullptr;
	ICVar* vr_debug_override_rh_angles = nullptr;
	ICVar* vr_debug_override_lh_offset = nullptr;
	ICVar* e_terrain_lod_ratio = nullptr;
	ICVar* e_detail_texture_min_fov = nullptr;
	ICVar* e_obj_view_dist_ratio = nullptr;

private:
	void RegisterCVars();

	VRInput m_input;
	VRHaptics m_vrHaptics;

	Vec3 m_referencePosition;
	Vec3 m_uncommittedReferencePosition;
	float m_referenceYaw = 0;
	float m_uncommittedReferenceYaw = 0;
	float m_referenceHeight = -1;
	Matrix34 m_hmdTransform;
	bool m_skippedRoomscaleMovement = false;
	bool m_wasInMenu = false;
	bool m_mousePressed = false;
	bool m_mouseReleased = false;
	float m_lastTimeButtonPressed = 0;
	bool m_buttonPressed = false;

	Matrix34 m_fixedHudTransform;
	bool m_fixedPositionInitialized = false;

	Ang3 m_curBinocularAngles;
	Vec3 m_curBinocularPos;
	CCamera m_binocularOriginalPlayerCam;
	bool m_wasBinocular = false;

	void UpdateHmdTransform();
	void ProcessRoomscale();

	void RecalibrateView();
};

extern VRManager* gVR;
