#pragma once
#include <openvr.h>
#include "WinlatorXR.h"

#undef GetUserName

class VRInput
{
public:
	// SteamVR / OpenVR action-manifest backend
	bool Init(CXGame* game);
	// WinlatorXR backend: the same gameplay mappings, fed from the XrAPI packet instead of OpenVR
	// actions (the bindings mirror steamvr/bindings_touch.json)
	bool InitWinlatorXR(CXGame* game);

	void ProcessInput();
	void ProcessInputOnFoot();
	void ProcessInputInVehicles();
	void ProcessInputBinoculars();

	void TriggerHaptics(int hand, float amplitude, float frequency, float duration);
	// WinlatorXR has no haptic API of its own; VRManager forwards these per frame via WinlatorXR::SendState
	float GetWinlatorHapticAmplitude(int hand) const { return m_wxrHaptics[hand & 1]; }

	Matrix34 GetControllerTransform(int hand);

private:
	// --- WinlatorXR backend ---
	// Synthetic action handles: small integers that never collide with real OpenVR handles, so the
	// existing gameplay code can keep passing vr::VRActionHandle_t around unchanged.
	enum WxrAction : unsigned int
	{
		WXR_NONE = 0,
		WXR_MENU, WXR_USE, WXR_BINOCULARS, WXR_ZOOMIN, WXR_ZOOMOUT, WXR_GRIP,
		WXR_MOVE, WXR_TURN, WXR_SNAPLEFT, WXR_SNAPRIGHT, WXR_SPRINT, WXR_JUMP, WXR_CROUCH,
		WXR_STEER, WXR_ACCELERATE, WXR_BRAKE, WXR_LEAVE, WXR_ATTACK, WXR_CHANGEVIEW, WXR_CHANGESEAT, WXR_LIGHTS, WXR_VEHICLE_RELOAD,
		WXR_FIRE, WXR_RELOAD, WXR_NEXT, WXR_GRENADES,
		WXR_ACTION_COUNT
	};
	static const vr::VRInputValueHandle_t WXR_HAND_LEFT = 1001;
	static const vr::VRInputValueHandle_t WXR_HAND_RIGHT = 1002;
	// digital action state per action and per device restriction (0 = any hand, 1 = left, 2 = right)
	struct WxrDigital { bool active = false; bool state = false; bool changed = false; };
	struct WxrAnalog { bool active = false; float x = 0, y = 0; };

	bool m_usingWinlatorXR = false;
	WinlatorXR::InputState m_wxrState;
	WxrDigital m_wxrDigital[WXR_ACTION_COUNT][3];
	WxrAnalog m_wxrAnalog[WXR_ACTION_COUNT];
	float m_wxrHaptics[2] = { 0.f, 0.f };
	bool m_wxrWarnedNoGrip = false;

	void UpdateWinlatorXRActions();
	void SetWxrDigital(WxrAction action, bool state, int device = 0);
	void SetWxrAnalog(WxrAction action, float x, float y);
	// backend-neutral queries used by all the Handle*/GetFloatValue helpers below
	void QueryDigital(vr::VRActionHandle_t action, vr::VRInputValueHandle_t restrictToDevice, vr::InputDigitalActionData_t& out);
	void QueryAnalog(vr::VRActionHandle_t action, vr::InputAnalogActionData_t& out);
	// --- end WinlatorXR backend ---

	struct DoubleBindAction
	{
		vr::VRActionHandle_t handle = vr::k_ulInvalidActionHandle;
		bool isPressed = false;
		float timeFirstPressed = 0;
	};

	CXGame* m_pGame = nullptr;

	vr::VRActionSetHandle_t m_defaultSet = vr::k_ulInvalidActionSetHandle;
	vr::VRActionSetHandle_t m_moveSet = vr::k_ulInvalidActionSetHandle;
	vr::VRActionSetHandle_t m_weaponsSet = vr::k_ulInvalidActionSetHandle;
	vr::VRActionSetHandle_t m_vehiclesSet = vr::k_ulInvalidActionHandle;

	vr::VRInputValueHandle_t m_handHandle[2] = { vr::k_ulInvalidInputValueHandle };
	vr::VRActionHandle_t m_handPoses[2] = { vr::k_ulInvalidActionHandle };
	vr::VRActionHandle_t m_haptics = vr::k_ulInvalidActionHandle;
	DoubleBindAction m_defaultUse;
	DoubleBindAction m_defaultMenu;
	vr::VRActionHandle_t m_defaultBinoculars = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_defaultZoomIn = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_defaultZoomOut = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_defaultGrip = vr::k_ulInvalidActionHandle;

	vr::VRActionHandle_t m_moveMove = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_moveTurn = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_moveSnapTurnLeft = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_moveSnapTurnRight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_moveSprint = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_moveJump = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_moveCrouch = vr::k_ulInvalidActionHandle;

	vr::VRActionHandle_t m_vehiclesSteer = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_vehiclesAccelerate = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_vehiclesBrake = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_vehiclesLeave = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_vehiclesAttack = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_vehiclesChangeView = vr::k_ulInvalidActionHandle;
	DoubleBindAction m_vehiclesChangeSeat;
	vr::VRActionHandle_t m_vehiclesLights = vr::k_ulInvalidActionHandle;
	DoubleBindAction m_vehiclesReloadFireMode;

	vr::VRActionHandle_t m_weaponsFire = vr::k_ulInvalidActionHandle;
	DoubleBindAction m_weaponsReloadFireMode;
	DoubleBindAction m_weaponsNextDrop;
	DoubleBindAction m_weaponsGrenades;

	using TriggerFn = void (CXClient::*)(float value, XActivationEvent ae);

	void HandleBooleanAction(vr::VRActionHandle_t actionHandle, TriggerFn trigger, bool continuous = true, vr::VRInputValueHandle_t restrictToDevice = vr::k_ulInvalidInputValueHandle);
	void HandleAnalogAction(vr::VRActionHandle_t actionHandle, int axis, TriggerFn trigger);
	float GetFloatValue(vr::VRActionHandle_t actionHandle, int axis = 0, bool *isActive = nullptr);

	void InitDoubleBindAction(DoubleBindAction& action, const char* actionName);
	void HandleDoubleBindAction(DoubleBindAction& action, TriggerFn shortPressTrigger, TriggerFn longPressTrigger, bool longContinuous = true);

	bool IsHandTouchingHead(int hand, float radius = 0.3f);
};
