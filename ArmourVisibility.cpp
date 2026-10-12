// ArmourVisibility - an RE_Kenshi / KenshiLib plugin for Kenshi
//
// Adds a per-character, per-slot visibility toggle for worn armour.
// Hidden armour stays equipped and keeps all of its stats; the plugin only
// stops the armour's mesh from being attached to the character model.
//
// How it works
//  * AppearanceBase::attachItem (both overloads) is hooked. When the game tries
//    to attach an item in a slot the player has hidden for that character, the
//    call is skipped.
//  * The arguments of every attach call are recorded, so a hidden item can be
//    re-attached exactly as the game did it when the player shows it again.
//  * Toggling from the UI queues a request. Requests are applied inside
//    GameWorld::mainLoop_GPUSensitiveStuff, the same place the official
//    Character Highlight example touches rendering state.
//  * Hidden slots are saved next to each save game (ArmourVisibility.txt) and
//    read back when that save is loaded.
//
// UI: each equipment slot in a squad member's inventory window gets a tick box
//     in its top-right corner. Ticked = shown, unticked = hidden.
//
// License: GPLv3 (KenshiLib is GPLv3, so plugins built on it must be too).

#define NOMINMAX

#include <Debug.h>
#include <core/Functions.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/Appearance.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/Enums.h>
#include <kenshi/SaveManager.h>
#include <kenshi/gui/InventoryGUI.h>
#include <kenshi/util/hand.h>
#include <kenshi/util/lektor.h>

#include <mygui/MyGUI_Widget.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Delegate.h>

#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// File written into each save folder.
static const char* STATE_FILE_NAME = "ArmourVisibility.txt";

// What a skipped attachItem call returns to the game. "false" is the value the
// game already sees when an item has no mesh for a race, so it is the
// well-tested path. Change to true only if hidden items cause odd behaviour.
static const bool BLOCKED_ATTACH_RETURN = false;

// After a toggle, ask human appearances to recompute parts hidden by clothing
// (hair under helmets etc.). Set to 0 if it causes problems.
#define AV_REFRESH_HIDDEN_PARTS 1

// ---------------------------------------------------------------------------
// Slots the player can toggle
// ---------------------------------------------------------------------------

struct SlotInfo
{
	AttachSlot slot;
	const char* label;
};

static const SlotInfo SLOTS[] = {
	{ ATTACH_HAT,      "Head" },
	{ ATTACH_SHIRT,    "Shirt" },
	{ ATTACH_BODY,     "Body armour" },
	{ ATTACH_LEGS,     "Legs" },
	{ ATTACH_BOOTS,    "Boots" },
	{ ATTACH_BELT,     "Belt" },
	{ ATTACH_BACKPACK, "Backpack" }
};
static const int NUM_SLOTS = sizeof(SLOTS) / sizeof(SLOTS[0]);

static bool IsManagedSlot(AttachSlot slot)
{
	for (int i = 0; i < NUM_SLOTS; ++i)
		if (SLOTS[i].slot == slot)
			return true;
	return false;
}

// ---------------------------------------------------------------------------
// Shared state (guarded by g_lock)
// ---------------------------------------------------------------------------

struct AttachRecord
{
	bool hasMesh;
	std::string mesh;
	std::string slot;
};

struct ToggleRequest
{
	hand character;
	AttachSlot slot;
	bool hide;
};

static boost::mutex g_lock;
// Keys are "<character handle string>\t<slot number>".
static boost::unordered_set<std::string> g_hidden;
// Last attachItem arguments seen for each item, used to re-show it.
static boost::unordered_map<Item*, AttachRecord> g_attachRecords;
// Toggles waiting to be applied on the render-safe main loop.
static std::vector<ToggleRequest> g_pending;

static std::string MakeKey(const std::string& handleString, int slot)
{
	std::ostringstream out;
	out << handleString << '\t' << slot;
	return out.str();
}

static std::string CharacterKey(Character* character, AttachSlot slot)
{
	return MakeKey(character->getHandle().toString(), (int)slot);
}

static bool IsHidden(Character* character, AttachSlot slot)
{
	if (!character || !IsManagedSlot(slot))
		return false;
	std::string key = CharacterKey(character, slot);
	boost::lock_guard<boost::mutex> lock(g_lock);
	return g_hidden.find(key) != g_hidden.end();
}

// Changes the stored state and queues the visual change.
static void SetSlotHidden(Character* character, AttachSlot slot, bool hidden)
{
	if (!character)
		return;
	std::string key = CharacterKey(character, slot);

	boost::lock_guard<boost::mutex> lock(g_lock);
	bool currentlyHidden = g_hidden.find(key) != g_hidden.end();
	if (currentlyHidden == hidden)
		return;

	if (hidden)
		g_hidden.insert(key);
	else
		g_hidden.erase(key);

	ToggleRequest request;
	request.character = character->getHandle();
	request.slot = slot;
	request.hide = hidden;
	g_pending.push_back(request);
}

static void ClearAllState()
{
	boost::lock_guard<boost::mutex> lock(g_lock);
	g_hidden.clear();
	g_attachRecords.clear();
	g_pending.clear();
}

// ---------------------------------------------------------------------------
// Hooks: AppearanceBase::attachItem
// ---------------------------------------------------------------------------

typedef bool (AppearanceBase::*AttachItem2Fn)(Item*, const std::string&);
typedef bool (AppearanceBase::*AttachItem3Fn)(Item*, const std::string&, const std::string&);

static bool (*AttachItem2_orig)(AppearanceBase*, Item*, const std::string&) = NULL;
static bool (*AttachItem3_orig)(AppearanceBase*, Item*, const std::string&, const std::string&) = NULL;

// One overload may call the other; only the outermost call is recorded/filtered.
static __declspec(thread) int t_attachDepth = 0;

static void RecordAttach(Item* item, bool hasMesh, const std::string& mesh, const std::string& slot)
{
	if (!item || !IsManagedSlot(item->slotType))
		return;
	AttachRecord record;
	record.hasMesh = hasMesh;
	record.mesh = mesh;
	record.slot = slot;
	boost::lock_guard<boost::mutex> lock(g_lock);
	g_attachRecords[item] = record;
}

static bool ShouldBlockAttach(AppearanceBase* appearance, Item* item)
{
	if (!appearance || !item || !appearance->me)
		return false;
	return IsHidden(appearance->me, item->slotType);
}

static bool AttachItem2_hook(AppearanceBase* self, Item* item, const std::string& slot)
{
	if (t_attachDepth == 0)
	{
		RecordAttach(item, false, std::string(), slot);
		if (ShouldBlockAttach(self, item))
			return BLOCKED_ATTACH_RETURN;
	}
	++t_attachDepth;
	bool result = AttachItem2_orig(self, item, slot);
	--t_attachDepth;
	return result;
}

static bool AttachItem3_hook(AppearanceBase* self, Item* item, const std::string& mesh, const std::string& slot)
{
	if (t_attachDepth == 0)
	{
		RecordAttach(item, true, mesh, slot);
		if (ShouldBlockAttach(self, item))
			return BLOCKED_ATTACH_RETURN;
	}
	++t_attachDepth;
	bool result = AttachItem3_orig(self, item, mesh, slot);
	--t_attachDepth;
	return result;
}

// ---------------------------------------------------------------------------
// Applying toggles (runs inside GameWorld::mainLoop_GPUSensitiveStuff)
// ---------------------------------------------------------------------------

static void CollectEquippedInSlot(Character* character, AttachSlot slot, std::vector<Item*>& out)
{
	Inventory* inventory = character->getInventory();
	if (!inventory)
		return;

	const lektor<Item*>& all = inventory->getAllItems();
	for (uint32_t i = 0; i < all.size(); ++i)
	{
		Item* item = all[i];
		if (item && item->isEquipped && item->slotType == slot)
			out.push_back(item);
	}

	// Fallback in case equipped armour is not part of getAllItems().
	lektor<Item*> armour;
	inventory->getEquippedArmour(armour);
	for (uint32_t i = 0; i < armour.size(); ++i)
	{
		Item* item = armour[i];
		if (!item || item->slotType != slot)
			continue;
		bool already = false;
		for (size_t j = 0; j < out.size(); ++j)
			if (out[j] == item)
				already = true;
		if (!already)
			out.push_back(item);
	}
}

static void ApplyToggle(const ToggleRequest& request)
{
	Character* character = request.character.getCharacter();
	if (!character)
		return;
	AppearanceBase* appearance = character->getAppearance();
	if (!appearance)
		return;

	std::vector<Item*> items;
	CollectEquippedInSlot(character, request.slot, items);

	bool changed = false;
	for (size_t i = 0; i < items.size(); ++i)
	{
		Item* item = items[i];
		if (request.hide)
		{
			if (appearance->detachItem(item))
				changed = true;
		}
		else
		{
			AttachRecord record;
			bool found = false;
			{
				boost::lock_guard<boost::mutex> lock(g_lock);
				boost::unordered_map<Item*, AttachRecord>::iterator it = g_attachRecords.find(item);
				if (it != g_attachRecords.end())
				{
					record = it->second;
					found = true;
				}
			}

			if (found)
			{
				if (record.hasMesh)
					appearance->attachItem(item, record.mesh, record.slot);
				else
					appearance->attachItem(item, record.slot);
				changed = true;
			}
			else
			{
				DebugLog("ArmourVisibility: no attach record for an item; re-equip it to show it.");
			}
		}
	}

	if (!changed)
		return;

	appearance->notifyDirty();
#if AV_REFRESH_HIDDEN_PARTS
	if (!character->isAnimal())
	{
		AppearanceHuman* human = static_cast<AppearanceHuman*>(appearance);
		human->setHiddenPartsEnabled(human->hiddenPartsEnabled);
	}
#endif
	appearance->updatePortrait();
}

static void (*MainLoopGPU_orig)(GameWorld*, float) = NULL;
static void MainLoopGPU_hook(GameWorld* self, float time)
{
	std::vector<ToggleRequest> work;
	{
		boost::lock_guard<boost::mutex> lock(g_lock);
		work.swap(g_pending);
	}
	for (size_t i = 0; i < work.size(); ++i)
		ApplyToggle(work[i]);

	MainLoopGPU_orig(self, time);
}

// ---------------------------------------------------------------------------
// Persistence: a small text file inside each save folder
// ---------------------------------------------------------------------------

static std::wstring ToWide(const std::string& s)
{
	if (s.empty())
		return std::wstring();
	UINT codePage = CP_UTF8;
	int len = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), NULL, 0);
	if (len <= 0)
	{
		codePage = CP_ACP;
		len = MultiByteToWideChar(codePage, 0, s.c_str(), (int)s.size(), NULL, 0);
	}
	if (len <= 0)
		return std::wstring();
	std::vector<wchar_t> buffer(len);
	MultiByteToWideChar(codePage, 0, s.c_str(), (int)s.size(), &buffer[0], len);
	return std::wstring(&buffer[0], len);
}

static std::string JoinPath(const std::string& a, const std::string& b)
{
	if (a.empty())
		return b;
	char last = a[a.size() - 1];
	if (last == '/' || last == '\\')
		return a + b;
	return a + "/" + b;
}

// Primary: <location>/<save name>/ArmourVisibility.txt
static std::string PrimaryStatePath(const std::string& location, const std::string& name)
{
	return JoinPath(JoinPath(location, name), STATE_FILE_NAME);
}

// Fallback: <location>/<save name>_ArmourVisibility.txt
static std::string FallbackStatePath(const std::string& location, const std::string& name)
{
	return JoinPath(location, name + "_" + STATE_FILE_NAME);
}

static void WriteStateFile(const std::string& location, const std::string& name)
{
	std::vector<std::string> lines;
	{
		boost::lock_guard<boost::mutex> lock(g_lock);
		for (boost::unordered_set<std::string>::const_iterator it = g_hidden.begin(); it != g_hidden.end(); ++it)
			lines.push_back(*it);
	}

	std::string path = PrimaryStatePath(location, name);
	std::ofstream out(ToWide(path).c_str(), std::ios::out | std::ios::trunc);
	if (!out.is_open())
	{
		path = FallbackStatePath(location, name);
		out.open(ToWide(path).c_str(), std::ios::out | std::ios::trunc);
	}
	if (!out.is_open())
	{
		ErrorLog("ArmourVisibility: could not write " + path);
		return;
	}

	out << "# ArmourVisibility v1: <character handle>\\t<slot>\n";
	for (size_t i = 0; i < lines.size(); ++i)
		out << lines[i] << '\n';
}

static void ReadStateFile(const std::string& location, const std::string& name)
{
	ClearAllState();

	std::ifstream in(ToWide(PrimaryStatePath(location, name)).c_str());
	if (!in.is_open())
		in.open(ToWide(FallbackStatePath(location, name)).c_str());
	if (!in.is_open())
		return; // nothing hidden in this save

	boost::lock_guard<boost::mutex> lock(g_lock);
	std::string line;
	while (std::getline(in, line))
	{
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);
		if (line.empty() || line[0] == '#')
			continue;
		size_t tab = line.rfind('\t');
		if (tab == std::string::npos || tab == 0)
			continue;
		int slot = std::atoi(line.c_str() + tab + 1);
		if (!IsManagedSlot((AttachSlot)slot))
			continue;
		g_hidden.insert(MakeKey(line.substr(0, tab), slot));
	}
}

static int (*SaveGame_orig)(SaveManager*, const std::string&, const std::string&) = NULL;
static int SaveGame_hook(SaveManager* self, const std::string& location, const std::string& name)
{
	int result = SaveGame_orig(self, location, name);
	WriteStateFile(location, name);
	return result;
}

static int (*LoadGame_orig)(SaveManager*, const std::string&, const std::string&) = NULL;
static int LoadGame_hook(SaveManager* self, const std::string& location, const std::string& name)
{
	// Read before the game builds characters, so hidden slots apply immediately.
	ReadStateFile(location, name);
	return LoadGame_orig(self, location, name);
}

static int (*ImportGame_orig)(SaveManager*, const std::string&, const std::string&, int) = NULL;
static int ImportGame_hook(SaveManager* self, const std::string& location, const std::string& name, int flags)
{
	// Imported characters get new handles, so old entries would not match.
	ClearAllState();
	return ImportGame_orig(self, location, name, flags);
}

static void (*NewGame_orig)(SaveManager*, const std::string&) = NULL;
static void NewGame_hook(SaveManager* self, const std::string& startId)
{
	ClearAllState();
	NewGame_orig(self, startId);
}

// ---------------------------------------------------------------------------
// UI: a tick box on each equipment slot of a character's inventory window.
// Ticked = armour in that slot is shown, unticked = hidden.
// Everything here runs on the UI thread.
// ---------------------------------------------------------------------------

// Tick skin used by RE_Kenshi's own settings menu.
static const char* TICK_SKIN = "Kenshi_TickBoxSkin";
static const int TICK_SIZE = 18;   // pixels
static const int TICK_MARGIN = 2;  // pixels from the slot's top-right corner

struct SlotTick
{
	MyGUI::Button* button;
	MyGUI::Widget* sectionWidget;
	hand owner;
	AttachSlot slot;
};

static std::string PointerString(const void* p)
{
	std::ostringstream out;
	out << p;
	return out.str();
}

static void PlaceTick(MyGUI::Button* button, MyGUI::Widget* sectionWidget)
{
	const MyGUI::IntCoord& c = sectionWidget->getCoord();
	button->setCoord(c.left + c.width - TICK_SIZE - TICK_MARGIN, c.top + TICK_MARGIN, TICK_SIZE, TICK_SIZE);
}

static void SyncTick(const SlotTick& tick)
{
	Character* character = tick.owner.getCharacter();
	bool show = character != NULL && character->isPlayerCharacter();
	tick.button->setVisible(show);
	if (!show)
		return;
	tick.button->setStateSelected(!IsHidden(character, tick.slot));
	PlaceTick(tick.button, tick.sectionWidget);
}

static void OnTickClick(MyGUI::WidgetPtr sender)
{
	MyGUI::Button* button = sender->castType<MyGUI::Button>(false);
	if (!button)
		return;

	hand owner;
	owner.fromString(button->getUserString("avOwner"));
	Character* character = owner.getCharacter();
	if (!character || !character->isPlayerCharacter())
		return;

	AttachSlot slot = (AttachSlot)std::atoi(button->getUserString("avSlot").c_str());
	bool shown = !button->getStateSelected();
	button->setStateSelected(shown);
	SetSlotHidden(character, slot, !shown);
}

typedef std::map<std::string, InventorySectionGUI*, std::less<std::string>,
	Ogre::STLAllocator<std::pair<const std::string, InventorySectionGUI*>, Ogre::GeneralAllocPolicy> > SectionGUIMap;

// CharacterInventoryLayout::setupSections has a mangled name too long for
// KenshiLib's assembler, so KenshiLib exports it under this placeholder name
// instead (see KenshiLib Source/kenshi/functions/CharacterInventoryLayout.inc).
// It must be dllimport so its address is the stub inside KenshiLib.dll, which
// is what KenshiLib::GetRealAddress expects.
extern "C" __declspec(dllimport) void CharacterInventoryLayout_setupSections_PLACEHOLDER();

static void (*SetupSections_orig)(CharacterInventoryLayout*, InventoryGUI*, SectionGUIMap&, Inventory*) = NULL;
static void SetupSections_hook(CharacterInventoryLayout* self, InventoryGUI* gui, SectionGUIMap& sections, Inventory* inventory)
{
	SetupSections_orig(self, gui, sections, inventory);

	if (!gui || !inventory || !inventory->getOwner())
		return;

	hand owner = inventory->getOwner()->getHandle();

	for (SectionGUIMap::iterator it = sections.begin(); it != sections.end(); ++it)
	{
		InventorySection* section = inventory->getSection(it->first);
		InventorySectionGUI* sectionGUI = it->second;
		if (!section || !sectionGUI || !section->isAnEquippedItemSection)
			continue;
		AttachSlot slot = section->getLimitedSlot();
		if (!IsManagedSlot(slot))
			continue;

		MyGUI::Widget* sectionWidget = sectionGUI->getWidget();
		MyGUI::Widget* parent = sectionWidget ? sectionWidget->getParent() : NULL;
		if (!parent)
			continue;

		// Reuse the tick if this window's sections are set up more than once.
		std::string name = "ArmourVisibilityTick_" + PointerString(gui) + "_" + it->first;
		MyGUI::Button* button = NULL;
		MyGUI::Widget* existing = parent->findWidget(name);
		if (existing)
			button = existing->castType<MyGUI::Button>(false);
		if (!button)
		{
			button = parent->createWidget<MyGUI::Button>(TICK_SKIN, 0, 0, TICK_SIZE, TICK_SIZE,
				MyGUI::Align::Default, name);
			button->eventMouseButtonClick += MyGUI::newDelegate(OnTickClick);
		}

		std::ostringstream slotText;
		slotText << (int)slot;
		button->setUserString("avOwner", owner.toString());
		button->setUserString("avSlot", slotText.str());

		SlotTick tick;
		tick.button = button;
		tick.sectionWidget = sectionWidget;
		tick.owner = owner;
		tick.slot = slot;
		// The widget pointers are only valid while this section layout is live.
		// Never retain them across GUI updates or a vendor window rebuild.
		SyncTick(tick);
	}
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

template<typename T>
static void InstallHook(intptr_t target, void* detour, T** original, const char* name)
{
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(target, detour, original))
		ErrorLog(std::string("ArmourVisibility: could not hook ") + name);
}

__declspec(dllexport) void startPlugin()
{
	InstallHook(KenshiLib::GetRealAddress(static_cast<AttachItem2Fn>(&AppearanceBase::attachItem)),
		(void*)&AttachItem2_hook, &AttachItem2_orig, "AppearanceBase::attachItem(item, slot)");
	InstallHook(KenshiLib::GetRealAddress(static_cast<AttachItem3Fn>(&AppearanceBase::attachItem)),
		(void*)&AttachItem3_hook, &AttachItem3_orig, "AppearanceBase::attachItem(item, mesh, slot)");

	InstallHook(KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
		(void*)&MainLoopGPU_hook, &MainLoopGPU_orig, "GameWorld::mainLoop_GPUSensitiveStuff");

	InstallHook(KenshiLib::GetRealAddress(&SaveManager::saveGame),
		(void*)&SaveGame_hook, &SaveGame_orig, "SaveManager::saveGame");
	InstallHook(KenshiLib::GetRealAddress(&SaveManager::loadGame),
		(void*)&LoadGame_hook, &LoadGame_orig, "SaveManager::loadGame");
	InstallHook(KenshiLib::GetRealAddress(&SaveManager::importGame),
		(void*)&ImportGame_hook, &ImportGame_orig, "SaveManager::importGame");
	InstallHook(KenshiLib::GetRealAddress(&SaveManager::newGame),
		(void*)&NewGame_hook, &NewGame_orig, "SaveManager::newGame");

	InstallHook(KenshiLib::GetRealAddress((void*)&CharacterInventoryLayout_setupSections_PLACEHOLDER),
		(void*)&SetupSections_hook, &SetupSections_orig, "CharacterInventoryLayout::setupSections");

	DebugLog("ArmourVisibility: loaded");
}
