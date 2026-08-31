#pragma once

class VRInput;
constexpr uint32_t HAPTIC_STEPS_PER_SEC = 30;
constexpr uint32_t MAX_HAPTIC_STEPS = 3 * HAPTIC_STEPS_PER_SEC;

struct HapticEffect
{
	uint8_t amplitudeSteps[MAX_HAPTIC_STEPS];
	uint32_t numSteps;
};

struct ActiveHapticEffect
{
	HapticEffect* effect = nullptr;
	uint32_t curStep = 0;
	float amplitudeModifier = 1.0f;
};


class VRHaptics
{
public:
	// full initialisation: controller vibration + bHaptics vest + ProTubeVR (SteamVR/PC)
	void Init(CXGame* game, VRInput* vrInput);
	// controller vibration only (WinlatorXR on a standalone headset - no vest/ProTube there)
	void InitControllerHaptics(CXGame* game, VRInput* vrInput);
	void Update();

	// Gameplay/Lua code calls into this class freely (e.g. weapon scripts register bHaptics effects
	// during level load), so every entry point must bail out gracefully when the corresponding
	// subsystem was never initialised.
	bool IsInitialized() const { return m_pGame != nullptr && m_vrInput != nullptr; }
	bool AreExternalHapticsReady() const { return m_externalHapticsReady; }

	void RegisterBHapticsEffect(const char* key, const char* file);

	void TriggerEffect(int hand, const char* effectName, float amplitudeModifier = 1.0f);
	void TriggerBHapticsEffect(const char* key, float intensity = 1.0f, float offsetAngleX = 0, float offsetY = 0);
	bool IsBHapticsEffectPlaying(const char* key) const;
	void StopBHapticsEffect(const char* key);
	void StopEffects(int hand);
	void StopAllEffects();

	void ProtubeKick(float power, bool twoHanded);
	void ProtubeRumble(float power, float seconds, bool twoHanded);
	void ProtubeShot(float kickPower, float rumblePower, float rumbleSeconds, bool twoHanded);

	void CreateFlatEffect(const char* effectName, float duration, float amplitude, float easeInTime = 0.0f, float easeOutTime = 0.0f);
	void CreateCustomEffect(const char* effectName, float* amplitudes, int count);

private:
	void InitEffects();

	CXGame* m_pGame = nullptr;
	VRInput* m_vrInput = nullptr;
	bool m_externalHapticsReady = false;

	std::map<std::string, HapticEffect> m_effects;
	std::vector<ActiveHapticEffect> m_activeEffects[2];
	
	float m_nextUpdateTime = 0.0f;
};
