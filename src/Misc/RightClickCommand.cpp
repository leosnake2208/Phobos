#include <Phobos.h>
#include <Utilities/Macro.h>

#include <MapClass.h>
#include <ObjectClass.h>
#include <DisplayClass.h>
#include <GeneralDefinitions.h>
#include <Ext/Techno/Body.h> // pulls in the complete FootClass/TechnoClass definitions that
                             // MapClass/DisplayClass inline abstract_casts require

// Right-click to command: the right mouse button issues orders, the left mouse button only
// selects / deploys (and deselects when clicking empty ground), matching the control style
// of modern RTS games. Off by default upstream; on by default in this integrated fork build.
// Enable with [Phobos] -> RightClickCommand. (Mirror of the Vinifera "Feature B" work.)
//
// The tactical mouse message handler is MouseClass method 0x6930A0. It dispatches Windows
// mouse messages through a jump table (0x693410). Reversed cases:
//   LBUTTONDOWN 0x201 -> 0x693126  (begin select/action, sets flag [this+0x555A]=1)
//   LBUTTONUP   0x202 -> 0x6931F9  (command dispatch: ProcessClickCoords -> DecideAction
//                                   -> apply 0x4AB9B0, at 0x69323E onward)
//   RBUTTONUP   0x205 -> 0x693366  (vanilla: cancel current mode, then deselect)
//
// RBUTTONUP and LBUTTONUP are cases of the SAME function sharing one stack frame and the
// same `this`; both prologues convert [esp+0x34] from a msg-point pointer into an inline
// Point2D value. So from the RMB-up handler we can jump straight into the LMB-up command
// dispatch at 0x69323E, reusing 100% of the game's command (and network) logic - no
// argument reconstruction needed.

namespace RightClickCommand
{
	// A special LEFT-click mode is active - RMB must keep its vanilla "cancel" behaviour
	// (cancel building placement, leave repair/sell/power/beacon/superweapon targeting),
	// and LMB must keep its vanilla behaviour (repair/sell/place/target).
	static bool InSpecialLeftClickMode()
	{
		auto& d = DisplayClass::Instance;
		return d.RepairMode
			|| d.SellMode
			|| d.PowerToggleMode
			|| d.PlaceBeaconMode
			|| d.PlanningMode
			|| d.CurrentSWTypeIndex >= 0    // superweapon targeting
			|| d.CurrentBuilding != nullptr; // building placement
	}

	// Set by the RBUTTONUP command hook just before it jumps into the shared LMB-up command
	// dispatch (0x69323E). That dispatch flows straight through the LBUTTONUP neutralise hook
	// at 0x693276, so the RMB-issued command must be allowed through unmodified - otherwise it
	// gets downgraded to None and right-click-to-command stops working.
	//
	// NON-static / externally visible on purpose: in this integrated build the RMB path also
	// flows through the multi-click hook at 0x693290 (MultiClickTypeSelect.cpp), which is the
	// LAST point the RMB event passes, so THAT hook consumes (clears) the flag and skips the
	// event. The 0x693276 hook below therefore only reads it and must NOT clear it.
	bool RmbCommandInProgress = false;

	// Which actions the LEFT button may still perform: selection and self-deploy only.
	// Everything else (Move/Attack/Enter/Harvest/Capture/Guard/...) is a command and belongs
	// to the RIGHT button now, so we neutralise it.
	static bool IsLeftClickAllowed(Action action)
	{
		switch (action)
		{
		case Action::None:
		case Action::Select:
		case Action::ToggleSelect:
		case Action::Self_Deploy:
			return true;
		default:
			return false;
		}
	}

	// Shared by the LBUTTONDOWN and LBUTTONUP handlers: if right-click-to-command is on and we
	// aren't in a special left-click mode, downgrade any command action (in EAX, freshly
	// returned by DecideAction) to None so the left button only selects/deploys. Returns true
	// if a command action was actually neutralised (the click landed on empty ground / an
	// enemy - a command target, not a selectable own unit).
	static bool NeutraliseLeftCommand(REGISTERS* R)
	{
		if (Phobos::Config::RightClickCommand && !InSpecialLeftClickMode())
		{
			if (!IsLeftClickAllowed(static_cast<Action>(R->EAX())))
			{
				R->EAX(static_cast<DWORD>(Action::None));
				return true;
			}
		}
		return false;
	}

	// Clear the whole current selection. MapClass::UnselectAll (0x48DC90) is the canonical
	// deselect-all (also used by Phobos' NextIdleHarvester command).
	static void DeselectAll()
	{
		MapClass::UnselectAll();
	}

	// Tick of the click that last (re)selected a unit, used by the deploy hold-off below.
	static DWORD LastSelectTick = 0;

	// A deployable unit deploys when you click it while it is selected, which collides with the
	// double-click type-select: the second click would unpack the MCV / GI instead of selecting.
	// So for a short while after a click selected something, the left button does not deploy.
	// Same trick Emperor: Battle for Dune 1.009 uses.
	static constexpr DWORD DeployDelay = 500; // ms, tunable

	// Runs after DecideAction on both left button hooks. Returns true when the action was
	// rewritten, which also means "do not deselect" - the unit stays selected and waits.
	static bool ApplyDeployHoldOff(REGISTERS* R)
	{
		const auto action = static_cast<Action>(R->EAX());

		// The object the click landed on, as ProcessClickCoords resolved it - the same slot the
		// game itself reads at 0x693276 to hand it to the applier. Both left button hooks sit at
		// the same stack depth, so the offset holds for either. Null on empty ground.
		const auto pClicked = R->Stack<ObjectClass*>(0x30);

		// Whether this click (re)selects the clicked unit: either it was not selected yet, or it
		// was part of a bigger selection which now narrows down to it. DecideAction reports that
		// as Select, but also as NoMove or Self_Deploy depending on what sits under the cursor,
		// so the action alone is not a usable signal - go by the clicked object. Getting this
		// wrong leaves the hold-off unarmed and the next click unpacks the unit.
		const bool reselects = pClicked
			&& (!pClicked->IsSelected || ObjectClass::CurrentObjects.Count > 1);

		if (reselects)
			LastSelectTick = GetTickCount();

		if (action != Action::Self_Deploy || !Phobos::Config::TypeSelectByMultiClick)
			return false;

		// A click that reselects is the first click of a possible double click, never a deploy.
		if (reselects)
		{
			R->EAX(static_cast<DWORD>(Action::Select));
			return true;
		}

		if (GetTickCount() - LastSelectTick > DeployDelay)
			return false;

		R->EAX(static_cast<DWORD>(Action::None));
		return true;
	}

	// Mouse flags RadarClass::GetMouseAction (0x6539D0) is called with, in its first stack
	// argument. The "up" bits mean the button is not down, i.e. the cursor is just hovering.
	enum RadarInput : BYTE
	{
		LeftPress = 0x01, LeftHeld = 0x02, LeftRelease = 0x04, LeftUp = 0x08,
		RightPress = 0x10, RightHeld = 0x20, RightRelease = 0x40, RightUp = 0x80,
	};

	// Swap the two buttons for the minimap, keeping the hover bits as they are so the cursor
	// still updates while moving over the radar.
	static BYTE SwapRadarButtons(BYTE flags)
	{
		const BYTE left = flags & (LeftPress | LeftHeld | LeftRelease);
		const BYTE right = flags & (RightPress | RightHeld | RightRelease);

		return static_cast<BYTE>((flags & (LeftUp | RightUp)) | (left << 4) | (right >> 4));
	}
}

// Injected inside the RBUTTONUP handler, just past its "press/drag in progress" gate
// (cmp [this+0x555A],bl / je 0x693408 at 0x69338F) so we inherit the same gate the vanilla
// deselect uses. Stolen bytes: cmp byte ptr [0x884D40], bl (absolute operand, safe to
// relocate).
//
// The LMB-up dispatch we jump into (0x69323E) reads the click point as &[esp+0x10], i.e. the
// VIEW-RELATIVE coords (raw window xy minus the tactical view origin at 0x886FA0/0x886FA4).
// The LMB prologue stores those at [esp+0x10]/[esp+0x14]; the RMB prologue computes the same
// values but only passes them to 0x63AB00 without storing them, so we populate the slots
// ourselves before jumping (otherwise ProcessClickCoords reads stale stack -> wrong cell).
DEFINE_HOOK(0x693397, TacticalMsgHandler_RButtonUp_RightClickCommand, 0x6)
{
	if (Phobos::Config::RightClickCommand
		&& ObjectClass::CurrentObjects.Count > 0
		&& !RightClickCommand::InSpecialLeftClickMode())
	{
		// Raw packed window xy stashed by the RMB prologue at [esp+0x34].
		const int packed = R->Stack<int>(0x34);
		const int rawX = static_cast<short>(packed & 0xFFFF);
		const int rawY = static_cast<short>((packed >> 16) & 0xFFFF);

		const int originX = *reinterpret_cast<int*>(0x886FA0);
		const int originY = *reinterpret_cast<int*>(0x886FA4);

		// Feed the LMB-up command dispatch the view-relative click point.
		R->Stack(0x10, rawX - originX);
		R->Stack(0x14, rawY - originY);

		// Tell the downstream LBUTTONUP / multi-click hooks to leave this RMB order alone.
		RightClickCommand::RmbCommandInProgress = true;

		// Issue the order to the current selection instead of deselecting.
		return 0x69323E;
	}

	// Vanilla: run the stolen compare and fall through to the cancel/deselect path.
	return 0;
}

// LBUTTONDOWN: neutralise a command action right after DecideAction so button-down does not
// preview/issue a command. Stolen bytes: mov reg,[esp+..] + push eax (the possibly-modified
// EAX is what the following push forwards to the applier).
DEFINE_HOOK(0x6931B4, TacticalMsgHandler_LButtonDown_RightClickSelectOnly, 0x5)
{
	if (!RightClickCommand::ApplyDeployHoldOff(R))
		RightClickCommand::NeutraliseLeftCommand(R);

	return 0;
}

// The minimap needs the same treatment. RadarClass::GetMouseAction (0x6539D0) decides what a
// click on the radar does, and vanilla already commands from there: the LEFT button runs the
// same applier the tactical view uses (0x4AB9B0, called at 0x653D58) and only moves the view
// when there is nothing to command, while the RIGHT button just moves the view. The whole
// function reads the buttons out of its flags argument, so swapping the button bits there
// once turns it around: right commands, left moves the view.
//
// Hooked right at the top, before the first read of the argument. Stolen bytes: mov dl,
// byte ptr [esp+0x48] (the flags we just rewrote) + push ebx.
DEFINE_HOOK(0x6539D3, RadarClass_GetMouseAction_RightClickCommand, 0x5)
{
	if (Phobos::Config::RightClickCommand && !RightClickCommand::InSpecialLeftClickMode())
		R->Stack8(0x48, RightClickCommand::SwapRadarButtons(R->Stack8(0x48)));

	return 0;
}

// LBUTTONUP: same neutralise, and turn a would-be command on empty ground / an enemy into a
// deselect (MapClass::UnselectAll). Clicking own units (Select/ToggleSelect) or deploying
// (Self_Deploy) is not neutralised, so it selects/deploys as normal - no deselect there.
// Completed band-selects never reach here (0x63A8E0 consumes them and early-exits at
// 0x693408), so deselecting on a neutralised command is always correct.
DEFINE_HOOK(0x693276, TacticalMsgHandler_LButtonUp_RightClickSelectOnly, 0x5)
{
	// If we arrived here via the RMB command redirect (0x69323E), let the order stand. The
	// flag is cleared LATER, in the post-apply multi-click hook at 0x693290, which this RMB
	// path also flows through - so do NOT clear it here.
	if (RightClickCommand::RmbCommandInProgress)
		return 0;

	// A held-off deploy leaves the unit selected, so no deselect here.
	if (RightClickCommand::ApplyDeployHoldOff(R))
		return 0;

	if (RightClickCommand::NeutraliseLeftCommand(R))
		RightClickCommand::DeselectAll();

	return 0;
}
