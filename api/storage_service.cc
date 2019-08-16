/*
 * Copyright (C) 2015 ScyllaDB
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

#include "storage_service.hh"
#include "api/api-doc/storage_service.json.hh"
#include "db/config.hh"
#include <optional>
#include <time.h>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include "service/storage_service.hh"
#include "db/commitlog/commitlog.hh"
#include "gms/gossiper.hh"
#include "db/system_keyspace.hh"
#include "seastar/http/exception.hh"
#include "repair/repair.hh"
#include "locator/snitch_base.hh"
#include "column_family.hh"
#include "log.hh"
#include "release.hh"
#include "sstables/compaction_manager.hh"
#include "sstables/sstables.hh"
#include "database.hh"
#include "db/extensions.hh"

sstables::sstable::version_types get_highest_supported_format();

namespace api {

namespace ss = httpd::storage_service_json;
using namespace json;

static sstring validate_keyspace(http_context& ctx, const parameters& param) {
    if (ctx.db.local().has_keyspace(param["keyspace"])) {
        return param["keyspace"];
    }
    throw bad_param_exception("Keyspace " + param["keyspace"] + " Does not exist");
}

static std::vector<ss::token_range> describe_ring(const sstring& keyspace) {
    std::vector<ss::token_range> res;
    for (auto d : service::get_local_storage_service().describe_ring(keyspace)) {
        ss::token_range r;
        r.start_token = d._start_token;
        r.end_token = d._end_token;
        r.endpoints = d._endpoints;
        r.rpc_endpoints = d._rpc_endpoints;
        for (auto det : d._endpoint_details) {
            ss::endpoint_detail ed;
            ed.host = det._host;
            ed.datacenter = det._datacenter;
            if (det._rack != "") {
                ed.rack = det._rack;
            }
            r.endpoint_details.push(ed);
        }
        res.push_back(r);
    }
    return res;
}

void set_storage_service(http_context& ctx, routes& r) {
    using ks_cf_func = std::function<future<json::json_return_type>(std::unique_ptr<request>, sstring, std::vector<sstring>)>;

    auto wrap_ks_cf = [&ctx](ks_cf_func f) {
        return [&ctx, f = std::move(f)](std::unique_ptr<request> req) {
            auto keyspace = validate_keyspace(ctx, req->param);
            auto column_families = split_cf(req->get_query_param("cf"));
            if (column_families.empty()) {
                column_families = map_keys(ctx.db.local().find_keyspace(keyspace).metadata().get()->cf_meta_data());
            }
            return f(std::move(req), std::move(keyspace), std::move(column_families));
        };
    };

    ss::local_hostid.set(r, [](std::unique_ptr<request> req) {
        return db::system_keyspace::get_local_host_id().then([](const utils::UUID& id) {
            return make_ready_future<json::json_return_type>(id.to_sstring());
        });
    });

    ss::get_tokens.set(r, [] (std::unique_ptr<request> req) {
        return make_ready_future<json::json_return_type>(stream_range_as_array(service::get_local_storage_service().get_token_metadata().sorted_tokens(), [](const dht::token& i) {
           return boost::lexical_cast<std::string>(i);
        }));
    });

    ss::get_node_tokens.set(r, [] (std::unique_ptr<request> req) {
        gms::inet_address addr(req->param["endpoint"]);
        return make_ready_future<json::json_return_type>(stream_range_as_array(service::get_local_storage_service().get_token_metadata().get_tokens(addr), [](const dht::token& i) {
           return boost::lexical_cast<std::string>(i);
       }));
    });

    ss::get_commitlog.set(r, [&ctx](const_req req) {
        return ctx.db.local().commitlog()->active_config().commit_log_location;
    });

    ss::get_token_endpoint.set(r, [] (std::unique_ptr<request> req) {
        return make_ready_future<json::json_return_type>(stream_range_as_array(service::get_local_storage_service().get_token_to_endpoint_map(), [](const auto& i) {
            storage_service_json::mapper val;
            val.key = boost::lexical_cast<std::string>(i.first);
            val.value = boost::lexical_cast<std::string>(i.second);
            return val;
        }));
    });

    ss::get_leaving_nodes.set(r, [](const_req req) {
        return container_to_vec(service::get_local_storage_service().get_token_metadata().get_leaving_endpoints());
    });

    ss::get_moving_nodes.set(r, [](const_req req) {
        std::unordered_set<sstring> addr;
        return container_to_vec(addr);
    });

    ss::get_joining_nodes.set(r, [](const_req req) {
        auto points = service::get_local_storage_service().get_token_metadata().get_bootstrap_tokens();
        std::unordered_set<sstring> addr;
        for (auto i: points) {
            addr.insert(boost::lexical_cast<std::string>(i.second));
        }
        return container_to_vec(addr);
    });

    ss::get_release_version.set(r, [](const_req req) {
        return service::get_local_storage_service().get_release_version();
    });

    ss::get_scylla_release_version.set(r, [](const_req req) {
        return scylla_version();
    });
    ss::get_schema_version.set(r, [](const_req req) {
        return service::get_local_storage_service().get_schema_version();
    });

    ss::get_all_data_file_locations.set(r, [&ctx](const_req req) {
        return container_to_vec(ctx.db.local().get_config().data_file_directories());
    });

    ss::get_saved_caches_location.set(r, [&ctx](const_req req) {
        return ctx.db.local().get_config().saved_caches_directory();
    });

    ss::get_range_to_endpoint_map.set(r, [&ctx](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto keyspace = validate_keyspace(ctx, req->param);
        std::vector<ss::maplist_mapper> res;
        return make_ready_future<json::json_return_type>(res);
    });

    ss::get_pending_range_to_endpoint_map.set(r, [&ctx](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto keyspace = validate_keyspace(ctx, req->param);
        std::vector<ss::maplist_mapper> res;
        return make_ready_future<json::json_return_type>(res);
    });

    ss::describe_any_ring.set(r, [&ctx](const_req req) {
        return describe_ring("");
    });

    ss::describe_ring.set(r, [&ctx](const_req req) {
        auto keyspace = validate_keyspace(ctx, req.param);
        return describe_ring(keyspace);
    });

    ss::get_host_id_map.set(r, [](const_req req) {
        std::vector<ss::mapper> res;
        return map_to_key_value(service::get_local_storage_service().
                get_token_metadata().get_endpoint_to_host_id_map_for_reading(), res);
    });

    ss::get_load.set(r, [&ctx](std::unique_ptr<request> req) {
        return get_cf_stats(ctx, &column_family::stats::live_disk_space_used);
    });

    ss::get_load_map.set(r, [] (std::unique_ptr<request> req) {
        return service::get_local_storage_service().get_load_map().then([] (auto&& load_map) {
            std::vector<ss::map_string_double> res;
            for (auto i : load_map) {
                ss::map_string_double val;
                val.key = i.first;
                val.value = i.second;
                res.push_back(val);
            }
            return make_ready_future<json::json_return_type>(res);
        });
    });

    ss::get_current_generation_number.set(r, [](std::unique_ptr<request> req) {
        gms::inet_address ep(utils::fb_utilities::get_broadcast_address());
        return gms::get_local_gossiper().get_current_generation_number(ep).then([](int res) {
            return make_ready_future<json::json_return_type>(res);
        });
    });

    ss::get_natural_endpoints.set(r, [&ctx](const_req req) {
        auto keyspace = validate_keyspace(ctx, req.param);
        return container_to_vec(service::get_local_storage_service().get_natural_endpoints(keyspace, req.get_query_param("cf"),
                req.get_query_param("key")));
    });

    ss::get_snapshot_details.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().get_snapshot_details().then([] (auto result) {
            std::vector<ss::snapshots> res;
            for (auto& map: result) {
                ss::snapshots all_snapshots;
                all_snapshots.key = map.first;

                std::vector<ss::snapshot> snapshot;
                for (auto& cf: map.second) {
                    ss::snapshot s;
                    s.ks = cf.ks;
                    s.cf = cf.cf;
                    s.live = cf.live;
                    s.total = cf.total;
                    snapshot.push_back(std::move(s));
                }
                all_snapshots.value = std::move(snapshot);
                res.push_back(std::move(all_snapshots));
            }
            return make_ready_future<json::json_return_type>(std::move(res));
        });
    });

    ss::take_snapshot.set(r, [](std::unique_ptr<request> req) {
        auto tag = req->get_query_param("tag");
        auto column_family = req->get_query_param("cf");

        std::vector<sstring> keynames = split(req->get_query_param("kn"), ",");

        auto resp = make_ready_future<>();
        if (column_family.empty()) {
            resp = service::get_local_storage_service().take_snapshot(tag, keynames);
        } else {
            if (keynames.size() > 1) {
                throw httpd::bad_param_exception("Only one keyspace allowed when specifying a column family");
            }
            resp = service::get_local_storage_service().take_column_family_snapshot(keynames[0], column_family, tag);
        }
        return resp.then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::del_snapshot.set(r, [](std::unique_ptr<request> req) {
        auto tag = req->get_query_param("tag");

        std::vector<sstring> keynames = split(req->get_query_param("kn"), ",");
        return service::get_local_storage_service().clear_snapshot(tag, keynames).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::true_snapshots_size.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().true_snapshots_size().then([] (int64_t size) {
            return make_ready_future<json::json_return_type>(size);
        });
    });

    ss::force_keyspace_compaction.set(r, [&ctx](std::unique_ptr<request> req) {
        auto keyspace = validate_keyspace(ctx, req->param);
        auto column_families = split_cf(req->get_query_param("cf"));
        if (column_families.empty()) {
            column_families = map_keys(ctx.db.local().find_keyspace(keyspace).metadata().get()->cf_meta_data());
        }
        return ctx.db.invoke_on_all([keyspace, column_families] (database& db) {
            std::vector<column_family*> column_families_vec;
            for (auto cf : column_families) {
                column_families_vec.push_back(&db.find_column_family(keyspace, cf));
            }
            return parallel_for_each(column_families_vec, [] (column_family* cf) {
                    return cf->compact_all_sstables();
            });
        }).then([]{
                return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::force_keyspace_cleanup.set(r, [&ctx](std::unique_ptr<request> req) {
        auto keyspace = validate_keyspace(ctx, req->param);
        auto column_families = split_cf(req->get_query_param("cf"));
        if (column_families.empty()) {
            column_families = map_keys(ctx.db.local().find_keyspace(keyspace).metadata().get()->cf_meta_data());
        }
        return ctx.db.invoke_on_all([keyspace, column_families] (database& db) {
            std::vector<column_family*> column_families_vec;
            auto& cm = db.get_compaction_manager();
            for (auto cf : column_families) {
                column_families_vec.push_back(&db.find_column_family(keyspace, cf));
            }
            return parallel_for_each(column_families_vec, [&cm] (column_family* cf) {
                return cm.perform_cleanup(cf);
            });
        }).then([]{
            return make_ready_future<json::json_return_type>(0);
        });
    });

    ss::scrub.set(r, wrap_ks_cf([&ctx](std::unique_ptr<request> req, sstring keyspace, std::vector<sstring> column_families) {
        // TODO: respect this
        auto skip_corrupted = req->get_query_param("skip_corrupted");

        auto f = make_ready_future<>();
        if (!req_param<bool>(*req, "disable_snapshot", false)) {
            auto tag = format("pre-scrub-{:d}", db_clock::now().time_since_epoch().count());
            f = parallel_for_each(column_families, [keyspace, tag](sstring cf) {
                return service::get_local_storage_service().take_column_family_snapshot(keyspace, cf, tag);
            });
        }

        return f.then([&ctx, keyspace, column_families] {
            return ctx.db.invoke_on_all([=] (database& db) {
                return do_for_each(column_families, [=, &db](sstring cfname) {
                    auto& cm = db.get_compaction_manager();
                    auto& cf = db.find_column_family(keyspace, cfname);
                    return cm.perform_sstable_scrub(&cf);
                });
            });
        }).then([]{
            return make_ready_future<json::json_return_type>(0);
        });
    }));

    ss::upgrade_sstables.set(r, wrap_ks_cf([&ctx](std::unique_ptr<request> req, sstring keyspace, std::vector<sstring> column_families) {
        bool exclude_current_version = req_param<bool>(*req, "exclude_current_version", false);

        return ctx.db.invoke_on_all([=] (database& db) {
            return do_for_each(column_families, [=, &db](sstring cfname) {
                auto& cm = db.get_compaction_manager();
                auto& cf = db.find_column_family(keyspace, cfname);
                return cm.perform_sstable_upgrade(&cf, exclude_current_version);
            });
        }).then([]{
            return make_ready_future<json::json_return_type>(0);
        });
    }));

    ss::force_keyspace_flush.set(r, [&ctx](std::unique_ptr<request> req) {
        auto keyspace = validate_keyspace(ctx, req->param);
        auto column_families = split_cf(req->get_query_param("cf"));
        if (column_families.empty()) {
            column_families = map_keys(ctx.db.local().find_keyspace(keyspace).metadata().get()->cf_meta_data());
        }
        return ctx.db.invoke_on_all([keyspace, column_families] (database& db) {
            return parallel_for_each(column_families, [&db, keyspace](const sstring& cf) mutable {
                return db.find_column_family(keyspace, cf).flush();
            });
        }).then([]{
                return make_ready_future<json::json_return_type>(json_void());
        });
    });


    ss::repair_async.set(r, [&ctx](std::unique_ptr<request> req) {
        static std::vector<sstring> options = {"primaryRange", "parallelism", "incremental",
                "jobThreads", "ranges", "columnFamilies", "dataCenters", "hosts", "trace",
                "startToken", "endToken" };
        std::unordered_map<sstring, sstring> options_map;
        for (auto o : options) {
            auto s = req->get_query_param(o);
            if (s != "") {
                options_map[o] = s;
            }
        }

        // The repair process is asynchronous: repair_start only starts it and
        // returns immediately, not waiting for the repair to finish. The user
        // then has other mechanisms to track the ongoing repair's progress,
        // or stop it.
        return repair_start(ctx.db, validate_keyspace(ctx, req->param),
                options_map).then([] (int i) {
                    return make_ready_future<json::json_return_type>(i);
                });
    });

    ss::get_active_repair_async.set(r, [&ctx](std::unique_ptr<request> req) {
        return get_active_repairs(ctx.db).then([] (std::vector<int> res){
            return make_ready_future<json::json_return_type>(res);
        });
    });

    ss::repair_async_status.set(r, [&ctx](std::unique_ptr<request> req) {
        return repair_get_status(ctx.db, boost::lexical_cast<int>( req->get_query_param("id")))
                .then_wrapped([] (future<repair_status>&& fut) {
            ss::ns_repair_async_status::return_type_wrapper res;
            try {
                res = fut.get0();
            } catch(std::runtime_error& e) {
                throw httpd::bad_param_exception(e.what());
            }
            return make_ready_future<json::json_return_type>(json::json_return_type(res));
        });
    });

    ss::force_terminate_all_repair_sessions.set(r, [](std::unique_ptr<request> req) {
        return repair_abort_all(service::get_local_storage_service().db()).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::force_terminate_all_repair_sessions_new.set(r, [](std::unique_ptr<request> req) {
        return repair_abort_all(service::get_local_storage_service().db()).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::decommission.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().decommission().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::move.set(r, [] (std::unique_ptr<request> req) {
        auto new_token = req->get_query_param("new_token");
        return service::get_local_storage_service().move(new_token).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::remove_node.set(r, [](std::unique_ptr<request> req) {
        auto host_id = req->get_query_param("host_id");
        return service::get_local_storage_service().removenode(host_id).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::get_removal_status.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().get_removal_status().then([] (auto status) {
            return make_ready_future<json::json_return_type>(status);
        });
    });

    ss::force_remove_completion.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().force_remove_completion().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::set_logging_level.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto class_qualifier = req->get_query_param("class_qualifier");
        auto level = req->get_query_param("level");
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::get_logging_levels.set(r, [](std::unique_ptr<request> req) {
        std::vector<ss::mapper> res;
        for (auto i : logging::logger_registry().get_all_logger_names()) {
            ss::mapper log;
            log.key = i;
            log.value = logging::level_name(logging::logger_registry().get_logger_level(i));
            res.push_back(log);
        }
        return make_ready_future<json::json_return_type>(res);
    });

    ss::get_operation_mode.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().get_operation_mode().then([] (auto mode) {
            return make_ready_future<json::json_return_type>(mode);
        });
    });

    ss::is_starting.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().is_starting().then([] (auto starting) {
            return make_ready_future<json::json_return_type>(starting);
        });
    });

    ss::get_drain_progress.set(r, [](std::unique_ptr<request> req) {
        return service::get_storage_service().map_reduce(adder<service::storage_service::drain_progress>(), [] (auto& ss) {
            return ss.get_drain_progress();
        }).then([] (auto&& progress) {
            auto progress_str = format("Drained {}/{} ColumnFamilies", progress.remaining_cfs, progress.total_cfs);
            return make_ready_future<json::json_return_type>(std::move(progress_str));
        });
    });

    ss::drain.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().drain().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });
    ss::truncate.set(r, [&ctx](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto keyspace = validate_keyspace(ctx, req->param);
        auto column_family = req->get_query_param("cf");
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::get_keyspaces.set(r, [&ctx](const_req req) {
        auto type = req.get_query_param("type");
        if (type == "user") {
            return ctx.db.local().get_non_system_keyspaces();
        } else if (type == "non_local_strategy") {
            return map_keys(ctx.db.local().get_keyspaces() | boost::adaptors::filtered([](const auto& p) {
                return p.second.get_replication_strategy().get_type() != locator::replication_strategy_type::local;
            }));
        }
        return map_keys(ctx.db.local().get_keyspaces());
    });

    ss::update_snitch.set(r, [](std::unique_ptr<request> req) {
        auto ep_snitch_class_name = req->get_query_param("ep_snitch_class_name");
        return locator::i_endpoint_snitch::reset_snitch(ep_snitch_class_name).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::stop_gossiping.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().stop_gossiping().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::start_gossiping.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().start_gossiping().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::is_gossip_running.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().is_gossip_running().then([] (bool running){
            return make_ready_future<json::json_return_type>(running);
        });
    });


    ss::stop_daemon.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::is_initialized.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().is_initialized().then([] (bool initialized) {
            return make_ready_future<json::json_return_type>(initialized);
        });
    });

    ss::stop_rpc_server.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().stop_rpc_server().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::start_rpc_server.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().start_rpc_server().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::is_rpc_server_running.set(r, [] (std::unique_ptr<request> req) {
        return service::get_local_storage_service().is_rpc_server_running().then([] (bool running) {
            return make_ready_future<json::json_return_type>(running);
        });
    });

    ss::start_native_transport.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().start_native_transport().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::stop_native_transport.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().stop_native_transport().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::is_native_transport_running.set(r, [] (std::unique_ptr<request> req) {
        return service::get_local_storage_service().is_native_transport_running().then([] (bool running) {
            return make_ready_future<json::json_return_type>(running);
        });
    });

    ss::join_ring.set(r, [](std::unique_ptr<request> req) {
        return service::get_local_storage_service().join_ring().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::is_joined.set(r, [] (std::unique_ptr<request> req) {
        return make_ready_future<json::json_return_type>(service::get_local_storage_service().is_joined());
    });

    ss::set_stream_throughput_mb_per_sec.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto value = req->get_query_param("value");
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::get_stream_throughput_mb_per_sec.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    ss::get_compaction_throughput_mb_per_sec.set(r, [&ctx](std::unique_ptr<request> req) {
        int value = ctx.db.local().get_config().compaction_throughput_mb_per_sec();
        return make_ready_future<json::json_return_type>(value);
    });

    ss::set_compaction_throughput_mb_per_sec.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto value = req->get_query_param("value");
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::is_incremental_backups_enabled.set(r, [](std::unique_ptr<request> req) {
        // If this is issued in parallel with an ongoing change, we may see values not agreeing.
        // Reissuing is asking for trouble, so we will just return true upon seeing any true value.
        return service::get_local_storage_service().db().map_reduce(adder<bool>(), [] (database& db) {
            for (auto& pair: db.get_keyspaces()) {
                auto& ks = pair.second;
                if (ks.incremental_backups_enabled()) {
                    return true;
                }
            }
            return false;
        }).then([] (bool val) {
            return make_ready_future<json::json_return_type>(val);
        });
    });

    ss::set_incremental_backups_enabled.set(r, [](std::unique_ptr<request> req) {
        auto val_str = req->get_query_param("value");
        bool value = (val_str == "True") || (val_str == "true") || (val_str == "1");
        return service::get_local_storage_service().db().invoke_on_all([value] (database& db) {
            db.set_enable_incremental_backups(value);

            // Change both KS and CF, so they are in sync
            for (auto& pair: db.get_keyspaces()) {
                auto& ks = pair.second;
                ks.set_incremental_backups(value);
            }

            for (auto& pair: db.get_column_families()) {
                auto cf_ptr = pair.second;
                cf_ptr->set_incremental_backups(value);
            }
        }).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::rebuild.set(r, [](std::unique_ptr<request> req) {
        auto source_dc = req->get_query_param("source_dc");
        return service::get_local_storage_service().rebuild(std::move(source_dc)).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::bulk_load.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto path = req->param["path"];
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::bulk_load_async.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto path = req->param["path"];
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::reschedule_failed_deletions.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::load_new_ss_tables.set(r, [&ctx](std::unique_ptr<request> req) {
        auto ks = validate_keyspace(ctx, req->param);
        auto cf = req->get_query_param("cf");
        // No need to add the keyspace, since all we want is to avoid always sending this to the same
        // CPU. Even then I am being overzealous here. This is not something that happens all the time.
        auto coordinator = std::hash<sstring>()(cf) % smp::count;
        return service::get_storage_service().invoke_on(coordinator, [ks = std::move(ks), cf = std::move(cf)] (service::storage_service& s) {
            return s.load_new_sstables(ks, cf);
        }).then_wrapped([] (auto&& f) {
            if (f.failed()) {
                auto msg = fmt::format("Failed to load new sstables: {}", f.get_exception());
                return make_exception_future<json::json_return_type>(httpd::server_error_exception(msg));
            }
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::sample_key_range.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        std::vector<sstring> res;
        return make_ready_future<json::json_return_type>(res);
    });

    ss::reset_local_schema.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::set_trace_probability.set(r, [](std::unique_ptr<request> req) {
        auto probability = req->get_query_param("probability");
        return futurize<json::json_return_type>::apply([probability] {
            double real_prob = std::stod(probability.c_str());
            return tracing::tracing::tracing_instance().invoke_on_all([real_prob] (auto& local_tracing) {
                local_tracing.set_trace_probability(real_prob);
            }).then([] {
                return make_ready_future<json::json_return_type>(json_void());
            });
        }).then_wrapped([probability] (auto&& f) {
            try {
                f.get();
                return make_ready_future<json::json_return_type>(json_void());
            } catch (std::out_of_range& e) {
                throw httpd::bad_param_exception(e.what());
            } catch (std::invalid_argument&){
                throw httpd::bad_param_exception(format("Bad format in a probability value: \"{}\"", probability.c_str()));
            }
        });
    });

    ss::get_trace_probability.set(r, [](std::unique_ptr<request> req) {
        return make_ready_future<json::json_return_type>(tracing::tracing::get_local_tracing_instance().get_trace_probability());
    });

    ss::get_slow_query_info.set(r, [](const_req req) {
        ss::slow_query_info res;
        res.enable = tracing::tracing::get_local_tracing_instance().slow_query_tracing_enabled();
        res.ttl = tracing::tracing::get_local_tracing_instance().slow_query_record_ttl().count() ;
        res.threshold = tracing::tracing::get_local_tracing_instance().slow_query_threshold().count();
        return res;
    });

    ss::set_slow_query.set(r, [](std::unique_ptr<request> req) {
        auto enable = req->get_query_param("enable");
        auto ttl = req->get_query_param("ttl");
        auto threshold = req->get_query_param("threshold");
        try {
            return tracing::tracing::tracing_instance().invoke_on_all([enable, ttl, threshold] (auto& local_tracing) {
                if (threshold != "") {
                    local_tracing.set_slow_query_threshold(std::chrono::microseconds(std::stol(threshold.c_str())));
                }
                if (ttl != "") {
                    local_tracing.set_slow_query_record_ttl(std::chrono::seconds(std::stol(ttl.c_str())));
                }
                if (enable != "") {
                    local_tracing.set_slow_query_enabled(strcasecmp(enable.c_str(), "true") == 0);
                }
            }).then([] {
                return make_ready_future<json::json_return_type>(json_void());
            });
        } catch (...) {
            throw httpd::bad_param_exception(format("Bad format value: "));
        }
    });

    ss::enable_auto_compaction.set(r, [&ctx](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto keyspace = validate_keyspace(ctx, req->param);
        auto column_family = req->get_query_param("cf");
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::disable_auto_compaction.set(r, [&ctx](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto keyspace = validate_keyspace(ctx, req->param);
        auto column_family = req->get_query_param("cf");
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::deliver_hints.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto host = req->get_query_param("host");
        return make_ready_future<json::json_return_type>(json_void());
      });

    ss::get_cluster_name.set(r, [](const_req req) {
        return gms::get_local_gossiper().get_cluster_name();
    });

    ss::get_partitioner_name.set(r, [](const_req req) {
        return gms::get_local_gossiper().get_partitioner_name();
    });

    ss::get_tombstone_warn_threshold.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    ss::set_tombstone_warn_threshold.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto debug_threshold = req->get_query_param("debug_threshold");
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::get_tombstone_failure_threshold.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    ss::set_tombstone_failure_threshold.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto debug_threshold = req->get_query_param("debug_threshold");
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::get_batch_size_failure_threshold.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    ss::set_batch_size_failure_threshold.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto threshold = req->get_query_param("threshold");
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::set_hinted_handoff_throttle_in_kb.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        auto debug_threshold = req->get_query_param("throttle");
        return make_ready_future<json::json_return_type>(json_void());
    });

    ss::get_metrics_load.set(r, [&ctx](std::unique_ptr<request> req) {
        return get_cf_stats(ctx, &column_family::stats::live_disk_space_used);
    });

    ss::get_exceptions.set(r, [](const_req req) {
        return service::get_local_storage_service().get_exception_count();
    });

    ss::get_total_hints_in_progress.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    ss::get_total_hints.set(r, [](std::unique_ptr<request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

    ss::get_ownership.set(r, [] (std::unique_ptr<request> req) {
        return service::get_local_storage_service().get_ownership().then([] (auto&& ownership) {
            std::vector<storage_service_json::mapper> res;
            return make_ready_future<json::json_return_type>(map_to_key_value(ownership, res));
        });
    });

    ss::get_effective_ownership.set(r, [&ctx] (std::unique_ptr<request> req) {
        auto keyspace_name = req->param["keyspace"] == "null" ? "" : validate_keyspace(ctx, req->param);
        return service::get_local_storage_service().effective_ownership(keyspace_name).then([] (auto&& ownership) {
            std::vector<storage_service_json::mapper> res;
            return make_ready_future<json::json_return_type>(map_to_key_value(ownership, res));
        });
    });

    ss::view_build_statuses.set(r, [&ctx] (std::unique_ptr<request> req) {
        auto keyspace = validate_keyspace(ctx, req->param);
        auto view = req->param["view"];
        return service::get_local_storage_service().view_build_statuses(std::move(keyspace), std::move(view)).then([] (std::unordered_map<sstring, sstring> status) {
            std::vector<storage_service_json::mapper> res;
            return make_ready_future<json::json_return_type>(map_to_key_value(std::move(status), res));
        });
    });

    ss::sstable_info.set(r, [&ctx] (std::unique_ptr<request> req) {
        auto ks = api::req_param<sstring>(*req, "keyspace", {}).value;
        auto cf = api::req_param<sstring>(*req, "cf", {}).value;

        // The size of this vector is bound by ks::cf. I.e. it is as most Nks + Ncf long
        // which is not small, but not huge either. 
        using table_sstables_list = std::vector<ss::table_sstables>;

        return do_with(table_sstables_list{}, [ks, cf, &ctx](table_sstables_list& dst) {
            return service::get_local_storage_service().db().map_reduce([&dst](table_sstables_list&& res) {
                for (auto&& t : res) {
                    auto i = std::find_if(dst.begin(), dst.end(), [&t](const ss::table_sstables& t2) {
                        return t.keyspace() == t2.keyspace() && t.table() == t2.table();
                    });
                    if (i == dst.end()) {
                        dst.emplace_back(std::move(t));
                        continue;
                    }
                    auto& ssd = i->sstables; 
                    for (auto&& sd : t.sstables._elements) {
                        auto j = std::find_if(ssd._elements.begin(), ssd._elements.end(), [&sd](const ss::sstable& s) {
                            return s.generation() == sd.generation();
                        });
                        if (j == ssd._elements.end()) {
                            i->sstables.push(std::move(sd));
                        }
                    }
                }
            }, [ks, cf](const database& db) {
                // see above
                table_sstables_list res;

                auto& ext = db.get_config().extensions();

                for (auto& t : db.get_column_families() | boost::adaptors::map_values) {
                    auto& schema = t->schema();
                    if ((ks.empty() || ks == schema->ks_name()) && (cf.empty() || cf == schema->cf_name())) {
                        // at most Nsstables long
                        ss::table_sstables tst;
                        tst.keyspace = schema->ks_name();
                        tst.table = schema->cf_name();

                        for (auto sstable : *t->get_sstables_including_compacted_undeleted()) {
                            auto ts = db_clock::to_time_t(sstable->data_file_write_time());
                            ::tm t;
                            ::gmtime_r(&ts, &t);

                            ss::sstable info;

                            info.timestamp = t;
                            info.generation = sstable->generation();
                            info.level = sstable->get_sstable_level();
                            info.size = sstable->bytes_on_disk();
                            info.data_size = sstable->ondisk_data_size();
                            info.index_size = sstable->index_size();
                            info.filter_size = sstable->filter_size();
                            info.version = sstable->get_version();

                            if (sstable->has_component(sstables::component_type::CompressionInfo)) {
                                auto& c = sstable->get_compression();
                                auto cp = sstables::get_sstable_compressor(c);

                                ss::named_maps nm;
                                nm.group = "compression_parameters";
                                for (auto& p : cp->options()) {
                                    ss::mapper e;
                                    e.key = p.first;
                                    e.value = p.second;
                                    nm.attributes.push(std::move(e));
                                }
                                if (!cp->options().count(compression_parameters::SSTABLE_COMPRESSION)) {
                                    ss::mapper e;
                                    e.key = compression_parameters::SSTABLE_COMPRESSION;
                                    e.value = cp->name();
                                    nm.attributes.push(std::move(e));
                                }
                                info.extended_properties.push(std::move(nm));
                            }

                            sstables::file_io_extension::attr_value_map map;

                            for (auto* ep : ext.sstable_file_io_extensions()) {
                                map.merge(ep->get_attributes(*sstable));
                            }

                            for (auto& p : map) {
                                struct {
                                    const sstring& key; 
                                    ss::sstable& info;
                                    void operator()(const std::map<sstring, sstring>& map) const {
                                        ss::named_maps nm;
                                        nm.group = key;
                                        for (auto& p : map) {
                                            ss::mapper e;
                                            e.key = p.first;
                                            e.value = p.second;
                                            nm.attributes.push(std::move(e));
                                        }
                                        info.extended_properties.push(std::move(nm));
                                    }
                                    void operator()(const sstring& value) const {
                                        ss::mapper e;
                                        e.key = key;
                                        e.value = value;
                                        info.properties.push(std::move(e));                                        
                                    }
                                } v{p.first, info};

                                std::visit(v, p.second);
                            }

                            tst.sstables.push(std::move(info));
                        }
                        res.emplace_back(std::move(tst));
                    }
                }
                std::sort(res.begin(), res.end(), [](const ss::table_sstables& t1, const ss::table_sstables& t2) {
                    return t1.keyspace() < t2.keyspace() || (t1.keyspace() == t2.keyspace() && t1.table() < t2.table());
                });
                return res;
            }).then([&dst] {
                return make_ready_future<json::json_return_type>(stream_object(dst));
            });
        });
    });

}

}
