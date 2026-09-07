#include "StdAfx.h"
#include "VRInput.h"

#include "VRManager.h"
#include "WeaponClass.h"
#include "XPlayer.h"
#include "XVehicle.h"

std::string GetDLLPath()
{
	HMODULE hModule = GetModuleHandleA("CryGame.dll");
	if (hModule == nullptr)
		return "";

	char filePath[MAX_PATH];
	filePath[0] = 0;
	GetModuleFileNameA(hModule, filePath, MAX_PATH);
	return std::string(filePath);
}

std::string GetActionManifestPath()
{
	std::string dllPath = GetDLLPath();
	std::string parentPath = dllPath.substr(0, dllPath.find_last_of("\\/"));
	return parentPath + "\\..\\steamvr\\actions.json";
}

bool VRInput::Init(CXGame* game)
{
	std::string actionManifestPath = GetActionManifestPath();
	CryLogAlways("SteamVR action manifest path: %s", actionManifestPath.c_str());
	vr::EVRInputError err = vr::VRInput()->SetActionManifestPath(actionManifestPath.c_str());
	if (err != vr::VRInputError_None)
	{
		CryLogAlways("Failed to load SteamVR action manifest: %i", err);
		return false;
	}

	vr::VRInput()->GetInputSourceHandle("/user/hand/left", &m_handHandle[0]);
	vr::VRInput()->GetInputSourceHandle("/user/hand/right", &m_handHandle[1]);

	vr::VRInput()->GetActionSetHandle("/actions/default", &m_defaultSet);
	vr::VRInput()->GetActionSetHandle("/actions/move", &m_moveSet);
	vr::VRInput()->GetActionSetHandle("/actions/vehicles", &m_vehiclesSet);
	vr::VRInput()->GetActionSetHandle("/actions/weapons", &m_weaponsSet);

	vr::VRInput()->GetActionHandle("/actions/default/in/hapticvibration", &m_haptics);
	vr::VRInput()->GetActionHandle("/actions/default/in/HandPoseLeft", &m_handPoses[0]);
	vr::VRInput()->GetActionHandle("/actions/default/in/HandPoseRight", &m_handPoses[1]);
	InitDoubleBindAction(m_defaultMenu, "/actions/default/in/menu");
	InitDoubleBindAction(m_defaultUse, "/actions/default/in/use");
	vr::VRInput()->GetActionHandle("/actions/default/in/binoculars", &m_defaultBinoculars);
	vr::VRInput()->GetActionHandle("/actions/default/in/zoomin", &m_defaultZoomIn);
	vr::VRInput()->GetActionHandle("/actions/default/in/zoomout", &m_defaultZoomOut);
	vr::VRInput()->GetActionHandle("/actions/default/in/grip", &m_defaultGrip);

	vr::VRInput()->GetActionHandle("/actions/move/in/move", &m_moveMove);
	vr::VRInput()->GetActionHandle("/actions/move/in/continuousturn", &m_moveTurn);
	vr::VRInput()->GetActionHandle("/actions/move/in/turnleft", &m_moveSnapTurnLeft);
	vr::VRInput()->GetActionHandle("/actions/move/in/turnright", &m_moveSnapTurnRight);
	vr::VRInput()->GetActionHandle("/actions/move/in/sprint", &m_moveSprint);
	vr::VRInput()->GetActionHandle("/actions/move/in/jump", &m_moveJump);
	vr::VRInput()->GetActionHandle("/actions/move/in/crouch", &m_moveCrouch);

	vr::VRInput()->GetActionHandle("/actions/vehicles/in/steer", &m_vehiclesSteer);
	vr::VRInput()->GetActionHandle("/actions/vehicles/in/accelerate", &m_vehiclesAccelerate);
	vr::VRInput()->GetActionHandle("/actions/vehicles/in/brake", &m_vehiclesBrake);
	vr::VRInput()->GetActionHandle("/actions/vehicles/in/attack", &m_vehiclesAttack);
	vr::VRInput()->GetActionHandle("/actions/vehicles/in/changeview", &m_vehiclesChangeView);
	InitDoubleBindAction(m_vehiclesChangeSeat, "/actions/vehicles/in/changeseat");
	vr::VRInput()->GetActionHandle("/actions/vehicles/in/leave", &m_vehiclesLeave);
	vr::VRInput()->GetActionHandle("/actions/vehicles/in/lights", &m_vehiclesLights);
	InitDoubleBindAction(m_vehiclesReloadFireMode, "/actions/vehicles/in/reload");

	vr::VRInput()->GetActionHandle("/actions/weapons/in/fire", &m_weaponsFire);
	InitDoubleBindAction(m_weaponsReloadFireMode, "/actions/weapons/in/reload");
	InitDoubleBindAction(m_weaponsNextDrop, "/actions/weapons/in/next");
	InitDoubleBindAction(m_weaponsGrenades, "/actions/weapons/in/grenades");

	m_pGame = game;

	return true;
}

void VRInput::ProcessInput()
{
	if (m_usingWinlatorXR)
	{
		UpdateWinlatorXRActions();
	}
	else
	{
		vr::ETrackedControllerRole hand = vr::TrackedControllerRole_RightHand;
		vr::VRInput()->GetDominantHand(&hand);
		if (hand == vr::TrackedControllerRole_RightHand && m_pGame->g_LeftHanded->GetIVal() != 0)
			vr::VRInput()->SetDominantHand(vr::TrackedControllerRole_LeftHand);
		if (hand == vr::TrackedControllerRole_LeftHand && m_pGame->g_LeftHanded->GetIVal() == 0)
			vr::VRInput()->SetDominantHand(vr::TrackedControllerRole_RightHand);

		std::vector<vr::VRActiveActionSet_t> activeSets;
		activeSets.push_back({ m_defaultSet, vr::k_ulInvalidInputValueHandle });
		activeSets.push_back({ m_moveSet, vr::k_ulInvalidInputValueHandle });
		activeSets.push_back({ m_weaponsSet, vr::k_ulInvalidInputValueHandle });
		activeSets.push_back({ m_vehiclesSet, vr::k_ulInvalidInputValueHandle });

		vr::VRInput()->UpdateActionState(&activeSets[0], sizeof(vr::VRActiveActionSet_t), activeSets.size());
	}

	if (!m_pGame->GetClient())
		return;

	if (m_pGame->IsCutSceneActive())
	{
		HandleDoubleBindAction(m_defaultMenu, &CXClient::TriggerMenu, &CXClient::StopCutScene);
	}
	else
	{
		HandleDoubleBindAction(m_defaultMenu, &CXClient::TriggerMenu, &CXClient::TriggerScoreBoard);
	}
	if (gVR->vr_snap_turn_amount == 0 || gVR->IsDrivingVehicleInCinemaMode())
	{
		HandleAnalogAction(m_moveTurn, 0, &CXClient::TriggerTurnLR);
		if (gVR->IsDrivingVehicleInCinemaMode())
			HandleAnalogAction(m_moveTurn, 1, &CXClient::TriggerTurnUD);
	}
	else
	{
		HandleBooleanAction(m_moveSnapTurnLeft, &CXClient::TriggerSnapTurnLeft, false);
		HandleBooleanAction(m_moveSnapTurnRight, &CXClient::TriggerSnapTurnRight, false);
	}

	CPlayer* player = m_pGame->GetLocalPlayer();
	if (m_pGame->AreBinocularsActive())
		ProcessInputBinoculars();
	else if (player && player->GetVehicle() && player->GetVehicle()->GetType() != VHT_PARAGLIDER)  // paraglider works better with on-foot controls
		ProcessInputInVehicles();
	else
		ProcessInputOnFoot();
}

void VRInput::ProcessInputOnFoot()
{
	int mainHand = m_pGame->g_LeftHanded->GetIVal() != 0 ? 0 : 1;
	int offHand = m_pGame->g_LeftHanded->GetIVal() != 0 ? 1 : 0;
	CPlayer* player = m_pGame->GetLocalPlayer();
	if (player && player->GetSelectedWeapon())
	{
		CWeaponClass* weapon = player->GetSelectedWeapon();
		if (weapon->IsZoomActive() && (!IsHandTouchingHead(mainHand, 0.35f) || !player->IsTwoHandedModeActive()))
		{
			player->GetEntity()->SendScriptEvent(ScriptEvent_ZoomToggle, 2);
		}
		else if (player->IsTwoHandedModeActive() && !player->m_stats.running && !player->IsSwimming() && !weapon->IsZoomActive() && IsHandTouchingHead(mainHand, 0.3f) && weapon->HasActualScope())
		{
			player->GetEntity()->SendScriptEvent(ScriptEvent_ZoomToggle, 1);
		}
	}

	if (player && player->IsWeaponZoomActive())
	{
		HandleBooleanAction(m_defaultZoomIn, &CXClient::TriggerZoomIn, false);
		HandleBooleanAction(m_defaultZoomOut, &CXClient::TriggerZoomOut, false);
	}
	else
	{
		if (IsHandTouchingHead(offHand))
		{
			// if touching head, toggle flashlight or thermal vision
			HandleDoubleBindAction(m_defaultUse, &CXClient::TriggerFlashlight, &CXClient::TriggerItem1, false);
		}
		else
		{
			// otherwise, normal use
			HandleBooleanAction(m_defaultUse.handle, &CXClient::TriggerUse, false);
			m_defaultUse.isPressed = false;
		}
		HandleBooleanAction(m_defaultBinoculars, &CXClient::TriggerItem0, false);
		HandleBooleanAction(m_moveCrouch, &CXClient::TriggerMoveModeSwitch, false);
		HandleBooleanAction(m_moveJump, &CXClient::TriggerJump, false);
		HandleDoubleBindAction(m_weaponsNextDrop, &CXClient::TriggerNextWeapon, &CXClient::TriggerDropWeapon, false);
		HandleDoubleBindAction(m_weaponsGrenades, &CXClient::CycleGrenade, &CXClient::TriggerFireGrenade, false);
		HandleBooleanAction(m_moveSprint, &CXClient::TriggerRunSprint);
	}

	HandleAnalogAction(m_moveMove, 0, &CXClient::TriggerMoveLR);
	HandleAnalogAction(m_moveMove, 1, &CXClient::TriggerMoveFB);
	HandleBooleanAction(m_weaponsFire, &CXClient::TriggerFire0);
	HandleDoubleBindAction(m_weaponsReloadFireMode, &CXClient::TriggerReload, &CXClient::TriggerFireMode, false);
	HandleBooleanAction(m_defaultGrip, &CXClient::TriggerLeftGrip, true, m_handHandle[0]);
	HandleBooleanAction(m_defaultGrip, &CXClient::TriggerRightGrip, true, m_handHandle[1]);
}

void VRInput::ProcessInputInVehicles()
{
	CPlayer* player = m_pGame->GetLocalPlayer();
	CVehicle* vehicle = player->GetVehicle();
	if (vehicle->GetUserInState(CPlayer::PVS_DRIVER) == player)
	{
		HandleAnalogAction(m_vehiclesSteer, 0, &CXClient::TriggerMoveLR);
		HandleBooleanAction(m_vehiclesLights, &CXClient::TriggerFlashlight, false);
		HandleBooleanAction(m_vehiclesAttack, &CXClient::TriggerFire0);

		// combine accelerate/brake to movement value
		bool isAccelActive = false, isBrakeActive = false;
		float accel = GetFloatValue(m_vehiclesAccelerate, 0, &isAccelActive);
		float brake = GetFloatValue(m_vehiclesBrake, 0, &isBrakeActive);
		float move = accel - brake;

		if (isAccelActive && isBrakeActive)
			m_pGame->GetClient()->TriggerMoveFB(move, XActivationEvent());
		else
			HandleAnalogAction(m_vehiclesSteer, 1, &CXClient::TriggerMoveFB);

		if (player->GetSelectedWeapon() && player->GetSelectedWeapon()->GetName() != vehicle->GetWeaponName(CPlayer::PVS_DRIVER))
		{
			// using own weapon - allow to reload and grab with second hand
			HandleDoubleBindAction(m_vehiclesReloadFireMode, &CXClient::TriggerReload, &CXClient::TriggerFireMode, false);
			HandleDoubleBindAction(m_weaponsNextDrop, &CXClient::TriggerNextWeapon, &CXClient::TriggerDropWeapon, false);
			HandleBooleanAction(m_defaultGrip, &CXClient::TriggerLeftGrip, true, m_handHandle[0]);
			HandleBooleanAction(m_defaultGrip, &CXClient::TriggerRightGrip, true, m_handHandle[1]);
		}
	}
	else
	{
		HandleBooleanAction(m_weaponsFire, &CXClient::TriggerFire0);
		HandleDoubleBindAction(m_weaponsReloadFireMode, &CXClient::TriggerReload, &CXClient::TriggerFireMode, false);
		HandleDoubleBindAction(m_weaponsNextDrop, &CXClient::TriggerNextWeapon, &CXClient::TriggerDropWeapon, false);
		HandleBooleanAction(m_defaultGrip, &CXClient::TriggerLeftGrip, true, m_handHandle[0]);
		HandleBooleanAction(m_defaultGrip, &CXClient::TriggerRightGrip, true, m_handHandle[1]);
	}

	HandleBooleanAction(m_vehiclesLeave, &CXClient::TriggerUse, false);
	HandleBooleanAction(m_vehiclesChangeView, &CXClient::TriggerChangeView, false);
	HandleDoubleBindAction(m_vehiclesChangeSeat, &CXClient::TriggerRunSprint, &CXClient::TriggerFireMode, false);

	// process some of the default actions to prevent them from immediately triggering when exiting the vehicle
	HandleBooleanAction(m_defaultBinoculars, &CXClient::NoOp, false);
	HandleBooleanAction(m_defaultUse.handle, &CXClient::NoOp, false);
	HandleBooleanAction(m_moveCrouch, &CXClient::NoOp, false);
	HandleBooleanAction(m_moveJump, &CXClient::NoOp, false);
	HandleBooleanAction(m_moveSprint, &CXClient::NoOp, false);
}

void VRInput::ProcessInputBinoculars()
{
	HandleBooleanAction(m_defaultUse.handle, &CXClient::TriggerUse, false);
	HandleBooleanAction(m_defaultBinoculars, &CXClient::TriggerItem0, false);
	HandleBooleanAction(m_defaultZoomIn, &CXClient::TriggerZoomIn, false);
	HandleBooleanAction(m_defaultZoomOut, &CXClient::TriggerZoomOut, false);
	HandleAnalogAction(m_moveMove, 0, &CXClient::TriggerMoveLR);
	HandleAnalogAction(m_moveMove, 1, &CXClient::TriggerMoveFB);
}

void VRInput::TriggerHaptics(int hand, float amplitude, float frequency, float duration)
{
	hand = clamp_tpl(hand, 0, 1);
	if (m_usingWinlatorXR)
	{
		// forwarded to WinlatorXR once per frame by VRManager::FinishFrame (no frequency/duration
		// control there - it pulses the controller as long as it keeps receiving a level > 0)
		m_wxrHaptics[hand] = clamp_tpl(amplitude, 0.f, 1.f);
		return;
	}

	vr::VRInput()->TriggerHapticVibrationAction(m_haptics, 0.f, duration, frequency, amplitude, m_handHandle[hand]);
}

Matrix34 VRInput::GetControllerTransform(int hand)
{
	hand = clamp_tpl(hand, 0, 1);

	if (m_usingWinlatorXR)
	{
		// Position: the aim pose (a few cm in front of where OpenVR's grip pose sits - close enough).
		// Orientation: the grip pose when the protocol provides it, because that is what OpenVR's
		// "handgrip" pose is (and what the mod's weapon/hand code is tuned for); otherwise fall back
		// to the aim orientation, which points the weapon along the controller instead of the handle.
		const WinlatorXR::HandState& h = hand == 0 ? m_wxrState.left : m_wxrState.right;
		float qx = h.qx, qy = h.qy, qz = h.qz, qw = h.qw;
		if (m_wxrState.hasGripOrientation)
		{
			qx = h.gripQx; qy = h.gripQy; qz = h.gripQz; qw = h.gripQw;
		}
		else if (!m_wxrWarnedNoGrip && m_wxrState.valid)
		{
			m_wxrWarnedNoGrip = true;
			CryLogAlways("[WinlatorXR] packets carry no grip orientation (protocol < 0.5?) - using aim pose for the hands");
		}
		float len = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
		if (len < 1e-4f)
			return Matrix34::CreateIdentity();
		qx /= len; qy /= len; qz /= len; qw /= len;

		// same floor-relative lift as the head pose (see VRManager::UpdateWinlatorXRPose)
		float posY = h.posY + gVR->GetWinlatorFloorOffset();

		vr::HmdMatrix34_t m;
		m.m[0][0] = 1.f - 2.f * (qy * qy + qz * qz); m.m[0][1] = 2.f * (qx * qy - qw * qz);       m.m[0][2] = 2.f * (qx * qz + qw * qy);       m.m[0][3] = h.posX;
		m.m[1][0] = 2.f * (qx * qy + qw * qz);       m.m[1][1] = 1.f - 2.f * (qx * qx + qz * qz); m.m[1][2] = 2.f * (qy * qz - qw * qx);       m.m[1][3] = posY;
		m.m[2][0] = 2.f * (qx * qz - qw * qy);       m.m[2][1] = 2.f * (qy * qz + qw * qx);       m.m[2][2] = 1.f - 2.f * (qx * qx + qy * qy); m.m[2][3] = h.posZ;

		// same grip-pose orientation fix-up as the OpenVR path below
		Matrix33 correction = Matrix33::CreateRotationX(gf_PI / 2);
		return OpenVRToFarCry(m) * correction;
	}

	vr::InputPoseActionData_t data;
	vr::VRInput()->GetPoseActionDataForNextFrame(m_handPoses[hand], vr::TrackingUniverseStanding, &data, sizeof(data), vr::k_ulInvalidInputValueHandle);

	// the grip pose has a peculiar orientation that we need to fix
	Matrix33 correction = Matrix33::CreateRotationX(gf_PI/2);
	return OpenVRToFarCry(data.pose.mDeviceToAbsoluteTracking) * correction;
}

void VRInput::HandleBooleanAction(vr::VRActionHandle_t actionHandle, TriggerFn trigger, bool continuous, vr::VRInputValueHandle_t restrictToDevice)
{
	vr::InputDigitalActionData_t actionData;
	QueryDigital(actionHandle, restrictToDevice, actionData);
	if (actionData.bActive && actionData.bState && (continuous || actionData.bChanged))
	{
		(m_pGame->GetClient()->*trigger)(1.f, XActivationEvent());
	}
}

void VRInput::HandleAnalogAction(vr::VRActionHandle_t actionHandle, int axis, TriggerFn trigger)
{
	vr::InputAnalogActionData_t actionData;
	QueryAnalog(actionHandle, actionData);
	if (!actionData.bActive)
		return;

	float value = axis == 0 ? actionData.x : (axis == 1 ? actionData.y : actionData.z);
	(m_pGame->GetClient()->*trigger)(value, XActivationEvent());
}

float VRInput::GetFloatValue(vr::VRActionHandle_t actionHandle, int axis, bool* isActive)
{
	vr::InputAnalogActionData_t actionData;
	QueryAnalog(actionHandle, actionData);
	if (isActive != nullptr)
		*isActive = actionData.bActive;
	if (!actionData.bActive)
		return 0.f;

	float value = axis == 0 ? actionData.x : (axis == 1 ? actionData.y : actionData.z);
	return value;
}

void VRInput::InitDoubleBindAction(DoubleBindAction& action, const char* actionName)
{
	vr::VRInput()->GetActionHandle(actionName, &action.handle);
}

void VRInput::HandleDoubleBindAction(DoubleBindAction& action, TriggerFn shortPressTrigger, TriggerFn longPressTrigger, bool longContinuous)
{
	vr::InputDigitalActionData_t actionData;
	QueryDigital(action.handle, vr::k_ulInvalidInputValueHandle, actionData);
	if (!actionData.bActive)
	{
		action.isPressed = false;
		action.timeFirstPressed = 0;
		return;
	}

	if (actionData.bState && actionData.bChanged)
	{
		action.isPressed = true;
		action.timeFirstPressed = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime();
	}

	if (actionData.bState && action.isPressed)
	{
		if (action.timeFirstPressed == 0)
		{
			// long press already active
			if (longContinuous)
				(m_pGame->GetClient()->*longPressTrigger)(1.f, XActivationEvent());
		}
		else
		{
			float delta = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime() - action.timeFirstPressed;
			if (delta >= gVR->vr_button_long_press_time)
			{
				action.timeFirstPressed = 0;  // mark long press active
				(m_pGame->GetClient()->*longPressTrigger)(1.f, XActivationEvent());
			}
		}
	}

	if (!actionData.bState && action.isPressed)
	{
		if (action.timeFirstPressed != 0)
		{
			// enable short press action on release since long press was not active
			(m_pGame->GetClient()->*shortPressTrigger)(1.f, XActivationEvent());
		}

		action.isPressed = false;
		action.timeFirstPressed = 0;
	}
}

bool VRInput::IsHandTouchingHead(int hand, float radius)
{
	Matrix34 hmdTransform = gVR->GetHmdTransform();
	Vec3 hmdPos = hmdTransform.GetTranslation();
	hmdPos -= hmdTransform.GetForward() * 0.1f; // get a bit closer to the player head's centre
	Matrix34 controllerTransform = gVR->GetControllerTransform(hand);
	Vec3 controllerPos = controllerTransform.GetTranslation();
	return controllerPos.GetDistance(hmdPos) <= radius;
}

// ---------------------------------------------------------------------------------------------------
// WinlatorXR backend
// ---------------------------------------------------------------------------------------------------

bool VRInput::InitWinlatorXR(CXGame* game)
{
	m_pGame = game;
	m_usingWinlatorXR = true;

	// hand the gameplay code synthetic handles; QueryDigital/QueryAnalog resolve them per frame
	m_handHandle[0] = WXR_HAND_LEFT;
	m_handHandle[1] = WXR_HAND_RIGHT;
	m_defaultMenu.handle = WXR_MENU;
	m_defaultUse.handle = WXR_USE;
	m_defaultBinoculars = WXR_BINOCULARS;
	m_defaultZoomIn = WXR_ZOOMIN;
	m_defaultZoomOut = WXR_ZOOMOUT;
	m_defaultGrip = WXR_GRIP;
	m_moveMove = WXR_MOVE;
	m_moveTurn = WXR_TURN;
	m_moveSnapTurnLeft = WXR_SNAPLEFT;
	m_moveSnapTurnRight = WXR_SNAPRIGHT;
	m_moveSprint = WXR_SPRINT;
	m_moveJump = WXR_JUMP;
	m_moveCrouch = WXR_CROUCH;
	m_vehiclesSteer = WXR_STEER;
	m_vehiclesAccelerate = WXR_ACCELERATE;
	m_vehiclesBrake = WXR_BRAKE;
	m_vehiclesLeave = WXR_LEAVE;
	m_vehiclesAttack = WXR_ATTACK;
	m_vehiclesChangeView = WXR_CHANGEVIEW;
	m_vehiclesChangeSeat.handle = WXR_CHANGESEAT;
	m_vehiclesLights = WXR_LIGHTS;
	m_vehiclesReloadFireMode.handle = WXR_VEHICLE_RELOAD;
	m_weaponsFire = WXR_FIRE;
	m_weaponsReloadFireMode.handle = WXR_RELOAD;
	m_weaponsNextDrop.handle = WXR_NEXT;
	m_weaponsGrenades.handle = WXR_GRENADES;

	CryLogAlways("[WinlatorXR] controller input backend initialised (Touch-style bindings)");
	return true;
}

void VRInput::SetWxrDigital(WxrAction action, bool state, int device)
{
	WxrDigital& d = m_wxrDigital[action][device];
	// 'state' persists across frames (only 'active'/'changed' are reset per frame), so this is a
	// proper edge detection like OpenVR's bChanged
	d.changed = (d.state != state);
	d.state = state;
	d.active = true;
}

void VRInput::SetWxrAnalog(WxrAction action, float x, float y)
{
	WxrAnalog& a = m_wxrAnalog[action];
	a.active = true;
	a.x = x;
	a.y = y;
}

// SteamVR's "exponent 2" joystick curve, used by the shipped Touch bindings for move/turn/steer
static float WxrStickCurve(float v)
{
	return v < 0.f ? -(v * v) : v * v;
}

void VRInput::UpdateWinlatorXRActions()
{
	// Mark everything inactive first; SetWxr* re-activates what is bound. Unbound actions (e.g. the
	// separate vehicle accelerate/brake axes, which the Touch bindings don't use either) stay inactive
	// so the gameplay code takes the same fallbacks as on Touch controllers.
	for (int a = 0; a < WXR_ACTION_COUNT; ++a)
	{
		for (int d = 0; d < 3; ++d)
		{
			m_wxrDigital[a][d].active = false;
			m_wxrDigital[a][d].changed = false;
		}
		m_wxrAnalog[a].active = false;
	}

	WinlatorXR::InputState state = WinlatorXR::GetLatestState();
	if (!state.valid)
		return;
	m_wxrState = state;

	// Dominant ("weapon") hand and off hand. Mirrors what SteamVR does for us with SetDominantHand:
	// the bindings are written for a right-handed player and get mirrored for left-handed play.
	bool leftHanded = m_pGame->g_LeftHanded->GetIVal() != 0;
	const WinlatorXR::HandState& dom = leftHanded ? state.left : state.right;
	const WinlatorXR::HandState& off = leftHanded ? state.right : state.left;
	// per-hand buttons, expressed in "lower"/"upper" face button terms (X/A lower, Y/B upper)
	bool domLower = leftHanded ? state.lButtonX : state.rButtonA;
	bool domUpper = leftHanded ? state.lButtonY : state.rButtonB;
	bool offLower = leftHanded ? state.rButtonA : state.lButtonX;
	bool offUpper = leftHanded ? state.rButtonB : state.lButtonY;
	bool domTrigger = leftHanded ? state.lTrigger : state.rTrigger;
	bool offTrigger = leftHanded ? state.rTrigger : state.lTrigger;
	bool domStickClick = leftHanded ? state.lThumbstickPress : state.rThumbstickPress;
	bool offStickClick = leftHanded ? state.rThumbstickPress : state.lThumbstickPress;
	bool menuButton = state.lMenu; // the dedicated menu button only exists on the left controller

	// --- /actions/default ---
	// menu only on the dedicated left menu button, freeing the off-hand upper face button (Y) for grenades
	SetWxrDigital(WXR_MENU, menuButton);
	SetWxrDigital(WXR_USE, offTrigger);
	SetWxrDigital(WXR_BINOCULARS, offLower);
	// dominant stick as a d-pad (80% deadzone in the Touch bindings)
	SetWxrDigital(WXR_ZOOMIN, dom.thumbY > 0.8f);
	SetWxrDigital(WXR_ZOOMOUT, dom.thumbY < -0.8f);
	// grip is bound on both hands and queried per physical hand by the gameplay code
	SetWxrDigital(WXR_GRIP, state.lGrip || state.rGrip, 0);
	SetWxrDigital(WXR_GRIP, state.lGrip, 1);
	SetWxrDigital(WXR_GRIP, state.rGrip, 2);

	// --- /actions/move ---
	SetWxrAnalog(WXR_MOVE, WxrStickCurve(off.thumbX), WxrStickCurve(off.thumbY));
	SetWxrAnalog(WXR_TURN, WxrStickCurve(dom.thumbX), WxrStickCurve(dom.thumbY));
	SetWxrDigital(WXR_SNAPLEFT, dom.thumbX < -0.75f);
	SetWxrDigital(WXR_SNAPRIGHT, dom.thumbX > 0.75f);
	SetWxrDigital(WXR_SPRINT, offStickClick);
	SetWxrDigital(WXR_JUMP, dom.thumbY > 0.8f);
	SetWxrDigital(WXR_CROUCH, dom.thumbY < -0.8f);

	// --- /actions/vehicles ---
	SetWxrAnalog(WXR_STEER, WxrStickCurve(off.thumbX), WxrStickCurve(off.thumbY));
	// WXR_ACCELERATE / WXR_BRAKE intentionally left inactive (steer's y axis drives instead)
	SetWxrDigital(WXR_LEAVE, offTrigger);
	SetWxrDigital(WXR_CHANGESEAT, domUpper);
	SetWxrDigital(WXR_VEHICLE_RELOAD, domLower);
	SetWxrDigital(WXR_ATTACK, domTrigger);
	SetWxrDigital(WXR_CHANGEVIEW, offStickClick);
	SetWxrDigital(WXR_LIGHTS, offLower);

	// --- /actions/weapons ---
	SetWxrDigital(WXR_FIRE, domTrigger);
	// weapon switch on the dominant upper face button (B, tap = next / hold = drop); grenades on the
	// off-hand upper face button (Y, tap = cycle / hold = throw). The dominant thumbstick click is left
	// UNBOUND on purpose so WinlatorXR can use it for its own menu/mode toggle.
	SetWxrDigital(WXR_NEXT, domUpper);
	SetWxrDigital(WXR_RELOAD, domLower);
	SetWxrDigital(WXR_GRENADES, offUpper);
}

void VRInput::QueryDigital(vr::VRActionHandle_t action, vr::VRInputValueHandle_t restrictToDevice, vr::InputDigitalActionData_t& out)
{
	if (!m_usingWinlatorXR)
	{
		vr::VRInput()->GetDigitalActionData(action, &out, sizeof(vr::InputDigitalActionData_t), restrictToDevice);
		return;
	}

	memset(&out, 0, sizeof(out));
	if (action == WXR_NONE || action >= WXR_ACTION_COUNT)
		return;
	int device = restrictToDevice == WXR_HAND_LEFT ? 1 : (restrictToDevice == WXR_HAND_RIGHT ? 2 : 0);
	const WxrDigital& d = m_wxrDigital[action][device];
	out.bActive = d.active;
	out.bState = d.state;
	out.bChanged = d.changed;
	out.activeOrigin = restrictToDevice;
}

void VRInput::QueryAnalog(vr::VRActionHandle_t action, vr::InputAnalogActionData_t& out)
{
	if (!m_usingWinlatorXR)
	{
		vr::VRInput()->GetAnalogActionData(action, &out, sizeof(vr::InputAnalogActionData_t), vr::k_ulInvalidInputValueHandle);
		return;
	}

	memset(&out, 0, sizeof(out));
	if (action == WXR_NONE || action >= WXR_ACTION_COUNT)
		return;
	const WxrAnalog& a = m_wxrAnalog[action];
	out.bActive = a.active;
	out.x = a.x;
	out.y = a.y;
}
