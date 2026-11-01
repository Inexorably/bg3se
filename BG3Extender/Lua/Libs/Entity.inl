/// <lua_module>Entity</lua_module>
#include <GameDefinitions/Components/All.h>

BEGIN_NS(lua::entity)

std::optional<Guid> HandleToUuid(lua_State* L, EntityHandle entity)
{
    auto uuid = State::FromLua(L)->GetEntitySystemHelpers()->GetComponent<UuidComponent>(entity);
    if (uuid) {
        return uuid->EntityUuid;
    } else {
        return {};
    }
}

EntityHandle UuidToHandle(lua_State* L, Guid uuid)
{
    return State::FromLua(L)->GetEntitySystemHelpers()->GetEntityHandle(uuid);
}

UserReturn Get(lua_State* L, lua::AnyRef entity)
{
    auto type = lua_type(L, entity.Index);
    switch (type) {
    case LUA_TNIL:
    {
        push(L, nullptr);
        break;
    }
    
    case LUA_TSTRING:
    {
        auto uuid = get<Guid>(L, entity.Index);
        auto handle = State::FromLua(L)->GetEntitySystemHelpers()->GetEntityHandle(uuid);
        if (handle) {
            EntityProxyMetatable::Make(L, handle);
        } else {
            push(L, nullptr);
        }
        break;
    }
    
    case LUA_TNUMBER:
    {
        auto netId = get<NetId>(L, entity.Index);
        auto ecs = State::FromLua(L)->GetEntitySystemHelpers();
        auto handle = ecs->NetIdToEntity(netId);
        if (handle) {
            EntityProxyMetatable::Make(L, *handle);
        } else {
            push(L, nullptr);
        }
        break;
    }
    
    case LUA_TLIGHTCPPOBJECT:
    {
        auto handle = get<EntityHandle>(L, entity.Index);
        if (handle) {
            EntityProxyMetatable::Make(L, handle);
        } else {
            push(L, nullptr);
        }
        break;
    }

    default:
        return luaL_error(L, "Expected entity GUID, network ID or entity handle, got %s", GetDebugName(L, entity.Index));
    }

    return 1;
}

// Armor Class (AC) of an entity.
//
// The authoritative value is eoc::ResistancesComponent::AC: the AC the engine
// itself sums up and shows on the character sheet (it already includes worn
// armour, ability contribution and every evaluated boost). At runtime it is
// returned unchanged as Total with Source = "engine"; it is never combined with,
// or overridden by, a value rebuilt in this function.
//
// The per-source rebuild below produces the `Computed` breakdown for diagnostics
// and is used as Total only when the engine field is empty (Source =
// "components"). It reads the same inputs the engine uses, in typed ECS form so no
// stat strings have to be parsed:
//   - eoc::ArmorComponent (base score, ArmorClassAbility, AbilityModifierCap)
//   - eoc::StatsComponent::AbilityModifiers for the ability contribution
//   - BlockAbilityModifierFromAC, which heavy armour applies
//   - every evaluated eoc::ArmorClassBoostComponent reachable through the entity's
//     BoostsContainer (statuses, passives, spells, shields)
//   - the best evaluated eoc::ACOverrideFormulaBoostComponent (Mage Armor,
//     Barkskin), which replaces the base calculation instead of adding to it
// The boost components are already evaluated, so no stat strings are parsed. This
// rebuild cannot see AC that lives on worn items, which is exactly why it is never
// allowed to override the engine total.

struct ArmorClassInfo
{
    int Total{ 0 };
    int EngineAC{ 0 };
    int Computed{ 0 };
    int Base{ 0 };
    AbilityId Ability{ AbilityId::None };
    int RawAbilityModifier{ 0 };
    int AbilityModifier{ 0 };
    int AbilityModifierCap{ 0 };
    bool AbilityBlocked{ false };
    int Boosts{ 0 };
    int BoostCount{ 0 };
    int OverrideAC{ 0 };
    bool HasArmor{ false };
    bool FromEngine{ false };
};

ArmorClassInfo GetArmorClassInfo(lua_State* L, EntityHandle handle)
{
    ArmorClassInfo info;
    auto ecs = State::FromLua(L)->GetEntitySystemHelpers();

    auto armor = ecs->GetComponent<ArmorComponent>(handle);
    if (armor != nullptr) {
        info.HasArmor = true;
        info.Base = armor->ArmorClass;
        info.AbilityModifierCap = armor->AbilityModifierCap;
        info.Ability = armor->ArmorClassAbility < 7
            ? (AbilityId)armor->ArmorClassAbility
            : AbilityId::Dexterity;
        if (info.Ability == AbilityId::None) {
            // Unarmored entities leave the ability unset; the engine adds Dexterity.
            info.Ability = AbilityId::Dexterity;
        }
    }

    auto resistances = ecs->GetComponent<ResistancesComponent>(handle);
    if (resistances != nullptr) {
        info.EngineAC = resistances->AC;
    }

    info.AbilityBlocked = ecs->GetComponent<BlockAbilityModifierFromACComponent>(handle) != nullptr;

    auto stats = ecs->GetComponent<StatsComponent>(handle);
    if (stats != nullptr) {
        auto index = (size_t)info.Ability;
        if (index < stats->AbilityModifiers.size()) {
            info.RawAbilityModifier = stats->AbilityModifiers[index];
        }
    }

    info.AbilityModifier = info.RawAbilityModifier;
    if (info.AbilityBlocked) {
        info.AbilityModifier = 0;
    } else if (info.AbilityModifier > info.AbilityModifierCap) {
        info.AbilityModifier = info.AbilityModifierCap;
    }

    auto container = ecs->GetComponent<BoostsContainerComponent>(handle);
    if (container != nullptr) {
        for (auto const& entry : container->Boosts) {
            if (entry.Type == BoostType::AC) {
                for (auto const& boostHandle : entry.Boosts) {
                    auto ac = ecs->GetComponent<ArmorClassBoostComponent>(boostHandle);
                    if (ac != nullptr) {
                        info.Boosts += ac->AC;
                        info.BoostCount++;
                    }
                }
            } else if (entry.Type == BoostType::ACOverrideFormula) {
                // Mage Armor / Barkskin / worn body armour: replaces the base
                // score instead of adding to it.
                for (auto const& boostHandle : entry.Boosts) {
                    auto formula = ecs->GetComponent<ACOverrideFormulaBoostComponent>(boostHandle);
                    if (formula == nullptr) {
                        continue;
                    }

                    auto value = formula->AC;
                    for (auto ability : formula->AddAbilityModifiers) {
                        auto index = (size_t)ability;
                        if (stats != nullptr && index < stats->AbilityModifiers.size()) {
                            value += stats->AbilityModifiers[index];
                        }
                    }

                    if (value > info.OverrideAC) {
                        info.OverrideAC = value;
                    }
                }
            }
        }
    }

    auto baseAC = info.Base + info.AbilityModifier;
    if (info.OverrideAC > baseAC) {
        baseAC = info.OverrideAC;
    }
    info.Computed = baseAC + info.Boosts;
    // ResistancesComponent::AC is the engine's own total and is authoritative.
    // Never let the local rebuild override or inflate it; the rebuild is only a
    // diagnostic breakdown, or a last-resort total when the engine field is empty.
    info.Total = info.EngineAC != 0 ? info.EngineAC : info.Computed;
    info.FromEngine = info.EngineAC != 0;

    if (!info.FromEngine && info.HasArmor) {
        // The rebuild cannot see AC that lives on worn items, so a zero engine
        // total means the value we return is incomplete. Say so loudly instead
        // of silently returning a number that looks authoritative.
        WARN_ONCE("eoc::ResistancesComponent.AC is zero for a character; "
            "Total is rebuilt from components and excludes worn item AC");
    }

    return info;
}

int PushArmorClassInfo(lua_State* L, ArmorClassInfo const& info)
{
    lua_createtable(L, 0, 14);

    lua_pushinteger(L, info.Total);
    lua_setfield(L, -2, "Total");
    lua_pushinteger(L, info.EngineAC);
    lua_setfield(L, -2, "EngineAC");
    lua_pushinteger(L, info.Computed);
    lua_setfield(L, -2, "Computed");
    lua_pushinteger(L, info.Base);
    lua_setfield(L, -2, "Base");
    lua_pushinteger(L, info.RawAbilityModifier);
    lua_setfield(L, -2, "RawAbilityModifier");
    lua_pushinteger(L, info.AbilityModifier);
    lua_setfield(L, -2, "AbilityModifier");
    lua_pushinteger(L, info.AbilityModifierCap);
    lua_setfield(L, -2, "AbilityModifierCap");
    lua_pushinteger(L, info.Boosts);
    lua_setfield(L, -2, "Boosts");
    lua_pushinteger(L, info.BoostCount);
    lua_setfield(L, -2, "BoostCount");
    lua_pushinteger(L, info.OverrideAC);
    lua_setfield(L, -2, "OverrideAC");
    lua_pushboolean(L, info.AbilityBlocked);
    lua_setfield(L, -2, "AbilityBlocked");
    lua_pushboolean(L, info.HasArmor);
    lua_setfield(L, -2, "HasArmor");

    push(L, EnumInfo<AbilityId>::Find(info.Ability));
    lua_setfield(L, -2, "Ability");
    lua_pushstring(L, info.FromEngine ? "engine" : "components");
    lua_setfield(L, -2, "Source");

    return 1;
}

UserReturn GetArmorClass(lua_State* L, lua::AnyRef entity)
{
    StackCheck _(L, 1);
    std::optional<EntityHandle> handle;

    switch (lua_type(L, entity.Index)) {
    case LUA_TNIL:
        break;

    case LUA_TSTRING:
        handle = UuidToHandle(L, get<Guid>(L, entity.Index));
        break;

    case LUA_TNUMBER:
        handle = State::FromLua(L)->GetEntitySystemHelpers()->NetIdToEntity(get<NetId>(L, entity.Index));
        break;

    case LUA_TLIGHTCPPOBJECT:
        handle = get<EntityHandle>(L, entity.Index);
        break;

    default:
        return luaL_error(L, "Expected entity GUID, network ID or entity handle, got %s", GetDebugName(L, entity.Index));
    }

    if (!handle) {
        push(L, nullptr);
        return 1;
    }

    auto info = GetArmorClassInfo(L, *handle);
    return PushArmorClassInfo(L, info);
}

HashMap<Guid, EntityHandle> GetAllEntitiesWithUuid(lua_State* L)
{
    OPTICK_EVENT();
    auto mappings = State::FromLua(L)->GetEntitySystemHelpers()->GetSingleton<UuidToHandleMappingComponent>();
    if (mappings) {
        return mappings->Mappings;
    } else {
        return {};
    }
}

Array<EntityHandle> GetAllEntitiesWithComponent(lua_State* L, ExtComponentType component)
{
    OPTICK_EVENT();
    auto ecs = State::FromLua(L)->GetEntitySystemHelpers();
    auto componentType = ecs->GetComponentIndex(component);
    if (!componentType) return {};

    Array<EntityHandle> entities;
    auto world = State::FromLua(L)->GetEntitySystemHelpers()->GetEntityWorld();

    auto const& meta = ecs->GetComponentMeta(component);
    if (meta.SingleComponentQuery != ecs::UndefinedQuery) {
        auto& query = world->Queries.Queries[(unsigned)meta.SingleComponentQuery];
        if (ecs::IsOneFrame(*componentType)) {
            for (auto const& storage : query.EntityStorages.values()) {
                auto pool = storage.Storage->OneFrameComponents.try_get(*componentType);
                if (pool) {
                    std::copy(pool->keys().begin(), pool->keys().end(), std::back_inserter(entities));
                }
            }
        } else {
            for (auto const& storage : query.EntityStorages.values()) {
                std::copy(storage.Storage->InstanceToPageMap.keys().begin(), storage.Storage->InstanceToPageMap.keys().end(), std::back_inserter(entities));
            }
        }
    } else {
        if (ecs::IsOneFrame(*componentType)) {
            for (auto cls : world->Storage->Storages) {
                if (cls->HasOneFrameComponents) {
                    auto pool = cls->OneFrameComponents.try_get(*componentType);
                    if (pool) {
                        std::copy(pool->keys().begin(), pool->keys().end(), std::back_inserter(entities));
                    }
                }
            }
        } else {
            for (auto cls : world->Storage->Storages) {
                if (cls->ComponentTypeToIndex.try_get(*componentType)) {
                    std::copy(cls->InstanceToPageMap.keys().begin(), cls->InstanceToPageMap.keys().end(), std::back_inserter(entities));
                }
            }
        }
    }


    return entities;
}

Array<EntityHandle> GetAllEntities(lua_State* L)
{
    OPTICK_EVENT();
    Array<EntityHandle> entities;

    auto world = State::FromLua(L)->GetEntitySystemHelpers()->GetEntityWorld();
    for (auto cls : world->Storage->Storages) {
        std::copy(cls->InstanceToPageMap.keys().begin(), cls->InstanceToPageMap.keys().end(), std::back_inserter(entities));
    }

    return entities;
}

Array<EntityHandle> GetEntitiesAroundPosition(lua_State* L, glm::vec3 position, float radius, 
    std::optional<bool> includeCharacters, std::optional<bool> includeItems)
{
    auto ecs = State::FromLua(L)->GetEntitySystemHelpers();
    auto grid = ecs->GetSingleton<spatial_grid::DataComponent>();

    if (grid) {
        return grid->Grid.Collect(position, radius, includeCharacters.value_or(true), includeItems.value_or(true));
    } else {
        return {};
    }
}

EntityHandle Create(lua_State* L)
{
    auto world = State::FromLua(L)->GetEntitySystemHelpers()->GetEntityWorld();
    return world->Deferred()->CreateEntityImmediate();
}

bool Destroy(lua_State* L, EntityHandle entity)
{
    auto world = State::FromLua(L)->GetEntitySystemHelpers()->GetEntityWorld();
    return world->Deferred()->DestroyEntity(entity);
}

std::optional<LuaEntitySubscriptionId> Subscribe(lua_State* L, ExtComponentType type, FunctionRef func, std::optional<EntityHandle> entity, std::optional<uint64_t> flags)
{
    return EntityEventHelpers::SubscribeReplication(L, entity ? *entity : EntityHandle{}, type, func.MakePersistent(L), flags);
}

LuaEntitySubscriptionId OnCreate(lua_State* L, ExtComponentType type, FunctionRef func, std::optional<EntityHandle> entity, 
    std::optional<bool> deferred, std::optional<bool> once)
{
    auto flags = ((deferred && *deferred) ? EntityComponentEventFlags::Deferred : (EntityComponentEventFlags)0)
        | ((once && *once) ? EntityComponentEventFlags::Once : (EntityComponentEventFlags)0);
    return EntityEventHelpers::Subscribe(L, entity ? *entity : EntityHandle{}, type, EntityComponentEvent::Create, 
        flags, func.MakePersistent(L));
}

LuaEntitySubscriptionId OnCreateDeferred(lua_State* L, ExtComponentType type, FunctionRef func, std::optional<EntityHandle> entity)
{
    return EntityEventHelpers::Subscribe(L, entity ? *entity : EntityHandle{}, type, EntityComponentEvent::Create, 
        EntityComponentEventFlags::Deferred, func.MakePersistent(L));
}

LuaEntitySubscriptionId OnCreateOnce(lua_State* L, ExtComponentType type, FunctionRef func, std::optional<EntityHandle> entity)
{
    return EntityEventHelpers::Subscribe(L, entity ? *entity : EntityHandle{}, type, EntityComponentEvent::Create, 
        EntityComponentEventFlags::Once, func.MakePersistent(L));
}

LuaEntitySubscriptionId OnCreateDeferredOnce(lua_State* L, ExtComponentType type, FunctionRef func, std::optional<EntityHandle> entity)
{
    return EntityEventHelpers::Subscribe(L, entity ? *entity : EntityHandle{}, type, EntityComponentEvent::Create, 
        EntityComponentEventFlags::Once | EntityComponentEventFlags::Deferred, func.MakePersistent(L));
}

LuaEntitySubscriptionId OnDestroy(lua_State* L, ExtComponentType type, FunctionRef func, std::optional<EntityHandle> entity, 
    std::optional<bool> deferred, std::optional<bool> once)
{
    auto flags = ((deferred && *deferred) ? EntityComponentEventFlags::Deferred : (EntityComponentEventFlags)0)
        | ((once && *once) ? EntityComponentEventFlags::Once : (EntityComponentEventFlags)0);
    return EntityEventHelpers::Subscribe(L, entity ? *entity : EntityHandle{}, type, EntityComponentEvent::Destroy, 
        flags, func.MakePersistent(L));
}

LuaEntitySubscriptionId OnDestroyDeferred(lua_State* L, ExtComponentType type, FunctionRef func, std::optional<EntityHandle> entity)
{
    return EntityEventHelpers::Subscribe(L, entity ? *entity : EntityHandle{}, type, EntityComponentEvent::Destroy, 
        EntityComponentEventFlags::Deferred, func.MakePersistent(L));
}

LuaEntitySubscriptionId OnDestroyOnce(lua_State* L, ExtComponentType type, FunctionRef func, std::optional<EntityHandle> entity)
{
    return EntityEventHelpers::Subscribe(L, entity ? *entity : EntityHandle{}, type, EntityComponentEvent::Destroy, 
        EntityComponentEventFlags::Once, func.MakePersistent(L));
}

LuaEntitySubscriptionId OnSystemUpdate(lua_State* L, ExtSystemType type, FunctionRef func, std::optional<bool> once)
{
    return EntityEventHelpers::SubscribeSystemUpdate(L, type, func.MakePersistent(L), false, once ? *once : false);
}

LuaEntitySubscriptionId OnSystemPostUpdate(lua_State* L, ExtSystemType type, FunctionRef func, std::optional<bool> once)
{
    return EntityEventHelpers::SubscribeSystemUpdate(L, type, func.MakePersistent(L), true, once ? *once : false);
}

LuaEntitySubscriptionId OnDestroyDeferredOnce(lua_State* L, ExtComponentType type, FunctionRef func, std::optional<EntityHandle> entity)
{
    return EntityEventHelpers::Subscribe(L, entity ? *entity : EntityHandle{}, type, EntityComponentEvent::Destroy, 
        EntityComponentEventFlags::Once | EntityComponentEventFlags::Deferred, func.MakePersistent(L));
}

bool Unsubscribe(lua_State* L, LuaEntitySubscriptionId handle)
{
    return EntityEventHelpers::Unsubscribe(L, handle);
}

void SetupTracing(lua_State* L, EntityTracingOptions options)
{
    auto ecs = State::FromLua(L)->GetEntitySystemHelpers();
    auto& opts = ecs->GetTracer().GetOptions();
    opts.TrackECB = options.TrackECB;
    opts.TrackImmediateWorldCache = options.TrackImmediateWorldCache;
    opts.TrackReplication = options.TrackReplication;
    opts.TrackModifications = options.TrackModifications;
    opts.ExcludeModificationTypes.Clear();

    for (auto const& component : options.ExcludeModificationComponents) {
        auto componentTypeIndex = ecs->GetComponentIndex(component);
        if (componentTypeIndex) {
            if (!ecs::IsOneFrame(*componentTypeIndex)) {
                opts.ExcludeModificationTypes.Set((unsigned)*componentTypeIndex);
            }
        }
    }
}

void EnableTracing(lua_State* L, bool enable)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("Entity tracing is only available in developer mode");
        return;
    }

    if (enable) {
        WARN_ONCE("Entity tracing is a development tool designed for tracking entity changes; it should not be used in production!");

        State::FromLua(L)->GetEntitySystemHelpers()->GetTracer().StartTracing();
    } else {
        State::FromLua(L)->GetEntitySystemHelpers()->GetTracer().StopTracing();
    }
}

ecs::ECSChangeLog* GetTrace(lua_State* L)
{
    return &State::FromLua(L)->GetEntitySystemHelpers()->GetTracer().GetLog();
}

void ClearTrace(lua_State* L)
{
    State::FromLua(L)->GetEntitySystemHelpers()->GetTracer().GetLog().Clear();
}

Array<StringView> GetRegisteredComponentTypes(lua_State* L, std::optional<bool> oneFrame, std::optional<bool> mapped)
{
    Array<StringView> types;
    
    auto helpers = State::FromLua(L)->GetEntitySystemHelpers();
    auto const& registry = helpers->GetEntityWorld()->ComponentRegistry_;
    for (unsigned i = 0; i < registry.Bitmask.Size; i++) {
        if (registry.Bitmask[i]) {
            auto const& component = registry.Types[i];
            auto name = helpers->GetComponentName(component.TypeId);
            if (name 
                && (!oneFrame || IsOneFrame(component.TypeId) == *oneFrame)
                && (!mapped || (bool)helpers->GetComponentType(component.TypeId) == *mapped)) {
                types.push_back(*name);
            }
        }
    }

    return types;
}

void RegisterEntityLib()
{
    DECLARE_MODULE(Entity, Both)
    BEGIN_MODULE()
    MODULE_FUNCTION(HandleToUuid)
    MODULE_FUNCTION(UuidToHandle)

    MODULE_FUNCTION(Get)
    MODULE_FUNCTION(GetArmorClass)
    MODULE_FUNCTION(GetAllEntitiesWithUuid)
    MODULE_FUNCTION(GetAllEntitiesWithComponent)
    MODULE_FUNCTION(GetAllEntities)
    MODULE_FUNCTION(GetEntitiesAroundPosition)

    MODULE_FUNCTION(Create)
    MODULE_FUNCTION(Destroy)

    MODULE_FUNCTION(Subscribe)
    MODULE_NAMED_FUNCTION("OnChange", Subscribe)
    MODULE_FUNCTION(OnCreate)
    MODULE_FUNCTION(OnCreateDeferred)
    MODULE_FUNCTION(OnCreateOnce)
    MODULE_FUNCTION(OnCreateDeferredOnce)
    MODULE_FUNCTION(OnDestroy)
    MODULE_FUNCTION(OnDestroyDeferred)
    MODULE_FUNCTION(OnDestroyOnce)
    MODULE_FUNCTION(OnDestroyDeferredOnce)
    MODULE_FUNCTION(OnSystemUpdate)
    MODULE_FUNCTION(OnSystemPostUpdate)
    MODULE_FUNCTION(Unsubscribe)

    MODULE_FUNCTION(SetupTracing)
    MODULE_FUNCTION(EnableTracing)
    MODULE_FUNCTION(GetTrace)
    MODULE_FUNCTION(ClearTrace)

    MODULE_FUNCTION(GetRegisteredComponentTypes)
    END_MODULE()
}

END_NS()
