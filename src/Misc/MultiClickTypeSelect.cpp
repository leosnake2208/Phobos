#include <Phobos.h>
#include <Utilities/Macro.h>

#include <ObjectClass.h>
#include <FootClass.h>
#include <TacticalClass.h>
#include <GeneralDefinitions.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>

// Multi-click type-select: double-click a unit -> select every unit of the same selection
// group within a pixel radius of it (a local cluster / one base); triple-click -> the whole
// map. RA2 has no native equivalent (double-clicks in the tactical area go to the jump-table
// default exit), so we detect the click streak ourselves with Win32 timing/position and
// expand the freshly-clicked selection. Enable with [Phobos] -> TypeSelectByMultiClick.
//
// Hooked at the post-apply point of the single-click LMB-up dispatch (0x693290), which the
// right-click-to-command redirect (RightClickCommand.cpp) ALSO flows through. That is the
// last point the RMB event passes, so this hook consumes RightClickCommand::RmbCommandInProgress
// and skips - both so the RMB order is not miscounted as a left-click for the streak, and to
// clear the shared flag exactly once.

namespace RightClickCommand
{
	// Defined in RightClickCommand.cpp. Set by the RMB command redirect; cleared here.
	extern bool RmbCommandInProgress;
}

namespace MultiClickTypeSelect
{
	static DWORD LastClickTick = 0;
	static POINT LastClickPos = { -9999, -9999 };
	static int ClickStreak = 0;

	// Double-click grabs same-group units within this pixel radius of the clicked unit (a
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
	// Also the exit point of the RMB command redirect - consume the shared flag here and skip
	// (do not count the RMB event as a left-click for the streak).
	if (RightClickCommand::RmbCommandInProgress)
	{
		RightClickCommand::RmbCommandInProgress = false;
		return 0;
	}

	if (Phobos::Config::TypeSelectByMultiClick)
	{
		POINT pos { 0, 0 };
		GetCursorPos(&pos);
		const DWORD now = GetTickCount();
		const int dx = pos.x - MultiClickTypeSelect::LastClickPos.x;
		const int dy = pos.y - MultiClickTypeSelect::LastClickPos.y;

		if (now - MultiClickTypeSelect::LastClickTick <= GetDoubleClickTime()
			&& dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4)
		{
			if (MultiClickTypeSelect::ClickStreak < 3)
				++MultiClickTypeSelect::ClickStreak;
		}
		else
		{
			MultiClickTypeSelect::ClickStreak = 1;
		}

		MultiClickTypeSelect::LastClickTick = now;
		MultiClickTypeSelect::LastClickPos = pos;

		if (MultiClickTypeSelect::ClickStreak == 2)
			MultiClickTypeSelect::SelectSameType(false); // same group near the clicked unit
		else if (MultiClickTypeSelect::ClickStreak == 3)
			MultiClickTypeSelect::SelectSameType(true);  // same group across the whole map
	}

	return 0;
}
