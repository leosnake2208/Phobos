#include <Phobos.h>
#include <Utilities/Macro.h>

#include <TacticalClass.h>
#include <GameOptionsClass.h>
#include <MessageListClass.h>
#include <Unsorted.h>

// WASD camera panning (mirrors the Vinifera "Feature A2" work on Tiberian Sun).
//
// Every active game frame we poll the physical W/A/S/D keys and nudge the tactical
// camera. Because GetAsyncKeyState is a passive read (it does NOT consume the key),
// W/A/S/D still fire their vanilla command hotkeys - those will need to be rebound
// in KeyboardMD.ini, exactly like the TibSun project.
//
// Camera internals reversed from gamemd.exe 1.11:
//   TacticalCoord1 (this+0xD64) is the authoritative logical camera position in
//   screen space; TacticalCoord2 (this+0xD74) mirrors it. The render-time TacticalPos
//   (this+0xB0) is *derived* from TacticalCoord1 by the refresh routine below.
//     0x6D8640  __thiscall(this, Point2D*)  clamps a screen-space position to the
//               valid map view bounds (returns whether it clamped). `this` is unused.
//     0x6D8B30  __thiscall(this)            recomputes TacticalPos (0xB0) from
//               TacticalCoord1 and marks the view dirty.
//   this+0xD7D is the "position updated / needs redraw" flag SetTacticalPosition sets.

namespace CameraPanWASD
{
	// Per-frame pan distance in tactical pixels. Tunable; 16 matched the confirmed-good
	// feel of the TibSun implementation. Adjust after in-game testing.
	static constexpr int Step = 16;

	using ClampPositionFunc = void(__thiscall*)(TacticalClass*, Point2D*);
	using RefreshPositionFunc = void(__thiscall*)(TacticalClass*);

	static bool InputBlocked()
	{
		// The window doesn't own the keyboard right now (GetAsyncKeyState ignores focus),
		// or a modal dialog (menu/options) is up.
		if (!Game::IsFocused || Game::SpecialDialog != 0)
			return true;

		// Composing a chat message - W/A/S/D are letters, leave the camera alone.
		if (MessageListClass::Instance.HasEditFocus())
			return true;

		return false;
	}

	static void Update()
	{
		if (InputBlocked())
			return;

		auto const IsDown = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };

		// Let Ctrl/Shift/Alt + letter game commands through untouched.
		if (IsDown(VK_CONTROL) || IsDown(VK_SHIFT) || IsDown(VK_MENU))
			return;

		int dx = 0, dy = 0;
		if (IsDown('W')) dy -= 1;
		if (IsDown('S')) dy += 1;
		if (IsDown('A')) dx -= 1;
		if (IsDown('D')) dx += 1;

		if (dx == 0 && dy == 0)
			return;

		auto const pTac = TacticalClass::Instance;
		if (!pTac)
			return;

		Point2D next { pTac->TacticalCoord1.X + dx * Step, pTac->TacticalCoord1.Y + dy * Step };

		// Clamp against the real map bounds, commit both coord copies, refresh the view.
		reinterpret_cast<ClampPositionFunc>(0x6D8640)(pTac, &next);
		pTac->TacticalCoord1 = next;
		pTac->TacticalCoord2 = next;
		reinterpret_cast<RefreshPositionFunc>(0x6D8B30)(pTac);
		*(reinterpret_cast<char*>(pTac) + 0xD7D) = 1; // TacticalPosUpdated
	}
}

// 0x55D377 sits just past the "is game active" gate at the top of the main loop and
// runs once per active frame. Stolen bytes: `mov al, byte ptr [0xA8ED80]` (5 bytes).
DEFINE_HOOK(0x55D377, MainLoop_WASDCameraPan, 0x5)
{
	CameraPanWASD::Update();
	return 0;
}
