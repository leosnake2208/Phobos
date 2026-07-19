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
	RightClickCommand::NeutraliseLeftCommand(R);
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

	if (RightClickCommand::NeutraliseLeftCommand(R))
		RightClickCommand::DeselectAll();

	return 0;
}
