# Far Cry VR — Meta Quest 3 (Standalone) — BETA

Play **Far Cry (2004)** in VR **standalone on a Meta Quest 3**, via the
[`farcry_vrmod`](https://github.com/fholger/farcry_vrmod) mod running under
**WinlatorXR** (Wine + Box64 + DXVK on Android). No PC needed at runtime.

> This is an early beta. Expect rough edges — see **Known Issues** at the bottom.
> There is **no ready-made container image** in this release (WinlatorXR's backup
> export is unreliable on the current build), so this README walks you through
> building the container yourself. It takes ~5 minutes.

---

## What you need

1. **Meta Quest 3** (should also work on Quest 2/Pro; only Quest 3 is tested).
2. **WinlatorXR** — current builds work well, including the newer **`dawn`** line
   (also tested on `cats-27` / public v27). The mod forces the game window
   borderless at (0,0) itself, so decorated/offset windows on current/public
   builds no longer break the stereo pipeline.
3. A **Far Cry (2004) install** — a legitimate copy (e.g. the DRM-free Steam
   build). You provide your own game files.
4. This release bundle:
   - `FarCry/` — the game folder (goes to the headset)
   - `Mods/CryVR/` — the VR mod (`CryGame.dll`, scripts, bindings)
   - `system.cfg` — tuned VR settings
   - `FarCryVR.desktop` — the WinlatorXR shortcut
5. A **USB cable** + **adb** on a PC (for copying files), or a file manager on
   the headset.

---

## Install

### 1. Install WinlatorXR
Sideload a current WinlatorXR build (the newer `dawn` line works well; `cats-27` /
public v27 also work):
```
adb install -r WinlatorXR-<build>.apk
```
On first launch, **grant all permissions**, especially **All files access**
(Settings → Apps → WinlatorXR → Permissions → Files) — the game and config live
on shared storage and it must be able to read them. Let first-time setup finish
(it extracts the system image).

### 2. Copy the game + mod to the headset
Place the Far Cry folder at **`/sdcard/Download/FarCry`** (this becomes
`D:\FarCry` inside Wine). The mod goes inside it:
```
adb push FarCry/            /sdcard/Download/FarCry/
adb push Mods/              /sdcard/Download/FarCry/Mods/
adb push system.cfg         /sdcard/Download/FarCry/system.cfg
adb push FarCryVR.desktop   /sdcard/Download/Winlator/FarCryVR.desktop
```
Verify the mod DLL landed at
`/sdcard/Download/FarCry/Mods/CryVR/Bin32/CryGame.dll`.

### 3. Create the container
In WinlatorXR → **Containers** tab → **add a new container** (it'll be
**container 1** if it's your first). Set:

| Setting | Value | Why |
|---|---|---|
| **Screen size** | `1591x1440` | Recommended; **aspect must be ~1.10** (see note) |
| **Graphics driver** | wrapper / Turnip | The Adreno wrapper the fork ships |
| **DX wrapper** | **DXVK** | Direct3D 9 → Vulkan |
| **Drive `D:`** | `/sdcard/Download` | So `D:\FarCry` resolves — **essential** |

> **Aspect rule:** WinlatorXR renders each eye into a *square* framebuffer, so
> the screen size **must keep ~1.10 width:height** or the image looks squeezed.
> Good values: `1792x1624` (sharp) or `1591x1440` (smoother). **Bad:** anything
> near-square like `1660x1600` (squeezes the view). Do **not** exceed ~1792 wide
> — `1856+` exhausts the 32-bit process and crashes.

### 4. Point the shortcut at your container
Edit `/sdcard/Download/Winlator/FarCryVR.desktop` so `container_id` matches the
container you made (e.g. `container_id=1`). The shortcut already carries the
right `screenSize` and `execArgs=-MOD:CryVR`.

### 5. Launch
From WinlatorXR's **Shortcuts** tab, tap **FarCryVR** (or launch it from a
frontend). First boot takes ~3–4 minutes (Box64). **Wear the headset** during
boot — if it reads as off-face, the Quest suspends the app and it freezes.

---

## Recommended settings (already in `system.cfg`)

| Setting | Value | Notes |
|---|---|---|
| `vr_winlatorxr_aer` | `1` | **Alternate-eye rendering** — full resolution per eye |
| `vr_winlatorxr_render_height` | `1624` | Pair with the `1792` screen |
| screen (`.desktop`) | `1792x1624` | Recommended preset (**needs the LAA patch**, below) |
| `vr_seated_mode` | `1` | Comfortable; crouch with the button, not physically |
| `vr_height_offset` | `0.30` | Eye-height tweak (metres). Adjust to taste |
| `vr_snap_turn_amount` | `45` | 45° snap turn |
| `vr_render_force_max_terrain_detail` | `0` | Off = coarser distant terrain, better fps (default) |
| `e_view_dist_ratio` / `e_obj_view_dist_ratio` | `50` | Draw distance (far sight) |
| `e_terrain_lod_ratio` | `1.5` | Terrain LOD at distance |
| `e_vegetation_min_size` | `2` | Cull tiny vegetation |
| `e_detail_distance` | `6` | Detail-geometry distance |
| `r_DetailTextures` | `0` | Drop detail-texture pass |
| `e_shadow_maps_view_dist_ratio` | `4` | Draw shadows closer (big fps saving) |
| `e_particles_max_count` | `4096` | Particle cap |

**Large-address-aware patch (required for the 1792 preset).** Far Cry's launcher is a
32-bit exe capped at 2 GB, which is what makes `1856+` crash and what used to make `1792`
dip. **Patch `Bin32/FarCry.exe` to be large-address-aware** (set the PE
`IMAGE_FILE_LARGE_ADDRESS_AWARE` bit — e.g. NTCore's *4GB Patch*, or `editbin
/LARGEADDRESSAWARE`). With that, `1792x1624` holds a **locked 72 fps**. Keep a backup of
the original exe. If you don't patch it, use the **1591x1440** preset (no patch needed).

**Framerate:** the Quest is **draw/CPU-bound under Box64**. Trimming shadows and detail
(the block above) frees enough headroom that the LAA-patched `1792` preset stays pegged at
72 fps. Water reflections are kept on (they render correctly in AER).

**Resolution presets:**
- **Recommended:** `1792x1624` (`render_height 1624`) — **locked 72 fps** *with the LAA
  patch + shadow trims above*.
- **No-patch fallback:** `1591x1440` (`render_height 1440`) — ~60–72 fps, runs without the
  LAA patch. Do **not** exceed ~1792 wide even patched — higher is GPU-bound and crawls.

Always play on **>50% battery** — the Quest power-throttles at low charge and
tanks the frame rate regardless of settings.

---

## Controls (Touch controllers)

| Input | Action |
|---|---|
| Right trigger | Fire |
| Left trigger | Use / interact |
| **A** | Reload (hold: fire mode) |
| **B** | Switch weapon (hold: drop) |
| **X** | Binoculars |
| **Y** | Grenade (tap: cycle, hold: throw) |
| **Left menu button** | Game menu |
| Right stick | Turn / 45° snap turn; push up = jump, down = crouch |
| Left stick | Move; click = sprint |
| Grips | Grab (two-handed weapon / ladders) |
| Right stick **click** | *unbound* — reserved for the WinlatorXR menu |

---

## Known issues (beta)

- **Main menu is unstable — leave the auto-load ON.** The game's packed
  frontend crashes on this build (attract-mode / map-load null-deref in
  `XRenderD3D9.dll`), so the main menu isn't reliable to navigate. The included
  `Mods/CryVR/Scripts/MenuScreens/MainScreen.lua` **auto-loads straight into
  Training** and is **enabled by default** — keep it. For save/load/options,
  use the **in-game pause menu** (stable), not the main menu. To restore the
  main menu, delete that file — but expect it to crash within a couple minutes.
- **Some map loads can crash** (`XRenderD3D9` null-read) — most often via the
  menu/demoloop path. Auto-loading directly into your target level avoids it;
  Training loads reliably.
- **Resolution ceiling ~1792 wide.** `1856+` crashes (32-bit memory). Keep the
  ~1.10 aspect (e.g. `1792x1624` or `1591x1440`).
- **AER** gives full per-eye resolution but each eye refreshes at half the
  composite rate; some may notice slight motion softness on fast head turns.
- **WinlatorXR container backup/export** is unreliable — hence these manual
  setup steps rather than a prefix image.
- Works on current WinlatorXR builds, including the newer **dawn** line (as well as
  cats-27 / public v27). Very old builds may render flat/doubled — update if so.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Image **squeezed/stretched** | Screen size isn't ~1.10 aspect — use `1792x1624` or `1591x1440` |
| **No stereo / no head tracking** (flat, frozen view) | The window must be borderless at (0,0) so WinlatorXR finds the sync pixel — the mod forces this automatically; if it persists, relaunch. |
| **Doubled** flat image (both eyes side by side) | Very old WinlatorXR build — update to a current build (dawn / cats-27 / v27) |
| Freezes on a static frame | Headset read as off-face — put it on; the app was suspended |
| Very low fps everywhere | Battery low (throttling) — charge to >50% |
| `D:\FarCry` not found / won't launch | Container `D:` drive isn't mapped to `/sdcard/Download` |
| Crash on the main menu / some map loads | Expected on this build — keep the auto-load-into-Training script and use the in-game pause menu |

---

*Mod: `farcry_vrmod` (fholger) + WinlatorXR (cmod). Far Cry © Crytek/Ubisoft —
bring your own legally-owned copy.*
