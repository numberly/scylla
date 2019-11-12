/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright (C) 2015 ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "cql3/restrictions/restriction.hh"
#include "cql3/statements/raw/cf_statement.hh"
#include "cql3/statements/bound.hh"
#include "cql3/column_identifier.hh"
#include "cql3/update_parameters.hh"
#include "cql3/column_condition.hh"
#include "cql3/cql_statement.hh"
#include "cql3/attributes.hh"
#include "cql3/operation.hh"
#include "cql3/relation.hh"
#include "cql3/restrictions/statement_restrictions.hh"
#include "cql3/single_column_relation.hh"
#include "cql3/statements/statement_type.hh"

#include <seastar/core/shared_ptr.hh>
#include <seastar/core/future-util.hh>

#include "unimplemented.hh"
#include "validation.hh"
#include "service/storage_proxy.hh"

#include <memory>
#include <optional>

namespace cql3 {

namespace statements {


namespace raw { class modification_statement; }

/*
 * Abstract parent class of individual modifications, i.e. INSERT, UPDATE and DELETE.
 */
class modification_statement : public cql_statement_opt_metadata {
public:
    const statement_type type;

private:
    const uint32_t _bound_terms;
    // If we have operation on list entries, such as adding or
    // removing an entry, the modification statement must prefetch
    // the old values of the list to create an idempotent mutation.
    // If the statement has conditions, conditional columns must
    // also be prefetched, to evaluate conditions. If the
    // statement has IF EXISTS/IF NOT EXISTS, we prefetch all
    // columns, to match Cassandra behaviour.
    // This bitset contains a mask of ordinal_id identifiers
    // of the required columns.
    column_mask _columns_to_read;
    // A CAS statement returns a result set with the columns
    // used in condition expression. This is a mask of ordinal_id
    // identifiers of the required columns. Contains all columns
    // of a schema if we have IF EXISTS/IF NOT EXISTS. Does *not*
    // contain LIST columns prefetched to apply updates, unless
    // these columns are also used in conditions.
    column_mask _columns_of_cas_result_set;
public:
    const schema_ptr s;
    const std::unique_ptr<attributes> attrs;

protected:
    std::vector<::shared_ptr<operation>> _column_operations;
private:
    // Separating normal and static conditions makes things somewhat easier
    std::vector<::shared_ptr<column_condition>> _column_conditions;
    std::vector<::shared_ptr<column_condition>> _static_conditions;

    // True if this statement has _if_exists or _if_not_exists or other
    // conditions that apply to static/regular columns, respectively.
    // Pre-computed during statement prepare.
    bool _has_static_column_conditions = false;
    bool _has_regular_column_conditions = false;
    // True if any of update operations requires a prefetch.
    // Pre-computed during statement prepare.
    bool _requires_read = false;
    bool _if_not_exists = false;
    bool _if_exists = false;

    bool _sets_static_columns = false;
    bool _sets_regular_columns = false;
    bool _sets_a_collection = false;
    std::optional<bool> _is_raw_counter_shard_write;

    const std::function<const column_definition&(::shared_ptr<column_condition>)> get_column_for_condition =
        [](::shared_ptr<column_condition> cond) -> const column_definition& {
            return cond->column;
        };

    cql_stats& _stats;
protected:
    ::shared_ptr<restrictions::statement_restrictions> _restrictions;
public:
    typedef std::optional<std::unordered_map<sstring, bytes_opt>> json_cache_opt;

    modification_statement(statement_type type_, uint32_t bound_terms, schema_ptr schema_, std::unique_ptr<attributes> attrs_, cql_stats& stats);

    virtual bool uses_function(const sstring& ks_name, const sstring& function_name) const override;

    virtual bool require_full_clustering_key() const = 0;

    virtual bool allow_clustering_key_slices() const = 0;

    virtual void add_update_for_key(mutation& m, const query::clustering_range& range, const update_parameters& params, const json_cache_opt& json_cache) = 0;

    virtual uint32_t get_bound_terms() override;

    virtual const sstring& keyspace() const;

    virtual const sstring& column_family() const;

    virtual bool is_counter() const;

    virtual bool is_view() const;

    int64_t get_timestamp(int64_t now, const query_options& options) const;

    bool is_timestamp_set() const;

    gc_clock::duration get_time_to_live(const query_options& options) const;

    virtual future<> check_access(const service::client_state& state) override;

    // Validate before execute, using client state and current schema
    void validate(service::storage_proxy&, const service::client_state& state) override;

    virtual bool depends_on_keyspace(const sstring& ks_name) const override;

    virtual bool depends_on_column_family(const sstring& cf_name) const override;

    void add_operation(::shared_ptr<operation> op);

    const ::shared_ptr<restrictions::statement_restrictions>& restrictions() const {
        return _restrictions;
    }
public:
    void add_condition(::shared_ptr<column_condition> cond);

    void set_if_not_exist_condition();

    bool has_if_not_exist_condition() const;

    void set_if_exist_condition();

    bool has_if_exist_condition() const;

    bool is_raw_counter_shard_write() const {
        return _is_raw_counter_shard_write.value_or(false);
    }

    void process_where_clause(database& db, std::vector<relation_ptr> where_clause, ::shared_ptr<variable_specifications> names);

    // CAS statement returns a result set. Prepare result set metadata
    // so that get_result_metadata() returns a meaningful value.
    void build_cas_result_set_metadata();
    // Build a result set with prefetched rows, but return only
    // the columns required by CAS. Static since reused by BATCH
    // CAS.
    static seastar::shared_ptr<cql_transport::messages::result_message>
    build_cas_result_set(seastar::shared_ptr<cql3::metadata> metadata,
            const column_mask& mask, bool is_applied,
            const update_parameters::prefetch_data& rows);
public:
    virtual dht::partition_range_vector build_partition_keys(const query_options& options, const json_cache_opt& json_cache);
    virtual query::clustering_row_ranges create_clustering_ranges(const query_options& options, const json_cache_opt& json_cache);

private:
    // Return true if this statement doesn't update or read any regular rows, only static rows.
    // Note, it isn't enought to just check !_sets_regular_columns && _column_conditions.empty(),
    // because a DELETE statement that deletes whole rows (DELETE FROM ...) technically doesn't
    // have any column operations and hence doesn't have _sets_regular_columns set. It doesn't
    // have _sets_static_columns set either so checking the latter flag too here guarantees that
    // this function works as expected in all cases.
    bool applies_only_to_static_columns() const {
        return _sets_static_columns && !_sets_regular_columns && _column_conditions.empty();
    }
public:
    // True if any of update operations of this statement requires
    // a prefetch of the old cell.
    bool requires_read() const { return _requires_read; }

    // Columns used in this statement conditions or operations.
    const column_mask& columns_to_read() const { return _columns_to_read; }

    // Columns of the statement result set (only CAS statement
    // returns a result set).
    const column_mask& columns_of_cas_result_set() { return _columns_of_cas_result_set; }

    // Build a read_command instance to fetch the previous mutation from storage. The mutation is
    // fetched if we need to check LWT conditions or apply updates to non-frozen list elements.
    lw_shared_ptr<query::read_command> read_command(query::clustering_row_ranges ranges, db::consistency_level cl) const;
    // Create a mutation object for the update operation represented by this modification statement.
    // A single mutation object for lightweight transactions, which can only span one partition, or a vector
    // of mutations, one per partition key, for statements which affect multiple partition keys,
    // e.g. DELETE FROM table WHERE pk  IN (1, 2, 3).
    std::vector<mutation> apply_updates(
            const std::vector<dht::partition_range>& keys,
            const std::vector<query::clustering_range>& ranges,
            const update_parameters& params,
            const json_cache_opt& json_cache);

    /**
     * Checks whether the conditions represented by this statement apply provided the current state of the row on
     * which those conditions are.
     *
     * @param row the row with current data corresponding to these conditions. Can be null if there
     * is no matching row.
     * @return whether the conditions represented by this statement apply or not.
     */
    bool applies_to(const update_parameters::prefetch_data::row* row, const query_options& options) const;

private:
    future<::shared_ptr<cql_transport::messages::result_message>>
    do_execute(service::storage_proxy& proxy, service::query_state& qs, const query_options& options);
    friend class modification_statement_executor;
public:
    // True if the statement has IF conditions. Pre-computed during prepare.
    bool has_conditions() const { return _has_regular_column_conditions || _has_static_column_conditions; }
    // True if the statement has IF conditions that apply to static columns.
    bool has_static_column_conditions() const { return _has_static_column_conditions; }
    // True if this statement needs to read only static column values to check if it can be applied.
    bool has_only_static_column_conditions() const { return !_has_regular_column_conditions && _has_static_column_conditions; }

    virtual future<::shared_ptr<cql_transport::messages::result_message>>
    execute(service::storage_proxy& proxy, service::query_state& qs, const query_options& options) override;

private:
    future<>
    execute_without_condition(service::storage_proxy& proxy, service::query_state& qs, const query_options& options);

    future<::shared_ptr<cql_transport::messages::result_message>>
    execute_with_condition(service::storage_proxy& proxy, service::query_state& qs, const query_options& options);

public:
    /**
     * Convert statement into a list of mutations to apply on the server
     *
     * @param options value for prepared statement markers
     * @param local if true, any requests (for collections) performed by getMutation should be done locally only.
     * @param now the current timestamp in microseconds to use if no timestamp is user provided.
     *
     * @return vector of the mutations
     * @throws invalid_request_exception on invalid requests
     */
    future<std::vector<mutation>> get_mutations(service::storage_proxy& proxy, const query_options& options, db::timeout_clock::time_point timeout, bool local, int64_t now, service::query_state& qs);

    virtual json_cache_opt maybe_prepare_json_cache(const query_options& options);
protected:
    /**
     * If there are conditions on the statement, this is called after the where clause and conditions have been
     * processed to check that they are compatible.
     * @throws InvalidRequestException
     */
    virtual void validate_where_clause_for_conditions() const;
    friend class raw::modification_statement;
};

}

}
