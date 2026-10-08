# Game UI (UMG): widgets, styles, how to add ours

Features that extend a game screen (inventory, bank, character, options …⊇) reuse the widgets and styles on this page
(rule: `.claude/rules/design.md`). Values below were read from the running game with `just ui` (2026-10-08, game build of
the current SDK). Where a value came from a class's designer template rather than an open screen, it says so.

## Look it up yourself
```bash
just ui WidgetitemBagHeaderMenu WidgetButton01   # live trees + designer templates of matching classes -> dos-tool-ui.yaml (path printed)
```
- `features/ui-probe` (Alpha, dev installs). Request words match class names. Designer templates (`- class:`) work while the
  class is loaded, with no screen open. Live trees (`- root:`) exist only while that screen is constructed. A request with
  no match stays pending until you open the screen in game.
- Per widget the dump shows: class, visibility, slot (box padding / fill size / alignment, canvas anchors), button brushes
  (texture, draw type, 9-slice margin, tint) and paddings, text font/size/colour/shadow, image and border brushes, size boxes.
  Per class it also shows property bindings (`Widget.Property <- Function`) and named slots.
- `outer WidgetTree … archetype` = a class template, not a live screen. Never modify one (flags & 0x30).

## Art direction (screenshots + dump)
```yaml
frames: dark slate stone panels with ornate metal corners (PanelBorder01_*, WidgetPanel01a_C), parchment for some panels (ParchmentPanel_*)
font: Narkisim (UE font asset, typeface Default) for all UI text; Roboto only for symbols (○ ● checks, size 40)
text: [0.937, 0.937, 0.937] on buttons; white 1.0 in panel titles/tooltips; drop shadow offset (1,1) at alpha 0
accent_orange: [1.0, 0.651, 0.270]   # spinner values + arrows; Button05_MouseOver art reads orange (screenshot)
highlight_yellow: [0.989, 1.0, 0.0]  # counts, filter-active marks
rarity_frames: item slots framed green / blue / magenta by grade (screenshot); exact values: BP_ArchonClientFunctionLibrary_C::GetItemColorForGrade (unread)
sizes: header row 60 px tall; button text 18; filter/grade buttons 24; spinner 20; panel title 20; tooltip 10   # font sizes in UMG units
```

## Reusable widgets
| class | is | style (designer template) | API |
|---|---|---|---|
| `WidgetButton01_C` | the game's generic button | SizeBox min 100×60 → Button (Button05_Base / _MouseOver / _Selected / _Disabled, Box 9-slice margin 0.2, padding 2 / pressed 2,3,2,1) + TextBlock `Text` Narkisim 18 white | `SetButtonText(FText)`; click fires `BndEvt__WidgetButton01_Button_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature` on the instance (ProcessEvent: detectable by a listener), then broadcasts `HandleClicked` |
| plain `UButton` in game screens | header buttons (Sort, Select) | same Button05 textures, DrawAs Image, padding 2 / 2,3,2,1; child TextBlock Narkisim 18 colour 0.937 | `WidgetStyle` (FButtonStyle) copyable by value before the Slate widget exists |
| `WidgetSpinner_C` | ◀ value ▶ selector (options menus) | SizeBox min 160×40; arrows PageArrow01 18×24 tinted orange; value Narkisim 20 orange | `kValues` TArray<FText> (game-allocated: not fillable from our heap), `SpinnerSet`, `OnValueChanged` |
| `WidgetSelectItemOptionGradeButton_C` / `…TypeButton_C` | filter toggles in the bag's Select menu | Button04 art (452×169), label Narkisim 24 0.937, status mark Roboto 40 yellow | the game's own grade/type filter |
| `WidgetItemBagTabButton_C` | bag tab | Inventory_Box (tint 0.5) / Inventory_Box_Selected | — |
| `WidgetPanel01a_C` | framed panel with title bar | PanelBorder01 corners/edges, TitleBar 68 px, title Narkisim 20, content `Slot_User` (named slot) | `Text_Title` |
| `WidgetPanel02_C` / `02a` / `03` | inner panels / backgrounds | ParchmentPanel_* / ItemBackgroundPanel01 | — |
| `WidgetTooltip_C` | tooltip | Border `Tooltip` texture (Box, margin 0.2), padding 4,5,4,4, Narkisim 10 white, min 100×22 | — |
| `WidgetConfirmation_C`, `WidgetMultiConfirmation_C` | yes/no dialogs | — | not surveyed |

## Screens
### Bag header (`WidgetitemBagHeaderMenu_C`, inventory and bank; `IsStorage` = bank)
```yaml
SizeBox_2: {h: 60}
  HorizontalBox_1:                       # children in order; fill sizes
    Button_Sort: {fill: 0.5}            # BndEvt__Button_Sort_*; label TextBlock_1 "Sort"
    Button_SelectItemOption: {fill: 0.5, visibility: GetVisibility_0}   # opens FilterMenu (grade/type filter)
    Button_SpecialOption0: {fill: 1.0, visibility: Get_Button_SpecialOption0_Visibility_0, enabled: Get_Button_DestroyItems_bIsEnabled_0}  # = DESTROY ITEMS in select mode: never reuse
    Button_SelectMode: {collapsed}
    UserSlotLeft: {NamedSlot, collapsed, fill: 1.0}
```
- Item sort adds a `WidgetButton01_C` right after Button_Sort (features/item-sort/inventory-ui.cpp).
- Live trees of the inventory, bank, character and options screens: not dumped yet (open the screen, then `just ui WidgetInventoryMenu WidgetStorageMenu WidgeCharacterMenu WidgetInGameMenu`).

### Item slot (`WidgetItemIconContainer_C`, designer template)
```yaml
SizeBox_0: {min: 100x100}
  ScaleBox_3 > Overlay_Container:                 # children bottom to top
    BackgroundSlot: {NamedSlot, pad: 8}
    Image_BackShadow: ItemIconContainer_Back (Box 0.2)
    Border_IconArea: {pad: 4, vis: Hidden}        # the item icon (WidgetItemIcon_C) goes here
    Image_Frame: ItemIconContainer_Frame (Box 0.2, pad -6)
    Image_Highlight: HeroicChoiceBorder (alpha 0; game sets it on hover/focus)
    ForegroundSlot: {NamedSlot, pad: 8}
    TextBlock_Checked: {text: "○", Roboto 45, yellow, pad right 10}   # Select mode mark
    Image_Focus: ComparisonIcon 64 (yellow tint, collapsed)
```
- Item: `CompressedItemSlot` + `IsStorage` (bank); `FItemContainerFunctions_C::ConvertCompressedItemSlot` → (item slot, `EItemContainerType`). `ItemData` (FSItemUIData: spec, icon, change id, grade, level).
- Bag: `WidgetItemBag_C::ItemContainers` (one per visible slot, re-bound on page change via `SetContainerItemSlot`).
- Game icons: `WidgetItemIconTooltip_C::Image_Action` shows `Tooltip_Sell` (128×128) / the salvage icon (`CanSalvage`).
- Item details panel `WidgetItemDisplayDetail_C`: `VB_Details` (no member; parent of `TextBlock_AlreadyLearned`) holds name panel, stats,
  set bonus, description, tier, AdditionalDetails (white), AlreadyLearned (orange 1, .651, .27), RequirementsNotMet (red), footer with
  price (`WidgetCurrency`). Narkisim 16. Shown item: `LoadedItemUIData`. Several instances live at once (compare panels).
- Suggested sell/salvage marks use these (features/item-sell/inventory-badges.cpp).

### Loot toast (`WidgetLootToast_C`, `WidgetLootToastEntry_C`, designer templates)
- `WidgetLootToast_C`: Overlay > `WidgetStandardViewport_C` (header/footer/medallion images, `ListView_Items`, `FadeOut` animation);
  data `DisplayTexts`/`IconIDs`/`Grades` arrays (ExposeOnSpawn: game-allocated TArrays, not fillable from our heap). Controller holds it as
  `LootToastWidget` (`I_PlayerControllerToast_C`). When the game shows it: not seen yet.
- Row `WidgetLootToastEntry_C`: HorizontalBox [SizeBox 40×40 > Overlay > `WidgetIconWithUV_C` (icon atlas by id) + `Image_ContentBorder`
  (`ItemIcon_QualityBorder`, Box 0.1)] + `TextBlock_ItemName` (Narkisim 24, outline 1 black). A list entry: fill it with
  `OnListItemObjectSet(WidgetLootToastEntryObject_C{DisplayName, IconId, Grade})`; outside a ListView (pickup toast): unverified.
- Icon id of an item: its spec's `I_GetIconID` (`items::io::IconId`).
- Pickup event: `BP_PlayerControllerGame_C::OnItemAddedDispatcherEvent(FSItemLocator)`, once per slot, also for every slot of a
  reorder/sort.

## Adding a game widget to a screen (game thread only)
1. Run inside a ProcessEvent listener (`game::SetEventListener`), never on the render thread. Guard re-entry (`thread_local` busy flag): our calls re-enter ProcessEvent.
2. Find the target: scan GObjects for the screen's class; skip CDOs and archetypes (`Flags & 0x30`); check `PtrOk` on every field you follow.
3. Create: `WidgetBlueprintLibrary::Create(WorldContext = target, WidgetType = <class>, OwningPlayer = local PC)`. Native static: call it on the class default object with the FUNC_Native flag set (`CallNative` pattern). UMG function bodies are not compiled in: resolve `UFunction`s by name (`Class->GetFunction("PanelWidget", "AddChild")`) and fill `Params::…` structs.
4. Style before adding: copy a sibling's style through the game's setter (`Button::SetStyle`, `TextBlock::SetFont`), never `a->WidgetStyle = b->WidgetStyle`: brushes, colours and fonts hold TSharedPtrs that a C++ byte copy duplicates without a reference (heap corruption, crash on the next map change, #61). Plain colours/sizes (FLinearColor, FVector2D) may be written directly. After adding, use setters only (`SetPadding`, `SetSize`, …).
5. Add: `PanelWidget::AddChild(widget)` appends. To insert after a sibling, `RemoveChild` the later siblings, add yours, re-add them, and restore each slot with `HorizontalBoxSlot::SetSize/SetPadding/SetHorizontalAlignment/SetVerticalAlignment`.
6. Text: `KismetTextLibrary::Conv_StringToText(FString)` gives an `FText` (each call leaks one reference; keep calls rare) → the widget's own setter (`SetButtonText`).
7. Clicks: use a game widget class whose blueprint binds the click (`WidgetButton01_C`). Its `BndEvt__…OnButtonClickedEvent…` passes ProcessEvent on your instance. A plain `UButton`'s `OnClicked` reaches nobody without a delegate binding.
8. Hot reload: the game keeps added widgets. Adopt them on the next load (find your class among the panel's children) and never add a second one.
9. Toggle off from the render thread cannot remove widgets (no UFunction calls there). They stay, inert, until the game rebuilds the screen.
10. Plain widgets (UImage, UTextBlock): `umg::Spawn(UImage::StaticClass(), userWidget->WidgetTree)` (GameplayStatics::SpawnObject), then
    `AddChild` into one of that user widget's panels. Helpers for all steps: `core/umg.hpp`.
