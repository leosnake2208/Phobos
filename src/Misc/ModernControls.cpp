#include <Phobos.h>
#include <Utilities/Macro.h>

#include <MapClass.h>
#include <ObjectClass.h>
#include <DisplayClass.h>
#include <FootClass.h>
#include <TacticalClass.h>
#include <GeneralDefinitions.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>

// Modern control scheme, Stage B1 - right mouse button issues the command.
// (Mirror of the Vinifera "Feature B" work on Tiberian Sun.)
//
// The tactical mouse message handler is MouseClass method 0x6930A0. It dispatches
// Windows mouse messages through a jump table (0x693410). Reversed cases:
//   LBUTTONDOWN 0x201 -> 0x693126  (begin select/action, sets flag [this+0x555A]=1)
//   LBUTTONUP   0x202 -> 0x6931F9  (command dispatch: ProcessClickCoords -> DecideAction
//                                   -> apply 0x4AB9B0, at 0x69323E onward)
//   RBUTTONDOWN 0x204 -> 0x6932A4
//   RBUTTONUP   0x205 -> 0x693366  (vanilla: cancel current mode, then deselect)
//
// RBUTTONUP and LBUTTONUP are cases of the SAME function sharing one stack frame and
// the same `this`; both prologues convert [esp+0x34] from a msg-point pointer into an
// inline Point2D value. So from the RMB-up handler we can jump straight into the LMB-up
// command dispatch at 0x69323E - it runs ProcessClickCoords -> DecideAction -> apply on
// the cursor cell, clears the drag flag and returns, reusing 100% of the game's command
// (and network) logic. No argument reconstruction needed.

namespace ModernControls
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

	// Set by the RBUTTONUP command hook just before it jumps into the shared LMB-up
	// command dispatch (0x69323E). Because that dispatch flows straight through the
	// LBUTTONUP neutralise hook at 0x693276, we must let the RMB-issued command pass
	// through unmodified - otherwise B2 downgrades the RMB order to None and B1 stops
	// working. The LBUTTONUP hook consumes (clears) the flag.
	static bool RmbCommandInProgress = false;

	// Which actions the LEFT button is still allowed to perform in modern controls:
	// selection and self-deploy only. Everything else (Move/Attack/Enter/Harvest/Capture/
	// Guard/...) is a command and belongs to the RIGHT button now, so we neutralise it.
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

	// Shared by the LBUTTONDOWN and LBUTTONUP handlers: if modern controls are on and we
	// aren't in a special left-click mode, downgrade any command action (in EAX, freshly
	// returned by DecideAction) to None so the left button only selects/deploys.
	// Returns true if a command action was actually neutralised (i.e. the click landed on
	// empty ground / an enemy - a command target, not a selectable own unit).
	static bool NeutraliseLeftCommand(REGISTERS* R)
	{
		if (Phobos::Config::ModernControls && !InSpecialLeftClickMode())
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
	// deselect-all (also used by Phobos' NextIdleHarvester command). NOTE: 0x4AEAD0 - which
	// an earlier pass wrongly used here - is NOT deselect; it only resets the cursor/action
	// mode (clears [this+0x11CF/0x11D0], calls vtable+0x48), which is why deselect never
	// actually happened.
	static void DeselectAll()
	{
		MapClass::UnselectAll();
	}
}

// Injected inside the RBUTTONUP handler, just past its "press/drag in progress" gate
// (cmp [this+0x555A],bl / je 0x693408 at 0x69338F) so we inherit the same gate the
// vanilla deselect uses. Stolen bytes: cmp byte ptr [0x884D40], bl (absolute operand,
// safe to relocate).
//
// The LMB-up command dispatch we jump into (0x69323E) reads the click point as
// Point2D* = &[esp+0x10], i.e. the VIEW-RELATIVE coords (raw window xy minus the tactical
// view origin at 0x886FA0/0x886FA4). The LMB prologue computes and stores those at
// [esp+0x10]/[esp+0x14]; the RMB prologue computes the same values but only passes them
// to 0x63AB00 without storing them, so we must populate the slots ourselves before
// jumping (otherwise ProcessClickCoords reads stale stack -> coords fly off to infinity).
DEFINE_HOOK(0x693397, TacticalMsgHandler_RButtonUp_ModernCommand, 0x6)
{
	if (Phobos::Config::ModernControls
		&& ObjectClass::CurrentObjects.Count > 0
		&& !ModernControls::InSpecialLeftClickMode())
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

		// Tell the LBUTTONUP neutralise hook to leave this (RMB-issued) command alone.
		ModernControls::RmbCommandInProgress = true;

		// Issue the order to the current selection instead of deselecting.
		return 0x69323E;
	}

	// Vanilla: run the stolen compare and fall through to the cancel/deselect path.
	return 0;
}

// Stage B2 - the LEFT button only selects/deploys; command actions move to the RIGHT
// button. Both the LBUTTONDOWN and LBUTTONUP handlers call DecideAction and then hand the
// result (in EAX) to the appliers (0x4AAE90 / 0x4AB9B0). We intercept right after each
// DecideAction and downgrade command actions to None. Stolen bytes at each site are
// `mov reg,[esp+..]` + `push eax` (5 bytes); the (possibly modified) EAX is what the
// following `push eax` forwards to the applier.
DEFINE_HOOK(0x6931B4, TacticalMsgHandler_LButtonDown_ModernSelectOnly, 0x5)
{
	ModernControls::NeutraliseLeftCommand(R);
	return 0;
}

// Multi-click type-select: double-click a unit -> select every unit of the same type on
// screen; triple-click -> the whole map. RA2 has no native equivalent (double-clicks in the
// tactical area go to the jump-table default exit), so we detect the click streak ourselves
// with Win32 timing/position and expand the freshly-clicked selection.
namespace ModernControls
{
	static DWORD LastClickTick = 0;
	static POINT LastClickPos = { -9999, -9999 };
	static int ClickStreak = 0;

	// Double-click grabs same-type units within this pixel radius of the clicked unit (a
	// local cluster / one base), NOT the whole screen. Triple-click ignores it (whole map).
	// Tunable.
	static constexpr int SameTypeScreenRadius = 250;

	// Add to the current selection every own, alive, selectable mobile unit that shares the
	// selection group of the just-clicked unit (CurrentObjects[0]) - reusing Phobos'
	// GetSelectionGroupID / HasSelectionGroupID (the GroupAs tag with the type ID as
	// fallback), so grouping stays consistent with the vanilla type-select hotkey.
	// wholeMap=false limits it to a SameTypeScreenRadius-px circle around the clicked unit.
	static void SelectSameType(bool wholeMap)
	{
		if (ObjectClass::CurrentObjects.Count < 1)
			return;

		auto const pClicked = abstract_cast<FootClass*>(ObjectClass::CurrentObjects.GetItem(0));
		if (!pClicked)
			return; // only mobile units get type-select

		auto const pType = pClicked->GetTechnoType();
		auto const pOwner = pClicked->Owner;
		if (!pType)
			return;

		const char* groupID = TechnoTypeExt::GetSelectionGroupID(pType);

		Point2D center {};
		if (!wholeMap)
			center = TacticalClass::Instance->CoordsToClient(pClicked->GetCoords()).first;

		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pFoot = abstract_cast<FootClass*>(TechnoClass::Array.GetItem(i));
			if (!pFoot || pFoot->IsSelected)
				continue;
			if (pFoot->Owner != pOwner || !TechnoTypeExt::HasSelectionGroupID(pFoot->GetTechnoType(), groupID))
				continue;
			if (!pFoot->IsAlive || pFoot->InLimbo || !pFoot->IsSelectable())
				continue;

			if (!wholeMap)
			{
				auto const client = TacticalClass::Instance->CoordsToClient(pFoot->GetCoords());
				const int dx = client.first.X - center.X;
				const int dy = client.first.Y - center.Y;
				if (dx * dx + dy * dy > SameTypeScreenRadius * SameTypeScreenRadius)
					continue;
			}

			pFoot->Select();
		}
	}
}

// Fires after the click's normal selection has been applied (0x4AB9B0), on the single-click
// dispatch path only - completed band-selects exit earlier at 0x693408. Stolen bytes:
// mov byte ptr [esi+0x555A], bl (absolute operand, safe to relocate).
DEFINE_HOOK(0x693290, TacticalMsgHandler_LButtonUp_MultiClickTypeSelect, 0x6)
{
	// Also the exit point of the RMB command redirect - consume the flag here and skip.
	if (ModernControls::RmbCommandInProgress)
	{
		ModernControls::RmbCommandInProgress = false;
		return 0;
	}

	if (Phobos::Config::ModernControls)
	{
		POINT pos { 0, 0 };
		GetCursorPos(&pos);
		const DWORD now = GetTickCount();
		const int dx = pos.x - ModernControls::LastClickPos.x;
		const int dy = pos.y - ModernControls::LastClickPos.y;

		if (now - ModernControls::LastClickTick <= GetDoubleClickTime()
			&& dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4)
		{
			if (ModernControls::ClickStreak < 3)
				++ModernControls::ClickStreak;
		}
		else
		{
			ModernControls::ClickStreak = 1;
		}

		ModernControls::LastClickTick = now;
		ModernControls::LastClickPos = pos;

		if (ModernControls::ClickStreak == 2)
			ModernControls::SelectSameType(false); // same type on screen
		else if (ModernControls::ClickStreak == 3)
			ModernControls::SelectSameType(true);  // same type across the whole map
	}

	return 0;
}

DEFINE_HOOK(0x693276, TacticalMsgHandler_LButtonUp_ModernSelectOnly, 0x5)
{
	// If we arrived here via the RMB command redirect (0x69323E), let the order stand.
	// (The flag is cleared later, in the post-apply multi-click hook at 0x693290, which
	// this RMB path also flows through - so that hook can likewise skip the RMB event.)
	if (ModernControls::RmbCommandInProgress)
		return 0;

	// A genuine single left-click (or a band-drag that selected nothing). If it would have
	// been a command (empty ground / enemy), modern controls turn it into "deselect"
	// instead: neutralise the command and clear the selection. Clicking own units
	// (Select/ToggleSelect) or deploying (Self_Deploy) is not neutralised, so it
	// selects/deploys as normal - no deselect there. Completed band-selects that grabbed
	// units never reach here (0x63A8E0 consumes them and early-exits at 0x693408), so no
	// extra guard is needed - deselecting on a neutralised command is always correct.
	if (ModernControls::NeutraliseLeftCommand(R))
		ModernControls::DeselectAll();

	return 0;
}
