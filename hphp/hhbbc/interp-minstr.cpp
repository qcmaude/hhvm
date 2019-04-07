/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include "hphp/hhbbc/interp.h"

#include <vector>
#include <algorithm>
#include <string>
#include <utility>

#include <folly/Optional.h>
#include <folly/Format.h>

#include "hphp/util/trace.h"

#include "hphp/hhbbc/interp-internal.h"
#include "hphp/hhbbc/type-ops.h"

namespace HPHP { namespace HHBBC {

namespace {

//////////////////////////////////////////////////////////////////////

const StaticString s_stdClass("stdClass");

//////////////////////////////////////////////////////////////////////

/*
 * Note: the couldBe comparisons here with sempty() are asking "can this string
 * be a non-reference counted empty string".  What actually matters is whether
 * it can be an empty string at all.  Currently, all reference counted strings
 * are TStr, which has no values and may also be non-reference
 * counted---emptiness isn't separately tracked like it is for arrays, so if
 * anything happened that could make it reference counted this check will
 * return true.
 *
 * This means this code is fine for now, but if we implement #3837503
 * (non-static strings with values in the type system) it will need to change.
 */

bool couldBeEmptyish(const Type& ty) {
  return ty.couldBe(BNull | BFalse) || ty.couldBe(sempty());
}

bool mustBeEmptyish(const Type& ty) {
  return ty.subtypeOf(BNull | BFalse) || ty.subtypeOf(sempty());
}

bool elemCouldPromoteToArr(const Type& ty) { return couldBeEmptyish(ty); }
bool elemMustPromoteToArr(const Type& ty)  { return mustBeEmptyish(ty); }

bool propCouldPromoteToObj(const Type& ty) {
  return RuntimeOption::EvalPromoteEmptyObject && couldBeEmptyish(ty);
}

bool propMustPromoteToObj(const Type& ty)  {
  return RuntimeOption::EvalPromoteEmptyObject && mustBeEmptyish(ty);
}

bool keyCouldBeWeird(const Type& key) {
  return key.couldBe(BObj | BArr | BVec | BDict | BKeyset);
}

bool mustBeArrLike(const Type& ty) {
  return ty.subtypeOf(BArr | BVec | BDict | BKeyset);
}

//////////////////////////////////////////////////////////////////////

Type baseLocNameType(const Base& b) {
  return b.locName ? sval(b.locName) : TInitGen;
}

//////////////////////////////////////////////////////////////////////

/*
 * A note about bases.
 *
 * Generally type inference needs to know two kinds of things about the base to
 * handle effects on tracked locations:
 *
 *   - Could the base be a location we're tracking deeper structure on, so the
 *     next operation actually affects something inside of it.  For example,
 *     could the base be an object with the same type as $this, or an array in a
 *     local variable.
 *
 *   - Could the base be something (regardless of type) that is inside one of
 *     the things we're tracking.  I.e., the base might be whatever (an array or
 *     a bool or something), but living inside a property inside an object with
 *     the same type as $this, or living inside of an array in the local frame.
 *
 * The first cases apply because final operations are going to directly affect
 * the type of these elements.  The second case is because vector operations may
 * change the base at each step if it is a defining instruction.
 *
 * Note that both of these cases can apply to the same base in some cases: you
 * might have an object property on $this that could be an object of the type of
 * $this.
 *
 * The functions below with names "couldBeIn*" detect the second case. The
 * effects on the tracked location in the second case are handled in the
 * functions with names "promoteIn*{Prop,Elem,..}".  The effects for the first
 * case are generally handled in the miFinal op functions.
 *
 * Control flow insensitive vs. control flow sensitive types:
 *
 * Things are also slightly complicated by the fact that we are analyzing some
 * control flow insensitve types along side precisely tracked types.  For
 * effects on locals, we perform the type effects of each operation on
 * base.type, and then allow updateBaseWithType() to make the updates to the
 * local when we know what its final type will be.
 *
 * This approach doesn't do as well for possible properties in $this or self::,
 * because we may see situations where the base could be one of these properties
 * but we're not sure---perhaps because it came off a property with the same
 * name on an object with an unknown type (i.e. base.type is InitCell but
 * couldBeInProp is true).  In these situations, we can get away with just
 * merging Obj=stdClass into the thisProp (because it 'could' promote) instead
 * of merging the whole InitCell, which possibly lets us leave the type at ?Obj
 * in some cases.
 *
 * This is why there's two fairly different mechanisms for handling the effects
 * of defining ops on base types.
 */

//////////////////////////////////////////////////////////////////////

bool couldBeThisObj(ISS& env, const Base& b) {
  auto const thisTy = thisType(env);
  return b.type.couldBe(thisTy ? *thisTy : TObj);
}

bool mustBeThisObj(ISS& env, const Base& b) {
  if (b.loc == BaseLoc::This) return true;
  if (auto const ty = thisType(env)) return b.type.subtypeOf(*ty);
  return false;
}

bool mustBeInLocal(const Base& b) {
  return b.loc == BaseLoc::Local;
}

bool mustBeInStack(const Base& b) {
  return b.loc == BaseLoc::Stack;
}

bool couldBeInProp(ISS& env, const Base& b) {
  if (b.loc != BaseLoc::Prop) return false;
  auto const thisTy = thisType(env);
  if (!thisTy) return true;
  if (!b.locTy.couldBe(*thisTy)) return false;
  if (b.locName) return isTrackedThisProp(env, b.locName);
  return true;
}

bool couldBeInPrivateStatic(ISS& env, const Base& b) {
  if (b.loc != BaseLoc::StaticProp) return false;
  auto const selfTy = selfCls(env);
  return !selfTy || b.locTy.couldBe(*selfTy);
}

bool couldBeInPublicStatic(const Base& b) {
  return b.loc == BaseLoc::StaticProp;
}

//////////////////////////////////////////////////////////////////////

// Base locations that only occur at the start of a minstr sequence.
bool isInitialBaseLoc(BaseLoc loc) {
  return
    loc == BaseLoc::Local ||
    loc == BaseLoc::Stack ||
    loc == BaseLoc::StaticProp ||
    loc == BaseLoc::This ||
    loc == BaseLoc::Global;
}

// Base locations that only occur after the start of a minstr sequence.
bool isDimBaseLoc(BaseLoc loc) {
  return loc == BaseLoc::Elem || loc == BaseLoc::Prop;
}

//////////////////////////////////////////////////////////////////////

/*
 * If the current base is an array-like, update it via *_set, and return true;
 * otherwise, return false.
 */
bool array_do_set(ISS& env, const Type& key, const Type& value) {
  auto& base = env.state.mInstrState.base.type;
  auto res = [&] () -> folly::Optional<std::pair<Type,ThrowMode>> {
    if (base.subtypeOf(BArr))    return  array_set(std::move(base), key, value);
    if (base.subtypeOf(BVec))    return    vec_set(std::move(base), key, value);
    if (base.subtypeOf(BDict))   return   dict_set(std::move(base), key, value);
    if (base.subtypeOf(BKeyset)) return keyset_set(std::move(base), key, value);
    return folly::none;
  }();
  if (!res) return false;

  switch (res->second) {
    case ThrowMode::None:
      nothrow(env);
      break;
    case ThrowMode::MaybeMissingElement:
    case ThrowMode::MissingElement:
    case ThrowMode::MaybeBadKey:
    case ThrowMode::BadOperation:
      break;
  }

  if (res->first == TBottom) {
    unreachable(env);
  }

  base = std::move(res->first);
  return true;
}

/*
 * If the current base is an array-like, return the best known type
 * for base[key].
 */
folly::Optional<Type> array_do_elem(ISS& env,
                                    bool nullOnMissing,
                                    const Type& key) {
  auto const& base = env.state.mInstrState.base.type;
  auto res = [&] () -> folly::Optional<std::pair<Type,ThrowMode>> {
    if (base.subtypeOf(BArr))    return  array_elem(base, key);
    if (base.subtypeOf(BVec))    return    vec_elem(base, key);
    if (base.subtypeOf(BDict))   return   dict_elem(base, key);
    if (base.subtypeOf(BKeyset)) return keyset_elem(base, key);
    return folly::none;
  }();
  if (!res) return folly::none;

  switch (res->second) {
    case ThrowMode::None:
      nothrow(env);
      break;
    case ThrowMode::MaybeMissingElement:
    case ThrowMode::MissingElement:
      if (nullOnMissing) {
        nothrow(env);
        res->first |= TInitNull;
      }
      break;
    case ThrowMode::MaybeBadKey:
      if (nullOnMissing) {
        res->first |= TInitNull;
      }
      break;
    case ThrowMode::BadOperation:
      break;
  }

  if (res->first == TBottom) {
    unreachable(env);
  }

  return std::move(res->first);
}

/*
 * If the current base is an array-like, update it via *_newelem, and
 * return the best known type for the key added; otherwise return folly::none.
 */
folly::Optional<Type> array_do_newelem(ISS& env, const Type& value) {
  auto& base = env.state.mInstrState.base.type;
  auto res = [&] () -> folly::Optional<std::pair<Type,Type>> {
    if (base.subtypeOf(BArr))    return  array_newelem(std::move(base), value);
    if (base.subtypeOf(BVec))    return    vec_newelem(std::move(base), value);
    if (base.subtypeOf(BDict))   return   dict_newelem(std::move(base), value);
    if (base.subtypeOf(BKeyset)) return keyset_newelem(std::move(base), value);
    return folly::none;
  }();
  if (!res) return folly::none;
  base = std::move(res->first);
  return res->second;
}

//////////////////////////////////////////////////////////////////////

// MInstrs can throw in between each op, so the states of locals
// need to be propagated across throw exit edges.
void miThrow(ISS& env) {
  for (auto exit : env.blk.throwExits) {
    auto const stackLess = with_throwable_only(env.index, env.state);
    env.propagate(exit, &stackLess);
  }
}

//////////////////////////////////////////////////////////////////////

void setLocalForBase(ISS& env, Type ty, LocalId firstKeyLoc) {
  assert(mustBeInLocal(env.state.mInstrState.base));
  if (env.state.mInstrState.base.locLocal == NoLocalId) {
    return loseNonRefLocalTypes(env);
  }
  FTRACE(4, "      ${} := {}\n",
    env.state.mInstrState.base.locName
     ? env.state.mInstrState.base.locName->data()
     : "$<unnamed>",
    show(ty)
  );
  setLoc(
    env,
    env.state.mInstrState.base.locLocal,
    std::move(ty),
    firstKeyLoc
  );
}

void setStackForBase(ISS& env, Type ty) {
  assert(mustBeInStack(env.state.mInstrState.base));

  auto const locSlot = env.state.mInstrState.base.locSlot;
  FTRACE(4, "      stk[{:02}] := {}\n", locSlot, show(ty));
  assert(locSlot < env.state.stack.size());

  auto const& oldTy = env.state.stack[locSlot].type;
  if (oldTy.subtypeOf(BInitCell)) {
    env.state.stack[locSlot] = StackElem { std::move(ty), NoLocalId };
  }
}

void setPrivateStaticForBase(ISS& env, Type ty) {
  assert(couldBeInPrivateStatic(env, env.state.mInstrState.base));

  if (auto const name = env.state.mInstrState.base.locName) {
    FTRACE(4, "      self::${} |= {}\n", name->data(), show(ty));
    mergeSelfProp(env, name, std::move(ty));
    return;
  }
  FTRACE(4, "      self::* |= {}\n", show(ty));
  mergeEachSelfPropRaw(
    env,
    [&](const Type& old){ return old.subtypeOf(BInitCell) ? ty : TBottom; }
  );
}

void setPublicStaticForBase(ISS& env, Type ty) {
  auto const& base = env.state.mInstrState.base;
  assertx(couldBeInPublicStatic(base));

  auto const nameTy = base.locName ? sval(base.locName) : TStr;
  FTRACE(
    4, "      public ({})::$({}) |= {}\n",
    show(base.locTy), show(nameTy), show(ty)
  );
  env.collect.publicSPropMutations.merge(
    env.index, env.ctx, base.locTy, nameTy, ty
  );
}

// Run backwards through an array chain doing array_set operations
// to produce the array type that incorporates the effects of any
// intermediate defining dims.
Type currentChainType(ISS& env, Type val) {
  auto it = env.state.mInstrState.arrayChain.end();
  while (it != env.state.mInstrState.arrayChain.begin()) {
    --it;
    if (it->base.subtypeOf(BArr)) {
      val = array_set(it->base, it->key, val).first;
    } else if (it->base.subtypeOf(BVec)) {
      val = vec_set(it->base, it->key, val).first;
      if (val == TBottom) val = TVec;
    } else if (it->base.subtypeOf(BDict)) {
      val = dict_set(it->base, it->key, val).first;
      if (val == TBottom) val = TDict;
    } else {
      assert(it->base.subtypeOf(BKeyset));
      val = keyset_set(it->base, it->key, val).first;
      if (val == TBottom) val = TKeyset;
    }
  }
  return val;
}

Type resolveArrayChain(ISS& env, Type val) {
  static UNUSED const char prefix[] = "              ";
  FTRACE(5, "{}chain\n", prefix, show(val));
  do {
    auto arr = std::move(env.state.mInstrState.arrayChain.back().base);
    auto key = std::move(env.state.mInstrState.arrayChain.back().key);
    env.state.mInstrState.arrayChain.pop_back();
    FTRACE(5, "{}  | {} := {} in {}\n", prefix,
      show(key), show(val), show(arr));
    if (arr.subtypeOf(BVec)) {
      val = vec_set(std::move(arr), key, val).first;
      if (val == TBottom) val = TVec;
    } else if (arr.subtypeOf(BDict)) {
      val = dict_set(std::move(arr), key, val).first;
      if (val == TBottom) val = TDict;
    } else if (arr.subtypeOf(BKeyset)) {
      val = keyset_set(std::move(arr), key, val).first;
      if (val == TBottom) val = TKeyset;
    } else {
      assert(arr.subtypeOf(BArr));
      val = array_set(std::move(arr), key, val).first;
    }
  } while (!env.state.mInstrState.arrayChain.empty());
  FTRACE(5, "{}  = {}\n", prefix, show(val));
  return val;
}

void updateBaseWithType(ISS& env,
                        const Type& ty,
                        LocalId firstKeyLoc = NoLocalId) {
  FTRACE(6, "    updateBaseWithType: {}\n", show(ty));

  auto const& base = env.state.mInstrState.base;

  if (mustBeInLocal(base)) {
    setLocalForBase(env, ty, firstKeyLoc);
    return miThrow(env);
  }
  if (mustBeInStack(base)) {
    return setStackForBase(env, ty);
  }

  if (couldBeInPrivateStatic(env, base)) setPrivateStaticForBase(env, ty);
  if (couldBeInPublicStatic(base))       setPublicStaticForBase(env, ty);
}

void startBase(ISS& env, Base base) {
  auto& oldState = env.state.mInstrState;
  assert(oldState.base.loc == BaseLoc::None);
  assert(oldState.arrayChain.empty());
  assert(isInitialBaseLoc(base.loc));

  oldState.base = std::move(base);
  FTRACE(5, "    startBase: {}\n", show(*env.ctx.func, oldState.base));
}

void endBase(ISS& env, bool update = true, LocalId keyLoc = NoLocalId) {
  auto& state = env.state.mInstrState;
  assert(state.base.loc != BaseLoc::None);

  FTRACE(5, "    endBase: {}\n", show(*env.ctx.func, state.base));

  auto const firstKeyLoc = state.arrayChain.empty()
    ? keyLoc
    : state.arrayChain.data()->keyLoc;
  auto const& ty = state.arrayChain.empty()
    ? state.base.type
    : resolveArrayChain(env, state.base.type);

  if (update) updateBaseWithType(env, ty, firstKeyLoc);
  state.base.loc = BaseLoc::None;
}

void moveBase(ISS& env,
              Base newBase,
              bool update = true,
              LocalId keyLoc = NoLocalId) {
  auto& state = env.state.mInstrState;
  assert(state.base.loc != BaseLoc::None);
  assert(isDimBaseLoc(newBase.loc));

  FTRACE(5, "    moveBase: {} -> {}\n",
         show(*env.ctx.func, state.base),
         show(*env.ctx.func, newBase));

  if (newBase.loc == BaseLoc::Elem) {
    state.base.type =
      add_nonemptiness(
        loosen_staticness(loosen_values(state.base.type))
      );
  }

  auto const firstKeyLoc = state.arrayChain.empty()
    ? keyLoc
    : state.arrayChain.data()->keyLoc;
  auto const& ty = state.arrayChain.empty()
    ? state.base.type
    : resolveArrayChain(env, state.base.type);

  if (update) updateBaseWithType(env, ty, firstKeyLoc);
  state.base = std::move(newBase);
}

void extendArrChain(ISS& env, Type key, Type arr,
                    Type val, bool update = true,
                    LocalId keyLoc = NoLocalId) {
  auto& state = env.state.mInstrState;
  assert(state.base.loc != BaseLoc::None);
  assert(mustBeArrLike(arr));

  state.arrayChain.emplace_back(
    State::MInstrState::ArrayChainEnt{std::move(arr), std::move(key), keyLoc}
  );
  state.base.type = std::move(val);

  auto const firstKeyLoc = state.arrayChain.data()->keyLoc;

  FTRACE(5, "    extendArrChain: {}\n", show(*env.ctx.func, state));
  if (update) {
    updateBaseWithType(
      env,
      currentChainType(env, state.base.type),
      firstKeyLoc
    );
  }
}

//////////////////////////////////////////////////////////////////////

/*
 * The following promoteBase{Elem,Prop}* functions are used to implement the
 * 'normal' portion of the effects on base types, which are mostly what are done
 * by intermediate dims.
 *
 * The contract with these functions is that they should handle all the effects
 * on the base type /except/ for the case of the base being an array
 * subtype---the caller is responsible for that. The reason for this is that for
 * tracking effects on specialized array types, the final ops generally need to
 * do completely different things to the array, so this allows reuse of this
 * shared part of the type transitions. The intermediate routines must handle
 * array subtypes outside of calls to this as well.
 */

void promoteBaseElemU(ISS& env) {
  // We're conservative with unsets on array types for now.
  env.state.mInstrState.base.type = loosen_all(env.state.mInstrState.base.type);
}

void promoteBasePropD(ISS& env, bool isNullsafe) {
  auto& ty = env.state.mInstrState.base.type;

  // NullSafe (Q) props do not promote an emptyish base to stdClass instance.
  if (isNullsafe || ty.subtypeOf(BObj)) return;

  if (propMustPromoteToObj(ty)) {
    ty = objExact(env.index.builtin_class(s_stdClass.get()));
    return;
  }
  if (propCouldPromoteToObj(ty)) {
    ty = promote_emptyish(ty, TObj);
    return;
  }
}

void promoteBaseElemD(ISS& env) {
  auto& ty = env.state.mInstrState.base.type;

  // When the base is actually a subtype of array, we handle it in the callers
  // of these functions.
  if (mustBeArrLike(ty)) return;

  if (elemMustPromoteToArr(ty)) {
    ty = some_aempty();
    return;
  }

  // Intermediate ElemD operations on strings fatal, unless the string is empty,
  // which promotes to array. So for any string here we can assume it promoted
  // to an empty array.
  if (ty.subtypeOf(BStr)) {
    ty = some_aempty();
    return;
  }

  if (elemCouldPromoteToArr(ty)) {
    ty = promote_emptyish(ty, some_aempty());
    return;
  }

  /*
   * If the base still could be some kind of array (but isn't an array sub-type
   * which would be handled outside this routine), we need to give up on any
   * better information here (or track the effects, but we're not doing that
   * yet).
   */
  ty = loosen_arrays(ty);
}

void promoteBaseNewElem(ISS& env) {
  promoteBaseElemD(env);
  // Technically we don't need to do TStr case.
}

//////////////////////////////////////////////////////////////////////

void handleInPublicStaticElemD(ISS& env) {
  auto const& base = env.state.mInstrState.base;
  if (!couldBeInPublicStatic(base)) return;

  auto const name = baseLocNameType(base);
  auto const ty = env.index.lookup_public_static(env.ctx, base.locTy, name);
  if (elemCouldPromoteToArr(ty)) {
    // Might be possible to only merge a TArrE, but for now this is ok.
    env.collect.publicSPropMutations.merge(
      env.index, env.ctx, base.locTy, name, TArr
    );
  }
}

void handleInThisElemD(ISS& env) {
  if (!couldBeInProp(env, env.state.mInstrState.base)) return;

  if (auto const name = env.state.mInstrState.base.locName) {
    auto const ty = thisPropAsCell(env, name);
    if (ty && elemCouldPromoteToArr(*ty)) {
      mergeThisProp(env, name, TArr);
    }
    return;
  }

  mergeEachThisPropRaw(env, [&] (const Type& t) {
      return elemCouldPromoteToArr(t) ? TArr : TBottom;
    });
}

void handleInPublicStaticPropD(ISS& env, bool isNullsafe) {
  // NullSafe (Q) props do not promote an emptyish base to stdClass instance.
  if (isNullsafe) return;

  auto const& base = env.state.mInstrState.base;
  if (!couldBeInPublicStatic(base)) return;

  auto const name = baseLocNameType(base);
  auto const ty = env.index.lookup_public_static(env.ctx, base.locTy, name);
  if (propCouldPromoteToObj(ty)) {
    env.collect.publicSPropMutations.merge(
      env.index, env.ctx, base.locTy, name,
      objExact(env.index.builtin_class(s_stdClass.get()))
    );
  }
}

void handleInThisPropD(ISS& env, bool isNullsafe) {
  // NullSafe (Q) props do not promote an emptyish base to stdClass instance.
  if (isNullsafe) return;

  if (!couldBeInProp(env, env.state.mInstrState.base)) return;

  if (auto const name = env.state.mInstrState.base.locName) {
    auto const ty = thisPropAsCell(env, name);
    if (ty && propCouldPromoteToObj(*ty)) {
      mergeThisProp(env, name,
                    objExact(env.index.builtin_class(s_stdClass.get())));
    }
    return;
  }

  if (RuntimeOption::EvalPromoteEmptyObject) {
    mergeEachThisPropRaw(env, [&] (const Type& t) {
        return propCouldPromoteToObj(t) ? TObj : TBottom;
      });
  }
}

// Currently NewElem and Elem InFoo effects don't need to do
// anything different from each other.
void handleInPublicStaticNewElem(ISS& env) { handleInPublicStaticElemD(env); }
void handleInThisNewElem(ISS& env) { handleInThisElemD(env); }

void handleInPublicStaticElemU(ISS& env) {
  auto const& base = env.state.mInstrState.base;
  if (!couldBeInPublicStatic(base)) return;

  /*
   * Merging InitCell is correct, but very conservative, for now.
   */
  auto const name = baseLocNameType(base);
  env.collect.publicSPropMutations.merge(
    env.index, env.ctx, base.locTy, name, TInitCell
  );
}

//////////////////////////////////////////////////////////////////////

// Returns nullptr if it's an unknown key or not a string.
SString mStringKey(const Type& key) {
  auto const v = tv(key);
  return v && v->m_type == KindOfPersistentString ? v->m_data.pstr : nullptr;
}

template<typename Op>
folly::Optional<Type> key_type_or_fixup(ISS& env, Op op) {
  auto fixup = [&] (Type ty, bool isProp) -> folly::Optional<Type> {
    if (auto const val = tv(ty)) {
      if (isStringType(val->m_type)) {
        op.mkey.mcode = isProp ? MPT : MET;
        op.mkey.litstr = val->m_data.pstr;
        reduce(env, op);
        return folly::none;
      }
      if (!isProp && val->m_type == KindOfInt64) {
        op.mkey.mcode = MEI;
        op.mkey.int64 = val->m_data.num;
        reduce(env, op);
        return folly::none;
      }
    }
    return std::move(ty);
  };
  switch (op.mkey.mcode) {
    case MEC: case MPC:
      return fixup(topC(env, op.mkey.idx), op.mkey.mcode == MPC);
    case MEL: case MPL:
      return fixup(locAsCell(env, op.mkey.local), op.mkey.mcode == MPL);
    case MW:
      return TBottom;
    case MEI:
      return ival(op.mkey.int64);
    case MET: case MPT: case MQT:
      return sval(op.mkey.litstr);
  }
  not_reached();
}

template<typename Op>
LocalId key_local(ISS& env, Op op) {
  switch (op.mkey.mcode) {
    case MEC: case MPC:
      return topStkLocal(env, op.mkey.idx);
    case MEL: case MPL:
      return op.mkey.local;
    case MW:
    case MEI:
    case MET: case MPT: case MQT:
      return NoLocalId;
  }
  not_reached();
}

//////////////////////////////////////////////////////////////////////
// base ops

Base miBaseLocal(ISS& env, LocalId locBase, MOpMode mode) {
  auto const locName = env.ctx.func->locals[locBase].name;
  auto const isDefine = mode == MOpMode::Define;
  if (mode == MOpMode::None ||
      (mode == MOpMode::Warn && !locCouldBeUninit(env, locBase))) {
    nothrow(env);
  }
  // If we're changing the local to define it, we don't need to do an miThrow
  // yet---the promotions (to array or stdClass) on previously uninitialized
  // locals happen before raising warnings that could throw, so we can wait
  // until the first moveBase.
  return Base { isDefine ? locAsCell(env, locBase) : derefLoc(env, locBase),
                BaseLoc::Local,
                TBottom,
                locName,
                locBase };
}

Base miBaseSProp(ISS& env, Type cls, const Type& tprop) {
  auto const self = selfCls(env);
  auto const prop = tv(tprop);
  auto const name = prop && prop->m_type == KindOfPersistentString
                      ? prop->m_data.pstr : nullptr;
  if (self && cls.subtypeOf(*self) && name) {
    if (auto ty = selfPropAsCell(env, name)) {
      return
        Base { std::move(*ty), BaseLoc::StaticProp, std::move(cls), name };
    }
  }
  auto indexTy = env.index.lookup_public_static(env.ctx, cls, tprop);
  if (!indexTy.subtypeOf(BInitCell)) indexTy = TInitCell;
  return Base { std::move(indexTy),
                BaseLoc::StaticProp,
                std::move(cls),
                name };
}

//////////////////////////////////////////////////////////////////////
// intermediate ops

void miProp(ISS& env, bool isNullsafe, MOpMode mode, Type key) {
  auto const name     = mStringKey(key);
  auto const isDefine = mode == MOpMode::Define;
  auto const isUnset  = mode == MOpMode::Unset;
  // PHP5 doesn't modify an unset local if you unset a property or
  // array elem on it, but hhvm does (it promotes it to init-null).
  auto const update = isDefine || isUnset;
  /*
   * MOpMode::Unset Props doesn't promote "emptyish" things to stdClass, or
   * affect arrays, however it can define a property on an object base.  This
   * means we don't need any couldBeInFoo logic, but if the base could actually
   * be $this, and a declared property could be Uninit, we need to merge
   * InitNull.
   *
   * We're trying to handle this case correctly as far as the type inference
   * here is concerned, but the runtime doesn't actually behave this way right
   * now for declared properties.  Note that it never hurts to merge more types
   * than a thisProp could actually be, so this is fine.
   *
   * See TODO(#3602740): unset with intermediate dims on previously declared
   * properties doesn't define them to null.
   */
  if (isUnset && couldBeThisObj(env, env.state.mInstrState.base)) {
    if (name) {
      auto const elem = thisPropRaw(env, name);
      if (elem && elem->ty.couldBe(BUninit)) {
        mergeThisProp(env, name, TInitNull);
      }
    } else {
      mergeEachThisPropRaw(env, [&] (const Type& ty) {
        return ty.couldBe(BUninit) ? TInitNull : TBottom;
      });
    }
  }

  if (isDefine) {
    handleInThisPropD(env, isNullsafe);
    handleInPublicStaticPropD(env, isNullsafe);
    promoteBasePropD(env, isNullsafe);
  }

  if (mustBeThisObj(env, env.state.mInstrState.base)) {
    auto const optThisTy = thisType(env);
    auto const thisTy    = optThisTy ? *optThisTy : TObj;
    if (name) {
      auto const ty = [&] {
        if (auto const propTy = thisPropAsCell(env, name)) return *propTy;
        return to_cell(
          env.index.lookup_public_prop(objcls(thisTy), sval(name))
        );
      }();

      moveBase(
        env,
        Base { ty, BaseLoc::Prop, thisTy, name },
        update
      );
    } else {
      moveBase(env,
               Base { TInitCell, BaseLoc::Prop, thisTy },
               update);
    }
    return;
  }

  // We know for sure we're going to be in an object property.
  if (env.state.mInstrState.base.type.subtypeOf(BObj)) {
    auto const ty = to_cell(
      env.index.lookup_public_prop(
        objcls(env.state.mInstrState.base.type),
        name ? sval(name) : TStr
      )
    );
    moveBase(env,
             Base { ty,
                    BaseLoc::Prop,
                    env.state.mInstrState.base.type,
                    name },
             update);
    return;
  }

  /*
   * Otherwise, intermediate props with define can promote a null, false, or ""
   * to stdClass.  Those cases, and others, if it's MOpMode::Define, will set
   * the base to a null value in tvScratch.  The base may also legitimately be
   * an object and our next base is in an object property. Conservatively treat
   * all these cases as "possibly" being inside of an object property with
   * "Prop" with locType TTop.
   */
  moveBase(env,
           Base { TInitCell, BaseLoc::Prop, TTop, name },
           update);
}

void miElem(ISS& env, MOpMode mode, Type key, LocalId keyLoc) {
  auto const isDefine = mode == MOpMode::Define;
  auto const isUnset  = mode == MOpMode::Unset;
  auto const update = isDefine || isUnset;

  if (isUnset) {
    /*
     * Elem dims with MOpMode::Unset can change a base from a static array into
     * a reference counted array.  It never promotes emptyish types, however.
     */
    handleInPublicStaticElemU(env);
    promoteBaseElemU(env);

    if (auto ty = array_do_elem(env, false, key)) {
      moveBase(
        env,
        Base { std::move(*ty), BaseLoc::Elem, env.state.mInstrState.base.type },
        update,
        keyLoc
      );
      return;
    }
  }

  if (isDefine) {
    handleInThisElemD(env);
    handleInPublicStaticElemD(env);
    promoteBaseElemD(env);
  }

  if (auto ty = array_do_elem(env, mode == MOpMode::None, key)) {
    extendArrChain(
      env, std::move(key), env.state.mInstrState.base.type,
      std::move(*ty),
      update,
      keyLoc
    );
    return;
  }

  if (env.state.mInstrState.base.type.subtypeOf(BStr)) {
    moveBase(env, Base { TStr, BaseLoc::Elem }, update);
    return;
  }

  /*
   * Other cases could leave the base as anything (if nothing else, via
   * ArrayAccess on an object).
   *
   * The resulting BaseLoc is either inside an array, is the global
   * init_null_variant, or inside tvScratch.  We represent this with the
   * Elem base location with locType TTop.
   */
  moveBase(env, Base { TInitCell, BaseLoc::Elem, TTop }, update, keyLoc);
}

void miNewElem(ISS& env) {
  handleInThisNewElem(env);
  handleInPublicStaticNewElem(env);
  promoteBaseNewElem(env);

  if (auto kty = array_do_newelem(env, TInitNull)) {
    extendArrChain(env,
                   std::move(*kty),
                   std::move(env.state.mInstrState.base.type),
                   TInitNull);
  } else {
    moveBase(env, Base { TInitCell, BaseLoc::Elem, TTop });
  }
}

//////////////////////////////////////////////////////////////////////
// final prop ops

void miFinalIssetProp(ISS& env, int32_t nDiscard, const Type& key) {
  auto const name = mStringKey(key);
  discard(env, nDiscard);
  if (name && mustBeThisObj(env, env.state.mInstrState.base)) {
    if (auto const pt = thisPropAsCell(env, name)) {
      if (isMaybeLateInitThisProp(env, name)) {
        // LateInit props can always be maybe unset, except if its never set at
        // all.
        return push(env, pt->subtypeOf(BBottom) ? TFalse : TBool);
      }
      if (pt->subtypeOf(BNull))  return push(env, TFalse);
      if (!pt->couldBe(BNull))   return push(env, TTrue);
    }
  }
  push(env, TBool);
}

void miFinalCGetProp(ISS& env, int32_t nDiscard, const Type& key) {
  auto const name = mStringKey(key);
  discard(env, nDiscard);

  auto const ty = [&] {
    if (name) {
      if (mustBeThisObj(env, env.state.mInstrState.base)) {
        if (auto const t = thisPropAsCell(env, name)) return *t;
      }
      return to_cell(
        env.index.lookup_public_prop(
          objcls(env.state.mInstrState.base.type), sval(name)
        )
      );
    }
    return TInitCell;
  }();
  if (ty.subtypeOf(BBottom)) unreachable(env);
  push(env, ty);
}

void miFinalVGetProp(ISS& env, int32_t nDiscard,
                     const Type& key, bool isNullsafe) {
  auto const name = mStringKey(key);
  handleInThisPropD(env, isNullsafe);
  handleInPublicStaticPropD(env, isNullsafe);
  promoteBasePropD(env, isNullsafe);
  if (couldBeThisObj(env, env.state.mInstrState.base)) {
    if (name) {
      boxThisProp(env, name);
    } else {
      killThisProps(env);
    }
  }
  endBase(env);
  discard(env, nDiscard);
  push(env, TRef);
}

void miFinalSetProp(ISS& env, int32_t nDiscard, const Type& key) {
  auto const name = mStringKey(key);
  auto const t1 = popC(env);

  auto const finish = [&](Type ty) {
    endBase(env);
    discard(env, nDiscard);
    push(env, std::move(ty));
  };

  handleInThisPropD(env, false);
  handleInPublicStaticPropD(env, false);
  promoteBasePropD(env, false);

  if (couldBeThisObj(env, env.state.mInstrState.base)) {
    if (!name) {
      mergeEachThisPropRaw(
        env,
        [&] (Type propTy) {
          return propTy.couldBe(BInitCell) ? t1 : TBottom;
        }
      );
    } else {
      mergeThisProp(env, name, t1);
    }
  }

  if (env.state.mInstrState.base.type.subtypeOf(BObj)) {
    moveBase(
      env,
      Base { t1, BaseLoc::Prop, env.state.mInstrState.base.type, name }
    );
    return finish(t1);
  }

  moveBase(env, Base { TInitCell, BaseLoc::Prop, TTop, name });
  return finish(TInitCell);
}

void miFinalSetOpProp(ISS& env, int32_t nDiscard,
                      SetOpOp subop, const Type& key) {
  auto const name = mStringKey(key);
  auto const rhsTy = popC(env);

  handleInThisPropD(env, false);
  handleInPublicStaticPropD(env, false);
  promoteBasePropD(env, false);

  auto const lhsTy = [&] {
    if (name) {
      if (mustBeThisObj(env, env.state.mInstrState.base)) {
        if (auto const t = thisPropAsCell(env, name)) return *t;
      }
      return to_cell(
        env.index.lookup_public_prop(
          objcls(env.state.mInstrState.base.type), sval(name)
        )
      );
    }
    return TInitCell;
  }();

  auto const resultTy = env.state.mInstrState.base.type.subtypeOf(TObj)
    ? typeSetOp(subop, lhsTy, rhsTy)
    : TInitCell;
  if (couldBeThisObj(env, env.state.mInstrState.base)) {
    if (name) {
      mergeThisProp(env, name, resultTy);
    } else {
      loseNonRefThisPropTypes(env);
    }
  }

  endBase(env);
  discard(env, nDiscard);
  push(env, resultTy);
}

void miFinalIncDecProp(ISS& env, int32_t nDiscard,
                       IncDecOp subop, const Type& key) {
  auto const name = mStringKey(key);

  handleInThisPropD(env, false);
  handleInPublicStaticPropD(env, false);
  promoteBasePropD(env, false);

  auto const postPropTy = [&] {
    if (name) {
      if (mustBeThisObj(env, env.state.mInstrState.base)) {
        if (auto const t = thisPropAsCell(env, name)) return *t;
      }
      return to_cell(
        env.index.lookup_public_prop(
          objcls(env.state.mInstrState.base.type), sval(name)
        )
      );
    }
    return TInitCell;
  }();
  auto const prePropTy = env.state.mInstrState.base.type.subtypeOf(TObj)
    ? typeIncDec(subop, postPropTy)
    : TInitCell;

  if (couldBeThisObj(env, env.state.mInstrState.base)) {
    if (name) {
      mergeThisProp(env, name, prePropTy);
    } else {
      loseNonRefThisPropTypes(env);
    }
  }

  endBase(env);
  discard(env, nDiscard);
  push(env, isPre(subop) ? prePropTy : postPropTy);
}

void miFinalUnsetProp(ISS& env, int32_t nDiscard, const Type& key) {
  auto const name = mStringKey(key);

  /*
   * Unset does define intermediate dims but with slightly different
   * rules than sets.  It only applies to object properties.
   *
   * Note that this can't affect self props, because static
   * properties can never be unset.  It also can't change anything
   * about an inner array type.
   */
  handleInThisPropD(env, false);

  if (couldBeThisObj(env, env.state.mInstrState.base)) {
    if (name) {
      unsetThisProp(env, name);
    } else {
      unsetUnknownThisProp(env);
    }
  }

  endBase(env);
  discard(env, nDiscard);
}

//////////////////////////////////////////////////////////////////////
// Final elem ops

// This is a helper for final defining Elem operations that need to
// handle array chains and frame effects, but don't yet do anything
// better than supplying a single type.
void pessimisticFinalElemD(ISS& env, const Type& key, const Type& ty) {
  array_do_set(env, key, ty);
}

template<typename F>
void miFinalCGetElem(ISS& env, int32_t nDiscard,
                     const Type& key, bool nullOnMissing,
                     F transform) {
  auto ty = [&] {
    if (auto type = array_do_elem(env, nullOnMissing, key)) {
      return std::move(*type);
    }
    return TInitCell;
  }();
  discard(env, nDiscard);
  push(env, transform(std::move(ty)));
}

void miFinalVGetElem(ISS& env, int32_t nDiscard, const Type& key) {
  handleInThisElemD(env);
  handleInPublicStaticElemD(env);
  promoteBaseElemD(env);
  pessimisticFinalElemD(env, key, TInitGen);

  auto const finish = [&](Type ty) {
    endBase(env);
    discard(env, nDiscard);
    push(env, std::move(ty));
  };

  auto const& baseTy = env.state.mInstrState.base.type;
  if (baseTy.subtypeOf(BVec) ||
      baseTy.subtypeOf(BDict) ||
      baseTy.subtypeOf(BKeyset)) {
    unreachable(env);
    return finish(TBottom);
  }
  finish(TRef);
}

void miFinalSetElem(ISS& env,
                    int32_t nDiscard,
                    const Type& key,
                    LocalId keyLoc) {
  auto const t1  = popC(env);

  handleInThisElemD(env);
  handleInPublicStaticElemD(env);

  auto const finish = [&](Type ty) {
    endBase(env, true, keyLoc);
    discard(env, nDiscard);
    push(env, std::move(ty));
  };

  // Note: we must handle the string-related cases before doing the
  // general handleBaseElemD, since operates on strings as if this
  // was an intermediate ElemD.
  if (env.state.mInstrState.base.type.subtypeOf(sempty())) {
    env.state.mInstrState.base.type = some_aempty();
  } else {
    auto& ty = env.state.mInstrState.base.type;
    if (ty.couldBe(BStr)) {
      // Note here that a string type stays a string (with a changed character,
      // and loss of staticness), unless it was the empty string, where it
      // becomes an array.  Do it conservatively for now:
      ty = union_of(
        loosen_staticness(loosen_values(std::move(ty))),
        some_aempty()
      );
    }
    if (!ty.subtypeOf(BStr)) promoteBaseElemD(env);
  }

  /*
   * In some unusual cases with illegal keys, SetM pushes null
   * instead of the right hand side.
   *
   * There are also some special cases for SetM for different base types:
   * 1. If the base is a string, SetM pushes a new string with the
   * value of the first character of the right hand side converted
   * to a string (or something like that).
   * 2. If the base is a primitive type, SetM pushes null.
   * 3. If the base is an object, and it does not implement ArrayAccess,
   * it is still ok to push the right hand side, because it is a
   * fatal.
   *
   * We push the right hand side on the stack only if the base is an
   * array, object or emptyish.
   */
  if (array_do_set(env, key, t1)) {
    if (env.state.unreachable) return finish(TBottom);
    auto const maybeWeird =
      env.state.mInstrState.base.type.subtypeOf(BArr) && keyCouldBeWeird(key);
    return finish(maybeWeird ? union_of(t1, TInitNull) : t1);
  }

  // ArrayAccess on $this will always push the rhs, even if things
  // were weird.
  if (mustBeThisObj(env, env.state.mInstrState.base)) return finish(t1);

  auto const isWeird =
    keyCouldBeWeird(key) ||
    (!mustBeEmptyish(env.state.mInstrState.base.type) &&
     !env.state.mInstrState.base.type.subtypeOf(BObj));
  finish(isWeird ? TInitCell : t1);
}

void miFinalSetOpElem(ISS& env, int32_t nDiscard,
                      SetOpOp subop, const Type& key,
                      LocalId keyLoc) {
  auto const rhsTy = popC(env);
  handleInThisElemD(env);
  handleInPublicStaticElemD(env);
  promoteBaseElemD(env);
  auto const lhsTy = [&] {
    if (auto ty = array_do_elem(env, false, key)) {
      if (env.state.unreachable) return TBottom;
      assertx(!ty->subtypeOf(BBottom));
      return std::move(*ty);
    }
    return TInitCell;
  }();
  auto const resultTy = typeSetOp(subop, lhsTy, rhsTy);
  pessimisticFinalElemD(env, key, resultTy);
  endBase(env, true, keyLoc);
  discard(env, nDiscard);
  push(env, resultTy);
}

void miFinalIncDecElem(ISS& env, int32_t nDiscard,
                       IncDecOp subop, const Type& key,
                       LocalId keyLoc) {
  handleInThisElemD(env);
  handleInPublicStaticElemD(env);
  promoteBaseElemD(env);
  auto const postTy = [&] {
    if (auto ty = array_do_elem(env, false, key)) {
      if (env.state.unreachable) return TBottom;
      assertx(!ty->subtypeOf(BBottom));
      return std::move(*ty);
    }
    return TInitCell;
  }();
  auto const preTy = typeIncDec(subop, postTy);
  pessimisticFinalElemD(env, key, preTy);
  endBase(env, true, keyLoc);
  discard(env, nDiscard);
  push(env, isPre(subop) ? preTy : postTy);
}

void miFinalUnsetElem(ISS& env, int32_t nDiscard, const Type&) {
  handleInPublicStaticElemU(env);
  promoteBaseElemU(env);
  // We don't handle inner-array types with unset yet.
  always_assert(env.state.mInstrState.arrayChain.empty());
  auto const& ty = env.state.mInstrState.base.type;
  always_assert(!ty.strictSubtypeOf(TArr) && !ty.strictSubtypeOf(TVec) &&
                !ty.strictSubtypeOf(TDict) && !ty.strictSubtypeOf(TKeyset));
  endBase(env);
  discard(env, nDiscard);
}

//////////////////////////////////////////////////////////////////////
// Final new elem ops

// This is a helper for final defining Elem operations that need to handle
// array chains and frame effects, but don't yet do anything better than
// supplying a single type.
void pessimisticFinalNewElem(ISS& env, const Type& type) {
  array_do_newelem(env, type);
}

void miFinalVGetNewElem(ISS& env, int32_t nDiscard) {
  handleInThisNewElem(env);
  handleInPublicStaticNewElem(env);
  promoteBaseNewElem(env);
  pessimisticFinalNewElem(env, TInitGen);

  auto const finish = [&](Type ty) {
    endBase(env);
    discard(env, nDiscard);
    push(env, std::move(ty));
  };

  auto const& baseTy = env.state.mInstrState.base.type;
  if (baseTy.subtypeOf(BVec) ||
      baseTy.subtypeOf(BDict) ||
      baseTy.subtypeOf(BKeyset)) {
    unreachable(env);
    return finish(TBottom);
  }
  finish(TRef);
}

void miFinalSetNewElem(ISS& env, int32_t nDiscard) {
  auto const t1 = popC(env);

  handleInThisNewElem(env);
  handleInPublicStaticNewElem(env);
  promoteBaseNewElem(env);

  auto const finish = [&](Type ty) {
    endBase(env);
    discard(env, nDiscard);
    push(env, std::move(ty));
  };

  if (array_do_newelem(env, t1)) {
    return finish(t1);
  }

  // ArrayAccess on $this will always push the rhs.
  if (mustBeThisObj(env, env.state.mInstrState.base)) return finish(t1);

  // TODO(#3343813): we should push the type of the rhs when we can;
  // SetM for a new elem still has some weird cases where it pushes
  // null instead to handle.  (E.g. if the base is a number.)
  finish(TInitCell);
}

void miFinalSetOpNewElem(ISS& env, int32_t nDiscard) {
  popC(env);
  handleInThisNewElem(env);
  handleInPublicStaticNewElem(env);
  promoteBaseNewElem(env);
  pessimisticFinalNewElem(env, TInitCell);
  endBase(env);
  discard(env, nDiscard);
  push(env, TInitCell);
}

void miFinalIncDecNewElem(ISS& env, int32_t nDiscard) {
  handleInThisNewElem(env);
  handleInPublicStaticNewElem(env);
  promoteBaseNewElem(env);
  pessimisticFinalNewElem(env, TInitCell);
  endBase(env);
  discard(env, nDiscard);
  push(env, TInitCell);
}

}

namespace interp_step {

//////////////////////////////////////////////////////////////////////
// Base operations

void in(ISS& env, const bc::BaseGC& op) {
  topC(env, op.arg1);
  startBase(env, Base{TInitCell, BaseLoc::Global});
}

void in(ISS& env, const bc::BaseGL& op) {
  locAsCell(env, op.loc1);
  startBase(env, Base{TInitCell, BaseLoc::Global});
}

void in(ISS& env, const bc::BaseSC& op) {
  auto prop = topC(env, op.arg1);
  auto cls = takeClsRefSlot(env, op.slot);
  startBase(env, miBaseSProp(env, std::move(cls), prop));
}

void in(ISS& env, const bc::BaseL& op) {
  startBase(env, miBaseLocal(env, op.loc1, op.subop2));
}

void in(ISS& env, const bc::BaseC& op) {
  assert(op.arg1 < env.state.stack.size());
  startBase(
    env,
    Base {
      topC(env, op.arg1),
      BaseLoc::Stack,
      TBottom,
      SString{},
      NoLocalId,
      (uint32_t)env.state.stack.size() - op.arg1 - 1
    }
  );
  nothrow(env);
}

void in(ISS& env, const bc::BaseH&) {
  auto const ty = thisType(env);
  startBase(env, Base{ty ? *ty : TObj, BaseLoc::This});
  nothrow(env);
}

//////////////////////////////////////////////////////////////////////
// Intermediate operations

void in(ISS& env, const bc::Dim& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;
  auto const keyLoc = key_local(env, op);
  if (mcodeIsProp(op.mkey.mcode)) {
    miProp(env, op.mkey.mcode == MQT, op.subop1, *key);
  } else if (mcodeIsElem(op.mkey.mcode)) {
    miElem(env, op.subop1, *key, keyLoc);
  } else {
    miNewElem(env);
  }
}

//////////////////////////////////////////////////////////////////////
// Final operations

void in(ISS& env, const bc::QueryM& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;
  auto const nDiscard = op.arg1;

  if (mcodeIsProp(op.mkey.mcode)) {
    // We don't currently do anything different for nullsafe query ops.
    switch (op.subop2) {
      case QueryMOp::CGet:
      case QueryMOp::CGetQuiet:
        miFinalCGetProp(env, nDiscard, *key);
        break;
      case QueryMOp::Isset:
        miFinalIssetProp(env, nDiscard, *key);
        break;
      case QueryMOp::Empty:
        discard(env, nDiscard);
        push(env, TBool);
        break;
      case QueryMOp::InOut:
        always_assert(false);
    }
  } else if (mcodeIsElem(op.mkey.mcode)) {
    switch (op.subop2) {
      case QueryMOp::InOut:
      case QueryMOp::CGet:
        miFinalCGetElem(env, nDiscard, *key, false,
                        [](Type t) { return t; });
        break;
      case QueryMOp::CGetQuiet:
        miFinalCGetElem(env, nDiscard, *key, true,
                        [](Type t) { return t; });
        break;
      case QueryMOp::Isset:
        miFinalCGetElem(env, nDiscard, *key, true,
                        [](Type t) {
                          return t.subtypeOf(BInitNull) ? TFalse :
                            !t.couldBe(BInitNull) ? TTrue : TBool;
                        });
        break;
      case QueryMOp::Empty:
        miFinalCGetElem(env, nDiscard, *key, true,
                        [](Type t) {
                          auto const e = emptiness(t);
                          return
                            e == Emptiness::Empty ? TTrue :
                            e == Emptiness::NonEmpty ? TFalse : TBool;
                        });
        break;
    }
  } else {
    // QueryMNewElem will always throw without doing any work.
    discard(env, op.arg1);
    push(env, TInitCell);
  }

  endBase(env, false);
}

void in(ISS& env, const bc::VGetM& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;
  if (mcodeIsProp(op.mkey.mcode)) {
    miFinalVGetProp(env, op.arg1, *key, op.mkey.mcode == MQT);
  } else if (mcodeIsElem(op.mkey.mcode)) {
    miFinalVGetElem(env, op.arg1, *key);
  } else {
    miFinalVGetNewElem(env, op.arg1);
  }
}

void in(ISS& env, const bc::SetM& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;
  auto const keyLoc = key_local(env, op);
  if (mcodeIsProp(op.mkey.mcode)) {
    miFinalSetProp(env, op.arg1, *key);
  } else if (mcodeIsElem(op.mkey.mcode)) {
    miFinalSetElem(env, op.arg1, *key, keyLoc);
  } else {
    miFinalSetNewElem(env, op.arg1);
  }
}

void in(ISS& env, const bc::SetRangeM& op) {
  auto const count = popC(env);
  auto const value = popC(env);
  auto const offset = popC(env);

  auto& base = env.state.mInstrState.base.type;
  if (base.couldBe(BStr)) {
    base = loosen_staticness(loosen_values(std::move(base)));
  }

  endBase(env, true, NoLocalId);
  discard(env, op.arg1);
}

void in(ISS& env, const bc::IncDecM& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;
  auto const keyLoc = key_local(env, op);
  if (mcodeIsProp(op.mkey.mcode)) {
    miFinalIncDecProp(env, op.arg1, op.subop2, *key);
  } else if (mcodeIsElem(op.mkey.mcode)) {
    miFinalIncDecElem(env, op.arg1, op.subop2, *key, keyLoc);
  } else {
    miFinalIncDecNewElem(env, op.arg1);
  }
}

void in(ISS& env, const bc::SetOpM& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;
  auto const keyLoc = key_local(env, op);
  if (mcodeIsProp(op.mkey.mcode)) {
    miFinalSetOpProp(env, op.arg1, op.subop2, *key);
  } else if (mcodeIsElem(op.mkey.mcode)) {
    miFinalSetOpElem(env, op.arg1, op.subop2, *key, keyLoc);
  } else {
    miFinalSetOpNewElem(env, op.arg1);
  }
}

void in(ISS& env, const bc::UnsetM& op) {
  auto const key = key_type_or_fixup(env, op);
  if (!key) return;
  if (mcodeIsProp(op.mkey.mcode)) {
    miFinalUnsetProp(env, op.arg1, *key);
  } else {
    assert(mcodeIsElem(op.mkey.mcode));
    miFinalUnsetElem(env, op.arg1, *key);
  }
}

}

//////////////////////////////////////////////////////////////////////

}}
