#include "mew_ui_api.h"
#include "FurnitureSale.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef void (__fastcall* FnFurnitureMenuButtonInit)(void* self, uint64_t furniture_id);
typedef int (__fastcall* FnGetFurnitureCost)(void* game_data, MewNarrowString* item_name, uint8_t is_rare);
typedef MewNarrowString* (__fastcall* FnCopyNarrowStringObject)(MewNarrowString* out_string, const MewNarrowString* source_string);
typedef void (__fastcall* FnRemoveOwnedFurniture)(void* furniture_resource, uint64_t furniture_id);
typedef void (__fastcall* FnButtonUpdate)(void* button);
typedef void (__fastcall* FnRebuildFurnitureList)(void* furniture_building_ui, int32_t page_index);
typedef void* (__fastcall* FnResolveFurnitureBuildingUIFromMenuButton)(void* furniture_menu_button);
typedef void (__fastcall* FnFurnitureUiNoArg)(void* furniture_building_ui);
typedef void* (__fastcall* FnGetAudioSourceFromComponent)(void* component);
typedef void (__fastcall* FnAudioSourcePlaySoundEvent)(void* audio_source, MewNarrowString* event_name, double x, double y, double z, uint8_t routed);

typedef struct FurnitureButtonRecord
{
    void* button;
    void* menu_button;
    void* parent_manager;
    uint64_t furniture_id;
    int last_mouse_button_index;
    uint8_t right_mouse_was_down;
    volatile LONG selling;
} FurnitureButtonRecord;

static FnFurnitureMenuButtonInit g_nextFurnitureMenuButtonInit = NULL;
static MewFnButtonCanActivate g_nextButtonCanActivate = NULL;
static MewFnButtonActivate g_nextButtonActivate = NULL;
static FnButtonUpdate g_nextButtonUpdate = NULL;
static FnRebuildFurnitureList g_nextRebuildFurnitureList = NULL;
static void* g_activeFurnitureRebuildManager = NULL;
static LONG g_activeFurnitureRebuildDepth = 0;
static FurnitureButtonRecord g_furnitureButtonRecords[MAX_TRACKED_FURNITURE_BUTTONS];

static MewjectorAPI g_mj;
static volatile LONG g_hookInstalled = 0;

typedef struct PendingFurnitureUiRefresh
{
    void* manager;
    int32_t page_index;
    uint32_t attempts_left;
    uint32_t delay_ticks;
    uint64_t sold_id;
} PendingFurnitureUiRefresh;

static PendingFurnitureUiRefresh g_pendingFurnitureUiRefresh;

static void* ResolveFurnitureBuildingUI(FurnitureButtonRecord* record);

static UINT_PTR GameAddress(UINT_PTR rva)
{
    if (!g_mj.GetGameBase)
    {
        return 0ULL;
    }

    return g_mj.GetGameBase() + rva;
}

static void Log(const char* fmt, ...)
{
    char buffer[512];
    va_list args;

    if (!fmt)
    {
        return;
    }

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    MewUI_LogMessage("%s", buffer);
}

static void* FindFurnitureMetaById(uint64_t furniture_id)
{
    void* resource;
    void** entries;
    uint32_t count;
    uint32_t i;

    resource = NULL;
    entries = NULL;
    count = 0U;

    __try
    {
        void** resource_slot = (void**)GameAddress(MEW_RVA_SAVE_DATA_SINGLETON);

        if (!resource_slot || !*resource_slot)
        {
            return NULL;
        }

        resource = *resource_slot;

        // (SaveData owns the furniture metadata vector used to resolve id -> item name/flags)...
        count = *(uint32_t*)((uint8_t*)resource + OFF_FURNITURE_META_VECTOR_COUNT);
        entries = *(void***)((uint8_t*)resource + OFF_FURNITURE_META_VECTOR_DATA);

        if (!entries || count > 8192U)
        {
            return NULL;
        }

        for (i = 0U; i < count; ++i)
        {
            void* entry = entries[i];

            if (entry && *(uint64_t*)((uint8_t*)entry + OFF_FURNITURE_META_ID) == furniture_id)
            {
                return entry;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return NULL;
    }

    return NULL;
}

static int InitNarrowLiteral(MewNarrowString* out_string, const char* text)
{
    MewFnInitNarrowString init_string;

    if (!out_string || !text)
    {
        return 0;
    }

    init_string = (MewFnInitNarrowString)GameAddress(MEW_RVA_INIT_NARROW_STRING);

    if (!init_string)
    {
        return 0;
    }

    memset(out_string, 0, sizeof(*out_string));
    init_string(out_string, text);
    return 1;
}

static void DestroyNarrowSafe(MewNarrowString* value)
{
    MewFnDestroyNarrowString destroy_string;

    if (!value)
    {
        return;
    }

    destroy_string = (MewFnDestroyNarrowString)GameAddress(MEW_RVA_DESTROY_NARROW_STRING);

    if (!destroy_string)
    {
        return;
    }

    __try
    {
        destroy_string(value);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static int SetTextElementWideStringCopyLocal(void* text_element, const MewWideString* text)
{
    MewFnCopyWideStringObject copy_wide_string;
    MewFnDestroyWideString destroy_wide_string;
    MewFnSetTextElementWideString set_text;
    MewWideString temporary_string;
    uint8_t temporary_initialized;
    uint8_t temporary_transferred_to_engine;
    int result;

    if (!text_element || !text)
    {
        return 0;
    }

    copy_wide_string = (MewFnCopyWideStringObject)GameAddress(MEW_RVA_COPY_WIDE_STRING_OBJECT);
    destroy_wide_string = (MewFnDestroyWideString)GameAddress(MEW_RVA_DESTROY_WIDE_STRING);
    set_text = (MewFnSetTextElementWideString)GameAddress(MEW_RVA_SET_TEXT_ELEMENT_WIDE_STRING);

    if (!copy_wide_string || !destroy_wide_string || !set_text)
    {
        return 0;
    }

    memset(&temporary_string, 0, sizeof(temporary_string));
    temporary_initialized = 0U;
    temporary_transferred_to_engine = 0U;
    result = 0;

    __try
    {
        copy_wide_string(&temporary_string, text);
        temporary_initialized = 1U;

        temporary_transferred_to_engine = 1U;
        set_text(text_element, &temporary_string, 0U, 1U);
        result = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = 0;
    }

    if (temporary_initialized && !temporary_transferred_to_engine)
    {
        __try
        {
            destroy_wide_string(&temporary_string);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    return result;
}

static int GetFurniturePrice(uint64_t furniture_id, uint32_t* out_price)
{
    void* meta;
    void* game_data;
    MewNarrowString item_name;
    FnGetFurnitureCost get_furniture_cost;
    FnCopyNarrowStringObject copy_item_name;
    uint8_t is_rare;
    int item_name_owned_by_local;
    int result;

    if (!out_price)
    {
        return 0;
    }

    *out_price = 0U;

    meta = FindFurnitureMetaById(furniture_id);

    if (!meta)
    {
        return 0;
    }

    get_furniture_cost = (FnGetFurnitureCost)GameAddress(MEW_RVA_GET_FURNITURE_COST);
    copy_item_name = (FnCopyNarrowStringObject)GameAddress(MEW_RVA_COPY_NARROW_STRING_OBJECT);

    if (!get_furniture_cost || !copy_item_name)
    {
        return 0;
    }

    memset(&item_name, 0, sizeof(item_name));
    item_name_owned_by_local = 0;
    result = 0;

    __try
    {
        // Always pass a copied temporary string and then let the game function consume it...
        game_data = *(void**)GameAddress(MEW_RVA_GAME_DATA_SINGLETON);

        if (!game_data)
        {
            __leave;
        }

        // Copy the metadata string because the native cost routine consumes its string argument...
        copy_item_name(&item_name, (const MewNarrowString*)((uint8_t*)meta + OFF_FURNITURE_META_ITEM_NAME_STRING));
        item_name_owned_by_local = 1;

        is_rare = (uint8_t)((*(uint8_t*)((uint8_t*)meta + OFF_FURNITURE_META_FLAGS) >> 1) & 1U);

        *out_price = (uint32_t)get_furniture_cost(game_data, &item_name, is_rare);

        // get_furniture_cost consumes/destroys item_name on the normal path...
        item_name_owned_by_local = 0;
        result = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = 0;
    }

    if (item_name_owned_by_local)
    {
        DestroyNarrowSafe(&item_name);
    }

    return result;
}

static int GetFurnitureSellPrice(uint64_t furniture_id, uint32_t* out_price)
{
    uint32_t original_price;

    if (!out_price)
    {
        return 0;
    }

    *out_price = 0U;

    if (!GetFurniturePrice(furniture_id, &original_price))
    {
        return 0;
    }

    // Furniture resale value is one quarter of the original shop price, rounded up...
    *out_price = (original_price + 3U) / 4U;
    return 1;
}

static void* FindNodeByPathOrNameRaw(void* root_node, const char* node_name, uint8_t preserve_path_mode)
{
    MewFnFindNodeByPathOrName find_node;
    MewFnInitNarrowString init_string;
    MewFnDestroyNarrowString destroy_string;
    MewNarrowString name_string;
    uint8_t name_initialized;
    uint8_t name_transferred_to_engine;
    void* result;

    if (!root_node || !node_name)
    {
        return NULL;
    }

    find_node = (MewFnFindNodeByPathOrName)GameAddress(MEW_RVA_UI_FIND_NODE_BY_PATH_OR_NAME);
    init_string = (MewFnInitNarrowString)GameAddress(MEW_RVA_INIT_NARROW_STRING);
    destroy_string = (MewFnDestroyNarrowString)GameAddress(MEW_RVA_DESTROY_NARROW_STRING);

    if (!find_node || !init_string || !destroy_string)
    {
        return NULL;
    }

    memset(&name_string, 0, sizeof(name_string));
    name_initialized = 0U;
    name_transferred_to_engine = 0U;
    result = NULL;

    __try
    {
        init_string(&name_string, node_name);
        name_initialized = 1U;

        // (This engine routine consumes the narrow string argument before returning)...
        name_transferred_to_engine = 1U;
        result = find_node(root_node, &name_string, preserve_path_mode);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = NULL;
    }

    if (name_initialized && !name_transferred_to_engine)
    {
        __try
        {
            destroy_string(&name_string);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    return result;
}

static void DumpPlausibleFurnitureMetaFields(uint64_t furniture_id)
{
    static volatile LONG dumped = 0;
    void* meta;
    char line[512];
    size_t used;
    uint32_t off;

    if (InterlockedCompareExchange(&dumped, 1, 0) != 0)
    {
        return;
    }

    meta = FindFurnitureMetaById(furniture_id);

    if (!meta)
    {
        Log("Furniture meta dump skipped: no meta for id=%llu", (unsigned long long)furniture_id);
        return;
    }

    used = 0U;
    used += (size_t)snprintf(line + used, sizeof(line) - used, "Meta candidate uint32 fields for id=%llu meta=%p:", (unsigned long long)furniture_id, meta);

    __try
    {
        for (off = OFF_FURNITURE_META_DUMP_SCAN_BEGIN; off <= OFF_FURNITURE_META_DUMP_SCAN_END; off += OFF_FURNITURE_META_DUMP_SCAN_STEP)
        {
            uint32_t v = *(uint32_t*)((uint8_t*)meta + off);

            if (v <= MAX_PLAUSIBLE_META_FIELD_VALUE)
            {
                used += (size_t)snprintf(line + used, sizeof(line) - used, " +0x%02X=%u", (unsigned int)off, (unsigned int)v);
                
                if (used + 32U >= sizeof(line))
                {
                    break;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Exception while dumping meta fields for id=%llu", (unsigned long long)furniture_id);
        return;
    }

    Log("%s", line);
}

static void* FindPriceNodeFromCandidateRoot(void* candidate_root, const char* candidate_label, const char** out_method)
{
    void* sell_info_node;
    void* price_node;

    if (out_method)
    {
        *out_method = NULL;
    }

    if (!candidate_root)
    {
        return NULL;
    }

    // First try the exact authored hierarchy...
    sell_info_node = MewUI_FindChildByName(candidate_root, SELL_INFO_CHILD_NAME);

    if (sell_info_node)
    {
        price_node = MewUI_FindChildByName(sell_info_node, PRICE_CHILD_NAME);

        if (price_node)
        {
            if (out_method) *out_method = "FindChild furnitureSellInfo -> price";
            return price_node;
        }
    }

    // Then use the game's recursive/path resolver, which is what FurnitureMenuButton::init uses..
    price_node = FindNodeByPathOrNameRaw(candidate_root, "furnitureSellInfo.price", 0U);

    if (price_node)
    {
        if (out_method) *out_method = "FindNode furnitureSellInfo.price mode0";
        return price_node;
    }

    price_node = FindNodeByPathOrNameRaw(candidate_root, "furnitureSellInfo.price", 1U);

    if (price_node)
    {
        if (out_method) *out_method = "FindNode furnitureSellInfo.price mode1";
        return price_node;
    }

    price_node = FindNodeByPathOrNameRaw(candidate_root, "furnitureSellInfo/price", 0U);

    if (price_node)
    {
        if (out_method) *out_method = "FindNode furnitureSellInfo/price mode0";
        return price_node;
    }

    price_node = FindNodeByPathOrNameRaw(candidate_root, "furnitureSellInfo/price", 1U);

    if (price_node)
    {
        if (out_method) *out_method = "FindNode furnitureSellInfo/price mode1";
        return price_node;
    }

    price_node = FindNodeByPathOrNameRaw(candidate_root, PRICE_CHILD_NAME, 0U);

    if (price_node)
    {
        if (out_method) *out_method = "FindNode price mode0";
        return price_node;
    }

    price_node = FindNodeByPathOrNameRaw(candidate_root, PRICE_CHILD_NAME, 1U);

    if (price_node)
    {
        if (out_method) *out_method = "FindNode price mode1";
        return price_node;
    }

    (void)candidate_label;
    return NULL;
}

static int TrySetPriceOnRoot(void* root_node, const char* root_label, uint64_t furniture_id, const char* price_value)
{
    void* price_node;
    const char* method;

    if (!root_node || !price_value)
    {
        return 0;
    }

    method = NULL;
    price_node = FindPriceNodeFromCandidateRoot(root_node, root_label, &method);
    if (!price_node)
    {
        return 0;
    }

    if (!MewUI_SetTextElementFromLocalizationKeyValue(price_node, PRICE_LOCALIZATION_KEY, price_value))
    {
        Log("Found price node but localized price set failed: root=%s id=%llu value=%s method=%s node=%p", root_label ? root_label : "?", (unsigned long long)furniture_id, price_value, method ? method : "?", price_node);
        return 0;
    }

    Log("Set owned furniture price: root=%s id=%llu value=%s method=%s node=%p", root_label ? root_label : "?", (unsigned long long)furniture_id, price_value, method ? method : "?", price_node);
    return 1;
}

static void SetOwnedFurniturePriceText(void* furniture_menu_button, uint64_t furniture_id)
{
    void* root_node;
    void* root_owned_node;
    void* root_display_node;
    void* maybe_button_node;
    uint32_t price;
    char price_value[32];

    if (!furniture_menu_button)
    {
        return;
    }

    root_node = NULL;
    root_owned_node = NULL;
    root_display_node = NULL;
    maybe_button_node = NULL;
    price = 0U;
    price_value[0] = '\0';

    if (!GetFurnitureSellPrice(furniture_id, &price))
    {
        Log("No furniture sell price for id=%llu", (unsigned long long)furniture_id);
        return;
    }

    snprintf(price_value, sizeof(price_value), "%u", (unsigned int)price);

    __try
    {
        // FurnitureMenuButton stores the clickable/root UI component here...
        root_node = *(void**)((uint8_t*)furniture_menu_button + OFF_FURNITURE_MENU_BUTTON_ROOT_NODE);

        if (root_node)
        {
            root_owned_node = *(void**)((uint8_t*)root_node + MEW_OFF_BUTTON_NODE);
            root_display_node = *(void**)((uint8_t*)root_node + OFF_UI_ROOT_DISPLAY_NODE);
            maybe_button_node = root_owned_node;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Exception while reading furniture button root for id=%llu", (unsigned long long)furniture_id);
        return;
    }

    if (TrySetPriceOnRoot(root_owned_node, "self+0x38/root+0x48", furniture_id, price_value) || TrySetPriceOnRoot(root_node, "self+0x38/root", furniture_id, price_value) || TrySetPriceOnRoot(root_display_node, "self+0x38/root+0x38", furniture_id, price_value) || TrySetPriceOnRoot(maybe_button_node, "root+MEW_OFF_BUTTON_NODE/fallback", furniture_id, price_value))
    {
        return;
    }

    Log("Could not find price node for furniture id=%llu root=%p root+0x48=%p root+0x38=%p price=%s", (unsigned long long)furniture_id, root_node, root_owned_node, root_display_node, price_value);
}

static FurnitureButtonRecord* FindTrackedFurnitureButton(void* button)
{
    uint32_t i;

    if (!button)
    {
        return NULL;
    }

    for (i = 0U; i < MAX_TRACKED_FURNITURE_BUTTONS; ++i)
    {
        if (g_furnitureButtonRecords[i].button == button)
        {
            return &g_furnitureButtonRecords[i];
        }
    }

    return NULL;
}

static FurnitureButtonRecord* TrackFurnitureButton(void* button, void* menu_button, void* parent_manager, uint64_t furniture_id)
{
    FurnitureButtonRecord* empty_record;
    uint32_t i;

    if (!button)
    {
        return NULL;
    }

    empty_record = NULL;

    for (i = 0U; i < MAX_TRACKED_FURNITURE_BUTTONS; ++i)
    {
        if (g_furnitureButtonRecords[i].button == button)
        {
            g_furnitureButtonRecords[i].menu_button = menu_button;
            g_furnitureButtonRecords[i].parent_manager = parent_manager;
            g_furnitureButtonRecords[i].furniture_id = furniture_id;
            g_furnitureButtonRecords[i].last_mouse_button_index = -1;
            g_furnitureButtonRecords[i].right_mouse_was_down = 0U;
            InterlockedExchange(&g_furnitureButtonRecords[i].selling, 0);
            return &g_furnitureButtonRecords[i];
        }

        if (!empty_record && !g_furnitureButtonRecords[i].button)
        {
            empty_record = &g_furnitureButtonRecords[i];
        }
    }

    if (!empty_record)
    {
        return NULL;
    }

    empty_record->button = button;
    empty_record->menu_button = menu_button;
    empty_record->parent_manager = parent_manager;
    empty_record->furniture_id = furniture_id;
    empty_record->last_mouse_button_index = -1;
    empty_record->right_mouse_was_down = 0U;
    InterlockedExchange(&empty_record->selling, 0);
    return empty_record;
}

static void UntrackFurnitureButton(void* button)
{
    FurnitureButtonRecord* record;

    record = FindTrackedFurnitureButton(button);
    if (record)
    {
        memset(record, 0, sizeof(*record));
        record->last_mouse_button_index = -1;
    }
}

static int RemoveOwnedFurnitureById(uint64_t furniture_id)
{
    void** resource_slot;
    void* resource;
    FnRemoveOwnedFurniture remove_owned_furniture;
    uint32_t before_count;
    uint32_t after_count;
    int result;

    remove_owned_furniture = (FnRemoveOwnedFurniture)GameAddress(MEW_RVA_REMOVE_OWNED_FURNITURE);

    if (!remove_owned_furniture)
    {
        return 0;
    }

    result = 0;

    __try
    {
        resource_slot = (void**)GameAddress(MEW_RVA_SAVE_DATA_SINGLETON);
        if (!resource_slot || !*resource_slot)
        {
            __leave;
        }

        resource = *resource_slot;
        before_count = *(uint32_t*)((uint8_t*)resource + OFF_FURNITURE_META_VECTOR_COUNT);

        remove_owned_furniture(resource, furniture_id);

        after_count = *(uint32_t*)((uint8_t*)resource + OFF_FURNITURE_META_VECTOR_COUNT);
        result = (after_count < before_count) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = 0;
    }

    return result;
}

static int RefreshHouseGoldText(uint32_t new_total)
{
    wchar_t wide_value[16];
    MewWideString value_string;
    void* scene;
    void* text_node;
    int refreshed;

    swprintf(wide_value, sizeof(wide_value) / sizeof(wide_value[0]), L"%u", (unsigned int)new_total);
    MewUI_InitSmallWideString(&value_string, wide_value);

    refreshed = 0;

    scene = MewUI_GetSceneByName("House");

    if (!scene)
    {
        return 0;
    }

    text_node = MewUI_FindNodeInSceneByName(scene, HOUSE_GOLD_TEXT_NODE_NAME);

    if (!text_node)
    {
        return 0;
    }

    if (SetTextElementWideStringCopyLocal(text_node, &value_string))
    {
        Log("Refreshed house_gold UI text in house scene! value=%u", (unsigned int)new_total);
        refreshed = 1;
    }

    return refreshed;
}

static uint32_t GetCurrentHouseCoinsSafe(void)
{
    uint32_t value;

    value = 0U;

    __try
    {
        void* save_data = *(void**)GameAddress(MEW_RVA_SAVE_DATA_SINGLETON);
        if (save_data)
        {
            value = *(uint32_t*)((uint8_t*)save_data + OFF_HOUSE_GOLD);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        value = 0U;
    }

    return value;
}

static int SetTextElementLiteralWide(void* text_element, const wchar_t* text)
{
    MewWideString value_string;

    if (!text_element || !text)
    {
        return 0;
    }

    MewUI_InitSmallWideString(&value_string, text);
    return SetTextElementWideStringCopyLocal(text_element, &value_string);
}

static void* FindNamedNodeFromRootCandidates(void* primary_root, const char* node_name)
{
    void* candidates[20];
    uint32_t candidate_count;
    uint32_t i;

    if (!primary_root || !node_name)
    {
        return NULL;
    }

    memset(candidates, 0, sizeof(candidates));
    candidate_count = 0U;
    candidates[candidate_count++] = primary_root;

    __try
    {
        void* ui_root = *(void**)((uint8_t*)primary_root + OFF_UI_ROOT_WRAPPER);

        if (ui_root && candidate_count < (uint32_t)(sizeof(candidates) / sizeof(candidates[0])))
        {
            candidates[candidate_count++] = ui_root;
        }

        if (ui_root)
        {
            uint32_t j;

            for (j = 0U; j < (uint32_t)(sizeof(FURNITURE_UI_ROOT_PROBE_OFFSETS) / sizeof(FURNITURE_UI_ROOT_PROBE_OFFSETS[0])); ++j)
            {
                void* child = *(void**)((uint8_t*)ui_root + FURNITURE_UI_ROOT_PROBE_OFFSETS[j]);

                if (child && candidate_count < (uint32_t)(sizeof(candidates) / sizeof(candidates[0])))
                {
                    candidates[candidate_count++] = child;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    for (i = 0U; i < candidate_count; ++i)
    {
        void* root;
        void* node;
        uint32_t k;
        int duplicate;

        root = candidates[i];

        if (!root)
        {
            continue;
        }

        duplicate = 0;

        for (k = 0U; k < i; ++k)
        {
            if (candidates[k] == root)
            {
                duplicate = 1;
                break;
            }
        }

        if (duplicate)
        {
            continue;
        }

        node = MewUI_FindChildByName(root, node_name);

        if (node)
        {
            return node;
        }

        node = FindNodeByPathOrNameRaw(root, node_name, 0U);

        if (node)
        {
            return node;
        }

        node = FindNodeByPathOrNameRaw(root, node_name, 1U);

        if (node)
        {
            return node;
        }
    }

    return NULL;
}

typedef int (*FnApplyFurnitureTextNode)(void* text_element, void* context);

static int ApplyLiteralWideTextNode(void* text_element, void* context)
{
    const wchar_t* value;

    value = (const wchar_t*)context;
    return SetTextElementLiteralWide(text_element, value);
}

static int ApplyFurnitureUiNamedTextWithCallback(void* furniture_building_ui, const char* node_name, FnApplyFurnitureTextNode apply, void* context)
{
    void* node;
    void* scene;
    int applied;

    if (!node_name || !apply)
    {
        return 0;
    }

    applied = 0;

    node = FindNamedNodeFromRootCandidates(furniture_building_ui, node_name);

    if (node && apply(node, context))
    {
        applied = 1;
    }

    scene = MewUI_GetSceneByName("House");

    if (!scene)
    {
        return 0;
    }

    node = MewUI_FindNodeInSceneByName(scene, node_name);

    if (!node)
    {
        node = MewUI_FindChildByNameInScene(scene, node_name);
    }

    if (node && apply(node, context))
    {
        applied = 1;
    }

    return applied;
}

static int SetFurnitureUiNamedText(void* furniture_building_ui, const char* node_name, const wchar_t* value)
{
    if (!value)
    {
        return 0;
    }

    return ApplyFurnitureUiNamedTextWithCallback(furniture_building_ui, node_name, ApplyLiteralWideTextNode, (void*)value);
}

static int PlayFurnitureUiCoinPulse(void* furniture_building_ui, const char* reason)
{
    void* pulse_node;
    void* scene;
    int played;

    played = 0;

    scene = MewUI_GetSceneByName("House");

    if (!scene)
    {
        return 0;
    }

    pulse_node = MewUI_FindNodeInSceneByName(scene, FURNITURE_SELL_COIN_PULSE_NODE_NAME);

    if (pulse_node && MewUI_PlayMovieClipFrame(pulse_node, 1))
    {
        played = 1;
    }

    if (played)
    {
        Log("Played furniture sell coin pulse: reason=%s manager=%p node=%s frame=1", reason ? reason : "", furniture_building_ui, FURNITURE_SELL_COIN_PULSE_NODE_NAME);
    }
    else
    {
        Log("Furniture sell coin pulse not found yet: reason=%s manager=%p buildMenu=%s node=%s", reason ? reason : "", furniture_building_ui, BUILD_MENU_NODE_NAME, FURNITURE_SELL_COIN_PULSE_NODE_NAME);
    }

    return played;
}

static void UpdateFurniturePickerMoney(void* furniture_building_ui, const char* reason)
{
    wchar_t money_value[16];
    uint32_t coins;
    int money_set;

    coins = GetCurrentHouseCoinsSafe();
    swprintf(money_value, sizeof(money_value) / sizeof(money_value[0]), L"%u", (unsigned int)coins);

    money_set = SetFurnitureUiNamedText(furniture_building_ui, FURNITURE_CURRENT_MONEY_TEXT_NODE_NAME, money_value);

    if (money_set)
    {
        Log("Updated furniture picker current money: reason=%s coins=%u furnitureCurrentMoney=%d manager=%p", reason ? reason : "", (unsigned int)coins, money_set, furniture_building_ui);
    }
    else
    {
        Log("Furniture picker current money text not found yet: reason=%s coins=%u manager=%p name=%s", reason ? reason : "", (unsigned int)coins, furniture_building_ui, FURNITURE_CURRENT_MONEY_TEXT_NODE_NAME);
    }
}

static void PlayFurnitureSellCoinSound(FurnitureButtonRecord* record)
{
    FnGetAudioSourceFromComponent get_audio_source;
    FnAudioSourcePlaySoundEvent play_sound_event;
    MewNarrowString sound_event;
    void* candidates[4];
    void* audio_source;
    uint32_t i;
    uint32_t audio_source_index;

    if (!record)
    {
        return;
    }

    get_audio_source = (FnGetAudioSourceFromComponent)GameAddress(MEW_RVA_GET_AUDIO_SOURCE_FROM_COMPONENT);
    play_sound_event = (FnAudioSourcePlaySoundEvent)GameAddress(MEW_RVA_AUDIO_SOURCE_PLAY_SOUND_EVENT);

    if (!get_audio_source || !play_sound_event)
    {
        Log("Sell coin sound skipped: audio function address unavailable!");
        return;
    }

    memset(candidates, 0, sizeof(candidates));
    // Prefer the underlying Button component...
    // (Button.cpp plays its audible UI click SFX by resolving AudioSource from the Button itself before calling AudioSource::PlaySoundEvent)...
    candidates[0] = record->button;
    candidates[1] = record->menu_button;
    candidates[2] = ResolveFurnitureBuildingUI(record);
    candidates[3] = record->parent_manager;

    audio_source = NULL;
    audio_source_index = UINT32_MAX;

    for (i = 0U; i < (uint32_t)(sizeof(candidates) / sizeof(candidates[0])); ++i)
    {
        if (!candidates[i])
        {
            continue;
        }

        __try
        {
            audio_source = get_audio_source(candidates[i]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            audio_source = NULL;
        }

        if (audio_source)
        {
            audio_source_index = i;
            break;
        }
    }

    if (!audio_source)
    {
        Log("Sell coin sound skipped: no AudioSource found for menu_button=%p button=%p manager=%p event=%s", record->menu_button, record->button, ResolveFurnitureBuildingUI(record), SELL_COIN_SOUND_EVENT_NAME);
        return;
    }

    if (!InitNarrowLiteral(&sound_event, SELL_COIN_SOUND_EVENT_NAME))
    {
        Log("Sell coin sound skipped: could not build sound-event string %s", SELL_COIN_SOUND_EVENT_NAME);
        return;
    }

    __try
    {
        play_sound_event(audio_source, &sound_event, 1.0, 1.0, 0.0, 0U);

        Log("Played furniture sell coin sound: event=%s audio_source=%p source_index=%u volume_xy=1.0,1.0", SELL_COIN_SOUND_EVENT_NAME, audio_source, (unsigned int)audio_source_index);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Sell coin sound failed: exception event=%s audio_source=%p", SELL_COIN_SOUND_EVENT_NAME, audio_source);
    }

    DestroyNarrowSafe(&sound_event);
}

static int MarkComponentDeleted(void* component)
{
    if (!component)
    {
        return 0;
    }

    __try
    {
        *(uint8_t*)((uint8_t*)component + OFF_COMPONENT_DELETED) = 1U;
        *(uint8_t*)((uint8_t*)component + OFF_COMPONENT_ENABLED) = 0U;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static int LooksLikeFurnitureBuildingUI(void* candidate)
{
    uint32_t count;
    void* data;
    int32_t page_index;
    void* ui_root;
    void* button_container;

    if (!candidate)
    {
        return 0;
    }

    __try
    {
        ui_root = *(void**)((uint8_t*)candidate + OFF_UI_ROOT_WRAPPER);

        if (!ui_root)
        {
            return 0;
        }

        button_container = *(void**)((uint8_t*)ui_root + OFF_UI_ROOT_BUTTON_CONTAINER);

        if (!button_container)
        {
            return 0;
        }

        count = *(uint32_t*)((uint8_t*)candidate + OFF_FURNITURE_BUILDING_UI_ITEM_COUNT);
        data = *(void**)((uint8_t*)candidate + OFF_FURNITURE_BUILDING_UI_ITEM_DATA);
        page_index = *(int32_t*)((uint8_t*)candidate + OFF_FURNITURE_BUILDING_UI_PAGE_INDEX);

        if (count > 8192U)
        {
            return 0;
        }

        if (count != 0U && !data)
        {
            return 0;
        }

        if (page_index < -1 || page_index > 512)
        {
            return 0;
        }

        (void)*(uint32_t*)((uint8_t*)button_container + OFF_FURNITURE_MENU_BUTTON_OWNED_VECTOR_COUNT);
        (void)*(void**)((uint8_t*)button_container + OFF_FURNITURE_MENU_BUTTON_OWNED_VECTOR_DATA);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static void* ResolveFurnitureBuildingUI(FurnitureButtonRecord* record)
{
    FnResolveFurnitureBuildingUIFromMenuButton resolve_ui;
    void* candidate;

    if (!record)
    {
        return NULL;
    }

    if (LooksLikeFurnitureBuildingUI(record->parent_manager))
    {
        return record->parent_manager;
    }

    if (LooksLikeFurnitureBuildingUI(g_activeFurnitureRebuildManager))
    {
        record->parent_manager = g_activeFurnitureRebuildManager;
        return record->parent_manager;
    }

    candidate = NULL;

    if (record->menu_button)
    {
        resolve_ui = (FnResolveFurnitureBuildingUIFromMenuButton)GameAddress(MEW_RVA_RESOLVE_FURNITURE_BUILDING_UI_FROM_MENU_BUTTON);

        if (resolve_ui)
        {
            __try
            {
                candidate = resolve_ui(record->menu_button);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                candidate = NULL;
            }

            if (LooksLikeFurnitureBuildingUI(candidate))
            {
                record->parent_manager = candidate;
                Log("Resolved FurnitureBuildingUI via native owner lookup: menu_button=%p manager=%p", record->menu_button, candidate);
                return candidate;
            }
        }

        __try
        {
            candidate = *(void**)((uint8_t*)record->menu_button + OFF_FURNITURE_MENU_BUTTON_PARENT_MANAGER);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            candidate = NULL;
        }

        if (LooksLikeFurnitureBuildingUI(candidate))
        {
            record->parent_manager = candidate;
            return candidate;
        }
    }

    Log("Could not resolve valid FurnitureBuildingUI: menu_button=%p stored_parent=%p", record->menu_button, record->parent_manager);
    return NULL;
}

static int RemoveMenuButtonFromOwnedVector(FurnitureButtonRecord* record)
{
    void* manager;
    void** entries;
    uint32_t count;
    uint32_t i;

    if (!record || !record->menu_button)
    {
        return 0;
    }

    manager = ResolveFurnitureBuildingUI(record);

    if (!manager)
    {
        return 0;
    }

    __try
    {
        // The live owned-button vector must be patched so the sold button is not reused...
        count = *(uint32_t*)((uint8_t*)manager + OFF_FURNITURE_MENU_BUTTON_OWNED_VECTOR_COUNT);
        entries = *(void***)((uint8_t*)manager + OFF_FURNITURE_MENU_BUTTON_OWNED_VECTOR_DATA);

        if (!entries || count > 4096U)
        {
            return 0;
        }

        for (i = 0U; i < count; ++i)
        {
            if (entries[i] == record->menu_button)
            {
                uint32_t j;

                for (j = i + 1U; j < count; ++j)
                {
                    entries[j - 1U] = entries[j];
                }

                entries[count - 1U] = NULL;
                *(uint32_t*)((uint8_t*)manager + OFF_FURNITURE_MENU_BUTTON_OWNED_VECTOR_COUNT) = count - 1U;

                Log("Removed sold FurnitureMenuButton from live owned-button vector: self=%p manager=%p index=%u count=%u->%u", record->menu_button, manager, (unsigned int)i, (unsigned int)count, (unsigned int)(count - 1U));
                return 1;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }

    return 0;
}

static int RemoveSoldFurnitureFromDisplayCache(FurnitureButtonRecord* record)
{
    void* manager;
    uint64_t* entries;
    uint32_t count;
    uint32_t i;

    if (!record)
    {
        return 0;
    }

    manager = ResolveFurnitureBuildingUI(record);

    if (!manager)
    {
        Log("Furniture UI cache removal skipped: FurnitureBuildingUI manager is unresolved!");
        return 0;
    }

    __try
    {
        // The display cache is a compact array of furniture ids backing the visible grid...
        count = *(uint32_t*)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_ITEM_COUNT);
        entries = *(uint64_t**)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_ITEM_DATA);

        if (!entries || count > 8192U)
        {
            Log("Furniture UI cache removal skipped: invalid cache entries=%p count=%u manager=%p", entries, (unsigned int)count, manager);
            return 0;
        }

        for (i = 0U; i < count; ++i)
        {
            if (entries[i] == record->furniture_id)
            {
                uint32_t j;

                for (j = i + 1U; j < count; ++j)
                {
                    entries[j - 1U] = entries[j];
                }

                entries[count - 1U] = 0ULL;

                *(uint32_t*)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_ITEM_COUNT) = count - 1U;

                Log("Removed sold furniture from FurnitureBuildingUI display cache: manager=%p id=%llu index=%u count=%u->%u", manager, (unsigned long long)record->furniture_id, (unsigned int)i, (unsigned int)count, (unsigned int)(count - 1U));
                return 1;
            }
        }

        Log("Furniture UI cache removal did not find id=%llu manager=%p count=%u first=%llu second=%llu third=%llu", (unsigned long long)record->furniture_id, manager, (unsigned int)count, (unsigned long long)(count > 0U ? entries[0] : 0ULL), (unsigned long long)(count > 1U ? entries[1] : 0ULL), (unsigned long long)(count > 2U ? entries[2] : 0ULL));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Furniture UI cache removal failed: exception manager=%p id=%llu", manager, (unsigned long long)record->furniture_id);
        return 0;
    }

    return 0;
}

static void CallRebuildFurnitureListWithCapture(FnRebuildFurnitureList rebuild_fn, void* manager, int32_t page_index)
{
    void* previous_manager;

    if (!rebuild_fn)
    {
        return;
    }

    previous_manager = g_activeFurnitureRebuildManager;
    g_activeFurnitureRebuildManager = manager;
    InterlockedIncrement(&g_activeFurnitureRebuildDepth);

    __try
    {
        rebuild_fn(manager, page_index);
    }
    __finally
    {
        InterlockedDecrement(&g_activeFurnitureRebuildDepth);
        g_activeFurnitureRebuildManager = previous_manager;
    }
}

static int RebuildOwnedFurnitureListNow(FurnitureButtonRecord* record)
{
    void* manager;
    int32_t page_index;
    FnRebuildFurnitureList rebuild_furniture_list;

    if (!record)
    {
        return 0;
    }

    manager = ResolveFurnitureBuildingUI(record);

    if (!manager)
    {
        Log("Immediate owned-furniture list rebuild skipped: FurnitureBuildingUI manager is unresolved");
        return 0;
    }

    rebuild_furniture_list = g_nextRebuildFurnitureList ? g_nextRebuildFurnitureList : (FnRebuildFurnitureList)GameAddress(MEW_RVA_REBUILD_FURNITURE_LIST);

    if (!rebuild_furniture_list)
    {
        Log("Immediate owned-furniture list rebuild skipped: function address unavailable");
        return 0;
    }

    __try
    {
        /*
           This is the path that runs when the furniture UI is opened/refreshed/navigated, so it
           removes stale button components and lays out the new owned list immediately...
        */
        page_index = *(int32_t*)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_PAGE_INDEX);
        *(uint8_t*)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_REDRAW_DIRTY) = 1U;
        CallRebuildFurnitureListWithCapture(rebuild_furniture_list, manager, page_index);
        Log("Rebuilt owned-furniture list after sale: manager=%p page=%d dirty=1", manager, (int)page_index);
        UpdateFurniturePickerMoney(manager, "rebuild-now");
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Immediate owned-furniture list rebuild failed: exception manager=%p", manager);
        return 0;
    }
}

static uint32_t GetOwnedFurnitureCountSafe(void)
{
    __try
    {
        void* save_data = *(void**)GameAddress(MEW_RVA_SAVE_DATA_SINGLETON);

        if (!save_data)
        {
            return 0U;
        }

        return *(uint32_t*)((uint8_t*)save_data + OFF_FURNITURE_META_VECTOR_COUNT);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0U;
    }
}

static void ClampFurnitureUiPageIndex(void* manager)
{
    uint32_t owned_count;
    int32_t page_index;
    int32_t max_page;

    if (!manager)
    {
        return;
    }

    __try
    {
        owned_count = GetOwnedFurnitureCountSafe();
        max_page = (owned_count == 0U) ? 0 : (int32_t)((owned_count - 1U) / 21U);
        page_index = *(int32_t*)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_PAGE_INDEX);

        if (page_index < 0)
        {
            page_index = 0;
        }
        if (page_index > max_page)
        {
            page_index = max_page;
        }

        *(int32_t*)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_PAGE_INDEX) = page_index;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static int ForceFurniturePickerLifecycleRefresh(FurnitureButtonRecord* record, const char* reason)
{
    void* manager;
    int32_t page_index;
    FnFurnitureUiNoArg refresh_cache;
    FnFurnitureUiNoArg sort_cache;
    FnFurnitureUiNoArg update_nav_state;
    FnRebuildFurnitureList redraw_list;

    if (!record)
    {
        return 0;
    }

    manager = ResolveFurnitureBuildingUI(record);

    if (!manager)
    {
        Log("Furniture picker lifecycle refresh skipped: manager unresolved id=%llu reason=%s", (unsigned long long)record->furniture_id, reason ? reason : "");
        return 0;
    }

    refresh_cache = (FnFurnitureUiNoArg)GameAddress(MEW_RVA_REFRESH_FURNITURE_CACHE);
    sort_cache = (FnFurnitureUiNoArg)GameAddress(MEW_RVA_SORT_FURNITURE_CACHE);
    update_nav_state = (FnFurnitureUiNoArg)GameAddress(MEW_RVA_UPDATE_FURNITURE_NAV_STATE);
    redraw_list = g_nextRebuildFurnitureList ? g_nextRebuildFurnitureList : (FnRebuildFurnitureList)GameAddress(MEW_RVA_REBUILD_FURNITURE_LIST);

    if (!refresh_cache || !redraw_list)
    {
        Log("Furniture picker lifecycle refresh skipped: function address unavailable manager=%p", manager);
        return 0;
    }

    __try
    {
        *(uint8_t*)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_REDRAW_DIRTY) = 1U;
        refresh_cache(manager);

        if (sort_cache)
        {
            sort_cache(manager);
        }

        ClampFurnitureUiPageIndex(manager);
        page_index = *(int32_t*)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_PAGE_INDEX);
        CallRebuildFurnitureListWithCapture(redraw_list, manager, page_index);

        if (update_nav_state)
        {
            update_nav_state(manager);
        }

        Log("Forced furniture picker lifecycle refresh: manager=%p page=%d reason=%s cache_count=%u", manager, (int)page_index, reason ? reason : "", (unsigned int)*(uint32_t*)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_ITEM_COUNT));
        UpdateFurniturePickerMoney(manager, reason ? reason : "refresh");
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Furniture picker lifecycle refresh failed: exception manager=%p id=%llu reason=%s", manager, (unsigned long long)record->furniture_id, reason ? reason : "");
        return 0;
    }
}

static void QueueFurniturePickerLifecycleRefresh(FurnitureButtonRecord* record)
{
    void* manager;
    int32_t page_index;

    if (!record)
    {
        return;
    }

    manager = ResolveFurnitureBuildingUI(record);

    if (!manager)
    {
        return;
    }

    page_index = 0;

    __try
    {
        page_index = *(int32_t*)((uint8_t*)manager + OFF_FURNITURE_BUILDING_UI_PAGE_INDEX);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        page_index = 0;
    }

    g_pendingFurnitureUiRefresh.manager = manager;
    g_pendingFurnitureUiRefresh.page_index = page_index;
    g_pendingFurnitureUiRefresh.attempts_left = 8U;
    g_pendingFurnitureUiRefresh.delay_ticks = 1U;
    g_pendingFurnitureUiRefresh.sold_id = record->furniture_id;
}

static void ProcessPendingFurniturePickerRefresh(void)
{
    void* manager;
    FurnitureButtonRecord temporary_record;

    if (g_pendingFurnitureUiRefresh.attempts_left == 0U)
    {
        return;
    }

    if (g_pendingFurnitureUiRefresh.delay_ticks != 0U)
    {
        --g_pendingFurnitureUiRefresh.delay_ticks;
        return;
    }

    manager = g_pendingFurnitureUiRefresh.manager;

    if (!manager)
    {
        g_pendingFurnitureUiRefresh.attempts_left = 0U;
        return;
    }

    memset(&temporary_record, 0, sizeof(temporary_record));
    temporary_record.parent_manager = manager;
    temporary_record.furniture_id = g_pendingFurnitureUiRefresh.sold_id;

    (void)ForceFurniturePickerLifecycleRefresh(&temporary_record, "deferred");

    --g_pendingFurnitureUiRefresh.attempts_left;
    g_pendingFurnitureUiRefresh.delay_ticks = 1U;
}

static void RemoveSoldButtonImmediately(FurnitureButtonRecord* record)
{
    void* root_node;

    if (!record)
    {
        return;
    }

    root_node = NULL;

    __try
    {
        if (record->menu_button)
        {
            root_node = *(void**)((uint8_t*)record->menu_button + OFF_FURNITURE_MENU_BUTTON_ROOT_NODE);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        root_node = NULL;
    }

    (void)RemoveMenuButtonFromOwnedVector(record);
    (void)MarkComponentDeleted(record->button);
    (void)MarkComponentDeleted(root_node);

    if (record->menu_button && record->menu_button != record->button)
    {
        (void)MarkComponentDeleted(record->menu_button);
    }
}

static int GrantHouseCoins(uint32_t amount, uint32_t* out_old_total, uint32_t* out_new_total)
{
    void* save_data;
    uint32_t* house_gold;
    uint32_t old_total;
    uint32_t new_total;

    if (out_old_total)
    {
        *out_old_total = 0U;
    }

    if (out_new_total)
    {
        *out_new_total = 0U;
    }

    if (amount == 0U)
    {
        return 1;
    }

    __try
    {
        save_data = *(void**)GameAddress(MEW_RVA_SAVE_DATA_SINGLETON);

        if (!save_data)
        {
            Log("GrantHouseCoins failed: save-data singleton is null");
            return 0;
        }

        house_gold = (uint32_t*)((uint8_t*)save_data + OFF_HOUSE_GOLD);
        old_total = *house_gold;
        new_total = old_total + amount;

        if (new_total < old_total)
        {
            new_total = UINT32_MAX;
        }

        *house_gold = new_total;

        if (out_old_total)
        {
            *out_old_total = old_total;
        }

        if (out_new_total)
        {
            *out_new_total = new_total;
        }

        if (!RefreshHouseGoldText(new_total))
        {
            Log("GrantHouseCoins note: house_gold storage updated but visible UI text was not found/refreshed");
        }

        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("GrantHouseCoins exception while adding amount=%u", (unsigned int)amount);
        return 0;
    }
}

static int SellTrackedFurniture(FurnitureButtonRecord* record)
{
    uint32_t price;
    uint64_t furniture_id;

    if (!record || !record->button)
    {
        return 0;
    }

    if (InterlockedCompareExchange(&record->selling, 1, 0) != 0)
    {
        return 1;
    }

    furniture_id = record->furniture_id;
    price = 0U;

    if (!GetFurnitureSellPrice(furniture_id, &price))
    {
        Log("Right-click furniture sell failed: could not resolve sell price for id=%llu", (unsigned long long)furniture_id);
        InterlockedExchange(&record->selling, 0);
        return 0;
    }

    if (!RemoveOwnedFurnitureById(furniture_id))
    {
        Log("Right-click furniture sell failed: owned-list removal failed for id=%llu", (unsigned long long)furniture_id);
        InterlockedExchange(&record->selling, 0);
        return 0;
    }

    uint32_t old_total = 0U;
    uint32_t new_total = 0U;

    if (!GrantHouseCoins(price, &old_total, &new_total))
    {
        Log("Right-click furniture sell warning: removed id=%llu but coin grant failed (price=%u)", (unsigned long long)furniture_id, (unsigned int)price);
    }
    else
    {
        void* furniture_ui = ResolveFurnitureBuildingUI(record);

        Log("Sold owned furniture from menu: id=%llu sell_price=%u coins=%u->%u", (unsigned long long)furniture_id, (unsigned int)price, (unsigned int)old_total, (unsigned int)new_total);
        
        PlayFurnitureSellCoinSound(record);
        PlayFurnitureUiCoinPulse(furniture_ui, "sale");
        UpdateFurniturePickerMoney(furniture_ui, "sale");
    }

    if (!ForceFurniturePickerLifecycleRefresh(record, "immediate"))
    {
        (void)RemoveSoldFurnitureFromDisplayCache(record);
        (void)RebuildOwnedFurnitureListNow(record);
    }

    QueueFurniturePickerLifecycleRefresh(record);

    UntrackFurnitureButton(record->button);
    return 1;
}

static int IsButtonHoveredOrPressed(void* button)
{
    int32_t state;

    if (!button)
    {
        return 0;
    }

    __try
    {
        state = *(int32_t*)((uint8_t*)button + OFF_BUTTON_STATE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }

    return (state == 1 || state == 2);
}

static void __fastcall HookButtonUpdate(void* button)
{
    FurnitureButtonRecord* record;
    SHORT right_state;
    int right_down;

    if (g_nextButtonUpdate)
    {
        g_nextButtonUpdate(button);
    }

    ProcessPendingFurniturePickerRefresh();

    record = FindTrackedFurnitureButton(button);

    if (!record)
    {
        return;
    }

    right_state = GetAsyncKeyState(VK_RBUTTON);
    right_down = ((right_state & 0x8000) != 0);

    if (!right_down)
    {
        record->right_mouse_was_down = 0U;
        return;
    }

    if (record->right_mouse_was_down)
    {
        return;
    }

    record->right_mouse_was_down = 1U;

    if (IsButtonHoveredOrPressed(button))
    {
        record->last_mouse_button_index = RIGHT_MOUSE_BUTTON_INDEX;
        (void)SellTrackedFurniture(record);
    }
}

static uint8_t __fastcall HookButtonCanActivate(void* button, int32_t button_index, uint8_t strict_mouse)
{
    uint8_t result;
    FurnitureButtonRecord* record;

    result = 0U;

    if (g_nextButtonCanActivate)
    {
        result = g_nextButtonCanActivate(button, button_index, strict_mouse);
    }

    record = FindTrackedFurnitureButton(button);

    if (record && button_index >= 0)
    {
        record->last_mouse_button_index = button_index;
    }

    return result;
}

static void __fastcall HookButtonActivate(void* button, uint8_t from_mouse)
{
    FurnitureButtonRecord* record;

    record = FindTrackedFurnitureButton(button);

    if (record && from_mouse && record->last_mouse_button_index == RIGHT_MOUSE_BUTTON_INDEX)
    {
        record->last_mouse_button_index = -1;
        (void)SellTrackedFurniture(record);
        return;
    }

    if (record)
    {
        record->last_mouse_button_index = -1;
        record->right_mouse_was_down = 0U;
    }

    if (g_nextButtonActivate)
    {
        g_nextButtonActivate(button, from_mouse);
    }
}

static void __fastcall HookRebuildFurnitureList(void* furniture_building_ui, int32_t page_index)
{
    CallRebuildFurnitureListWithCapture(g_nextRebuildFurnitureList, furniture_building_ui, page_index);

    if (LooksLikeFurnitureBuildingUI(furniture_building_ui))
    {
        UpdateFurniturePickerMoney(furniture_building_ui, "rebuild");
    }
}

static void __fastcall HookFurnitureMenuButtonInit(void* self, uint64_t furniture_id)
{
    void* button;
    void* parent_manager;

    if (g_nextFurnitureMenuButtonInit)
    {
        g_nextFurnitureMenuButtonInit(self, furniture_id);
    }

    button = NULL;
    parent_manager = NULL;

    __try
    {
        if (self)
        {
            // Capture the underlying Button component created by FurnitureMenuButton::init...
            button = *(void**)((uint8_t*)self + OFF_FURNITURE_MENU_BUTTON_ROOT_NODE);

            if (LooksLikeFurnitureBuildingUI(g_activeFurnitureRebuildManager))
            {
                parent_manager = g_activeFurnitureRebuildManager;
            }
            else
            {
                parent_manager = *(void**)((uint8_t*)self + OFF_FURNITURE_MENU_BUTTON_PARENT_MANAGER);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        button = NULL;
    }

    if (button)
    {
        FurnitureButtonRecord* tracked_record;
        tracked_record = TrackFurnitureButton(button, self, parent_manager, furniture_id);

        if (tracked_record && LooksLikeFurnitureBuildingUI(parent_manager))
        {
            Log("Tracked FurnitureMenuButton with captured FurnitureBuildingUI: menu_button=%p manager=%p id=%llu", self, parent_manager, (unsigned long long)furniture_id);
        }
    }

    SetOwnedFurniturePriceText(self, furniture_id);
}

static void TryInstallHooks(void)
{
    MewjectorAPI resolved;

    if (InterlockedCompareExchange(&g_hookInstalled, 0, 0) != 0)
    {
        return;
    }

    memset(&resolved, 0, sizeof(resolved));

    if (!MJ_Resolve(&resolved))
    {
        return;
    }

    memcpy(&g_mj, &resolved, sizeof(g_mj));

    if (!g_mj.InstallHook(MEW_RVA_FURNITURE_MENU_BUTTON_INIT, MEW_RVA_FURNITURE_MENU_BUTTON_INIT_STOLEN_BYTES, (void*)HookFurnitureMenuButtonInit, (void**)&g_nextFurnitureMenuButtonInit, HOOK_PRIORITY, MOD_NAME))
    {
        return;
    }

    if (!g_mj.InstallHook(MEW_RVA_REBUILD_FURNITURE_LIST, REBUILD_FURNITURE_LIST_STOLEN_BYTES, (void*)HookRebuildFurnitureList, (void**)&g_nextRebuildFurnitureList, HOOK_PRIORITY - 2, MOD_NAME))
    {
        return;
    }

    if (!g_mj.InstallHook(MEW_RVA_BUTTON_UPDATE, BUTTON_UPDATE_STOLEN_BYTES, (void*)HookButtonUpdate, (void**)&g_nextButtonUpdate, HOOK_PRIORITY - 1, MOD_NAME))
    {
        return;
    }

    if (!g_mj.InstallHook(MEW_RVA_BUTTON_CAN_ACTIVATE, MEW_RVA_BUTTON_CAN_ACTIVATE_STOLEN_BYTES, (void*)HookButtonCanActivate, (void**)&g_nextButtonCanActivate, HOOK_PRIORITY - 1, MOD_NAME))
    {
        return;
    }

    if (!g_mj.InstallHook(MEW_RVA_BUTTON_ACTIVATE, MEW_RVA_BUTTON_ACTIVATE_STOLEN_BYTES, (void*)HookButtonActivate, (void**)&g_nextButtonActivate, HOOK_PRIORITY - 1, MOD_NAME))
    {
        return;
    }

    InterlockedExchange(&g_hookInstalled, 1);
    Log("Installed FurnitureMenuButton::init/rebuild/right-click-sell hooks at RVAs 0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX", (unsigned long long)MEW_RVA_FURNITURE_MENU_BUTTON_INIT, (unsigned long long)MEW_RVA_REBUILD_FURNITURE_LIST, (unsigned long long)MEW_RVA_BUTTON_UPDATE, (unsigned long long)MEW_RVA_BUTTON_CAN_ACTIVATE, (unsigned long long)MEW_RVA_BUTTON_ACTIVATE);
}

static void __cdecl UITick(void* userData)
{
    (void)userData;
    TryInstallHooks();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        MewUI_SetDebugLogsEnabled(false);
        MewUI_Start(MOD_NAME, HOOK_PRIORITY, UI_BOOTSTRAP_INTERVAL_MS, UI_TICK_INTERVAL_MS, UITick, NULL);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        MewUI_Stop();
    }

    return TRUE;
}