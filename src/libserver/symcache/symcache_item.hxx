/*-
 * Copyright 2022 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef RSPAMD_SYMCACHE_ITEM_HXX
#define RSPAMD_SYMCACHE_ITEM_HXX

#pragma once

#include <utility>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <variant>

#include "rspamd_symcache.h"
#include "symcache_id_list.hxx"
#include "contrib/expected/expected.hpp"
#include "contrib/libev/ev.h"
#include "lua/lua_common.h"

namespace rspamd::symcache {

class symcache;
struct cache_item;
using cache_item_ptr = std::shared_ptr<cache_item>;

enum class symcache_item_type {
	CONNFILTER, /* Executed on connection stage */
	PREFILTER, /* Executed before all filters */
	FILTER, /* Normal symbol with a callback */
	POSTFILTER, /* Executed after all filters */
	IDEMPOTENT, /* Executed after postfilters, cannot change results */
	CLASSIFIER, /* A virtual classifier symbol */
	COMPOSITE, /* A virtual composite symbol */
	VIRTUAL, /* A virtual symbol... */
};

/*
 * Compare item types: earlier stages symbols are > than later stages symbols
 * Order for virtual stuff is not defined.
 */
bool operator<(symcache_item_type lhs, symcache_item_type rhs);

/**
 * This is a public helper to convert a legacy C type to a more static type
 * @param type input type as a C enum
 * @return pair of type safe symcache_item_type + the remaining flags or an error
 */
auto item_type_from_c(enum rspamd_symbol_type type) -> tl::expected<std::pair<symcache_item_type, int>, std::string>;

struct item_condition {
private:
	lua_State *L;
	int cb;
public:
	item_condition(lua_State *_L, int _cb) : L(_L), cb(_cb)
	{
	}

	virtual ~item_condition()
	{
		if (cb != -1 && L != nullptr) {
			luaL_unref(L, LUA_REGISTRYINDEX, cb);
		}
	}
};

class normal_item {
private:
	symbol_func_t func;
	void *user_data;
	std::vector<item_condition> conditions;
public:
	explicit normal_item(symbol_func_t _func, void *_user_data) : func(_func), user_data(_user_data)
	{
	}

	auto add_condition(lua_State *L, int cbref) -> void
	{
		conditions.emplace_back(L, cbref);
	}

	auto call() -> void
	{
		// TODO
	}
};

class virtual_item {
private:
	int parent_id;
	cache_item_ptr parent;
public:
	explicit virtual_item(int _parent_id) : parent_id(_parent_id)
	{
	}

	auto get_parent(const symcache &cache) const -> const cache_item *;

	auto resolve_parent(const symcache &cache) -> bool;
};

struct cache_dependency {
	cache_item_ptr item; /* Real dependency */
	std::string sym; /* Symbolic dep name */
	int id; /* Real from */
	int vid; /* Virtual from */
public:
	/* Default piecewise constructor */
	cache_dependency(cache_item_ptr _item, std::string _sym, int _id, int _vid) :
			item(std::move(_item)), sym(std::move(_sym)), id(_id), vid(_vid)
	{
	}
};

struct cache_item : std::enable_shared_from_this<cache_item> {
	/* This block is likely shared */
	struct rspamd_symcache_item_stat *st = nullptr;
	struct rspamd_counter_data *cd = nullptr;

	/* Unique id - counter */
	int id;
	std::uint64_t last_count = 0;
	std::string symbol;
	symcache_item_type type;
	int flags;

	/* Condition of execution */
	bool enabled = true;

	/* Priority */
	int priority = 0;
	/* Topological order */
	unsigned int order = 0;
	int frequency_peaks = 0;

	/* Specific data for virtual and callback symbols */
	std::variant<normal_item, virtual_item> specific;

	/* Settings ids */
	id_list allowed_ids{};
	/* Allows execution but not symbols insertion */
	id_list exec_only_ids{};
	id_list forbidden_ids{};

	/* Dependencies */
	std::vector<cache_dependency> deps;
	/* Reverse dependencies */
	std::vector<cache_dependency> rdeps;

public:
	/**
	 * Create a normal item with a callback
	 * @param name
	 * @param priority
	 * @param func
	 * @param user_data
	 * @param type
	 * @param flags
	 * @return
	 */
	[[nodiscard]] static auto create_with_function(rspamd_mempool_t *pool,
												   int id,
												   std::string &&name,
												   int priority,
												   symbol_func_t func,
												   void *user_data,
												   symcache_item_type type,
												   int flags) -> cache_item_ptr
	{
		return std::shared_ptr<cache_item>(new cache_item(pool,
				id, std::move(name), priority,
				func, user_data,
				type, flags));
	}

	/**
	 * Create a virtual item
	 * @param name
	 * @param priority
	 * @param parent
	 * @param type
	 * @param flags
	 * @return
	 */
	[[nodiscard]] static auto create_with_virtual(rspamd_mempool_t *pool,
												  int id,
												  std::string &&name,
												  int parent,
												  symcache_item_type type,
												  int flags) -> cache_item_ptr
	{
		return std::shared_ptr<cache_item>(new cache_item(pool, id, std::move(name),
				parent, type, flags));
	}

	/**
	 * Share ownership on the item
	 * @return
	 */
	auto getptr() -> cache_item_ptr
	{
		return shared_from_this();
	}

	/**
	 * Process and resolve dependencies for the item
	 * @param cache
	 */
	auto process_deps(const symcache &cache) -> void;

	auto is_virtual() const -> bool
	{
		return std::holds_alternative<virtual_item>(specific);
	}

	auto is_filter() const -> bool
	{
		return std::holds_alternative<normal_item>(specific) &&
			   (type == symcache_item_type::FILTER);
	}

	/**
	 * Returns true if a symbol should have some score defined
	 * @return
	 */
	auto is_scoreable() const -> bool
	{
		return (type == symcache_item_type::FILTER) ||
			   is_virtual() ||
			   (type == symcache_item_type::COMPOSITE) ||
			   (type == symcache_item_type::CLASSIFIER);
	}

	auto is_ghost() const -> bool
	{
		return flags & SYMBOL_TYPE_GHOST;
	}

	auto get_parent(const symcache &cache) const -> const cache_item *;

	auto resolve_parent(const symcache &cache) -> bool;

	auto get_type() const -> auto
	{
		return type;
	}

	auto get_name() const -> const std::string &
	{
		return symbol;
	}

	auto get_flags() const -> auto {
		return flags;
	};

	auto add_condition(lua_State *L, int cbref) -> bool
	{
		if (!is_virtual()) {
			auto &normal = std::get<normal_item>(specific);
			normal.add_condition(L, cbref);

			return true;
		}

		return false;
	}

	auto update_counters_check_peak(lua_State *L,
									struct ev_loop *ev_loop,
									double cur_time,
									double last_resort) -> bool;

	auto inc_frequency() -> void {
		g_atomic_int_inc(&st->hits);
	}

private:
	/**
	 * Constructor for a normal symbols with callback
	 * @param name
	 * @param _priority
	 * @param func
	 * @param user_data
	 * @param _type
	 * @param _flags
	 */
	cache_item(rspamd_mempool_t *pool,
			   int _id,
			   std::string &&name,
			   int _priority,
			   symbol_func_t func,
			   void *user_data,
			   symcache_item_type _type,
			   int _flags) : id(_id),
							 symbol(std::move(name)),
							 type(_type),
							 flags(_flags),
							 priority(_priority),
							 specific(normal_item{func, user_data})
	{
		forbidden_ids.reset();
		allowed_ids.reset();
		exec_only_ids.reset();
		st = rspamd_mempool_alloc0_shared_type(pool, std::remove_pointer_t<decltype(st)>);
		cd = rspamd_mempool_alloc0_shared_type(pool, std::remove_pointer_t<decltype(cd)>);
	}

	/**
	 * Constructor for a virtual symbol
	 * @param name
	 * @param _priority
	 * @param parent
	 * @param _type
	 * @param _flags
	 */
	cache_item(rspamd_mempool_t *pool,
			   int _id,
			   std::string &&name,
			   int parent,
			   symcache_item_type _type,
			   int _flags) : id(_id),
							 symbol(std::move(name)),
							 type(_type),
							 flags(_flags),
							 specific(virtual_item{parent})
	{
		forbidden_ids.reset();
		allowed_ids.reset();
		exec_only_ids.reset();
		st = rspamd_mempool_alloc0_shared_type(pool, std::remove_pointer_t<decltype(st)>);
		cd = rspamd_mempool_alloc0_shared_type(pool, std::remove_pointer_t<decltype(cd)>);
	}
};

}

#endif //RSPAMD_SYMCACHE_ITEM_HXX