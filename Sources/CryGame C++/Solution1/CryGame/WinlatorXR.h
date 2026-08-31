#pragma once

// Minimal client for WinlatorXR's XrAPI - a UDP-based OpenVR/OpenXR replacement exposed by the
// WinlatorXR compatibility layer that runs Windows apps under Wine on standalone Android VR headsets
// (Quest/Pico), see https://github.com/WinlatorXR/WinlatorXR.
//
// The protocol here was reverse-derived from the reference client implementation shipped in
// WinlatorXR/HaloCEWXR (HaloCEVR/VR/WinXrApi.cpp and WinXrApiUDP.cpp), which is the only public,
// working C++ integration available at the time this was written. XrAPI is explicitly labelled
// experimental (v0.5) by its authors, so this may need updating against future WinlatorXR versions.
namespace WinlatorXR
{
	struct HandState
	{
		// OpenXR *aim* pose of the controller (same reference space and conventions as the HMD pose)
		float qx = 0, qy = 0, qz = 0, qw = 1;
		float thumbX = 0, thumbY = 0;
		float posX = 0, posY = 0, posZ = 0;
		// OpenXR *grip* pose orientation (protocol >= 0.5 only, see hasGripOrientation). Matches
		// OpenVR's "handgrip" pose orientation, which is what the mod's hand/weapon code is tuned for.
		float gripQx = 0, gripQy = 0, gripQz = 0, gripQw = 1;
	};

	struct InputState
	{
		bool valid = false;
		int frameId = -1;

		HandState left;
		HandState right;

		float hmdQx = 0, hmdQy = 0, hmdQz = 0, hmdQw = 1;
		float hmdX = 0, hmdY = 0, hmdZ = 0;

		// WinlatorXR sends raw OpenXR data (STAGE reference space, x = right, y = up, -z = forward,
		// metres): the HMD quaternion is the left-eye view orientation, the HMD position is the
		// midpoint between both eye views, and ipd is the distance between the two eye views.
		float ipd = 0.064f;
		// symmetric horizontal/vertical field of view in degrees
		float fovH = 90.f;
		float fovV = 90.f;

		// Button order matches the XrAPI wire format exactly - do not reorder.
		// (thumb left/right/up/down are WinlatorXR's own 0.6 deadzone digitisation of the sticks;
		// lMenu is the dedicated menu button of the left controller)
		bool lGrip = false, lMenu = false, lThumbstickPress = false, lThumbLeft = false, lThumbRight = false;
		bool lThumbUp = false, lThumbDown = false, lTrigger = false, lButtonX = false, lButtonY = false;
		bool rButtonA = false, rButtonB = false, rGrip = false, rThumbstickPress = false, rThumbLeft = false;
		bool rThumbRight = false, rThumbUp = false, rThumbDown = false, rTrigger = false;

		// protocol >= 0.5 extras (absent -> defaults)
		float hmdAltitude = 0.f;          // head height above the floor (STAGE space), metres
		bool hasGripOrientation = false;  // HandState::gripQ* are valid
	};

	// Heuristic check for "are we likely running under WinlatorXR (i.e. Wine on the headset)".
	// Based on the presence of a Z:\ drive, which is Wine's conventional mapping of the host root
	// filesystem and is not normally present on a native Windows/SteamVR install.
	bool IsLikelyPresent();

	// Starts the UDP receive thread and announces VR mode to WinlatorXR. Safe to call once.
	void Init();
	void Shutdown();

	// Thread-safe copy of the most recently received packet. InputState::valid is false until the
	// first packet has arrived.
	InputState GetLatestState();

	// Sends haptics + mode + FOV back to WinlatorXR (call once per frame; uses a persistent socket).
	// modeVr: 0 = off, 1 = VR (frame is a head-tracked projection), 2 = screen (frame is shown on a
	// virtual screen). mode3d: 0 = mono, 1 = side-by-side (left eye in the left half of the frame),
	// 2 = alternate-eye. fovX/fovY in degrees; 0/0 = keep the headset's native FOV (which WinlatorXR
	// reports back in InputState::fovH/fovV).
	// In VR mode WinlatorXR additionally reads screen pixel (0,0): if G == 0 and A > 0, R selects the
	// head pose (InputState::frameId) the frame was rendered with, for correct late reprojection.
	void SendState(float lHaptics, float rHaptics, int modeVr, int mode3d, float fovX, float fovY);
}
