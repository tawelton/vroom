/*

This file is part of VROOM.

Copyright (c) 2015-2018, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include "problems/vrptw/local_search/local_search.h"
#include "problems/ls_operator.h"
#include "problems/vrptw/heuristics/solomon.h"
#include "problems/vrptw/local_search/2_opt.h"
#include "problems/vrptw/local_search/cross_exchange.h"
#include "problems/vrptw/local_search/exchange.h"
#include "problems/vrptw/local_search/or_opt.h"
#include "problems/vrptw/local_search/relocate.h"
#include "problems/vrptw/local_search/reverse_2_opt.h"
#include "utils/helpers.h"

#include "utils/output_json.h"

unsigned vrptw_local_search::ls_rank = 0;

vrptw_local_search::vrptw_local_search(const input& input, tw_solution& tw_sol)
  : local_search(input),
    _tw_sol(tw_sol),
    log(false),
    log_iter(0),
    log_name("debug_" + std::to_string(++ls_rank) + "_") {
  // Setup solution state.
  _sol_state.setup(_tw_sol);
}

void vrptw_local_search::try_job_additions(const std::vector<index_t>& routes,
                                           double regret_coeff) {
  bool job_added;

  do {
    double best_cost = std::numeric_limits<double>::max();
    index_t best_job = 0;
    index_t best_route = 0;
    index_t best_rank = 0;

    for (const auto j : _sol_state.unassigned) {
      auto& current_amount = _input._jobs[j].amount;
      std::vector<gain_t> best_costs(routes.size(),
                                     std::numeric_limits<gain_t>::max());
      std::vector<index_t> best_ranks(routes.size());

      for (std::size_t i = 0; i < routes.size(); ++i) {
        auto v = routes[i];
        const auto& v_target = _input._vehicles[v];
        const amount_t& v_amount = _sol_state.total_amount(v);

        if (_input.vehicle_ok_with_job(v, j) and
            v_amount + current_amount <= _input._vehicles[v].capacity) {
          for (std::size_t r = 0; r <= _tw_sol[v].route.size(); ++r) {
            if (_tw_sol[v].is_valid_addition_for_tw(_input, j, r)) {
              gain_t current_cost =
                addition_cost(_input, _m, j, v_target, _tw_sol[v].route, r);

              if (current_cost < best_costs[i]) {
                best_costs[i] = current_cost;
                best_ranks[i] = r;
              }
            }
          }
        }
      }

      auto smallest = std::numeric_limits<gain_t>::max();
      auto second_smallest = std::numeric_limits<gain_t>::max();
      std::size_t smallest_idx = std::numeric_limits<gain_t>::max();

      for (std::size_t i = 0; i < routes.size(); ++i) {
        if (best_costs[i] < smallest) {
          smallest_idx = i;
          second_smallest = smallest;
          smallest = best_costs[i];
        } else if (best_costs[i] < second_smallest) {
          second_smallest = best_costs[i];
        }
      }

      // Find best route for current job based on cost of addition and
      // regret cost of not adding.
      for (std::size_t i = 0; i < routes.size(); ++i) {
        auto addition_cost = best_costs[i];
        if (addition_cost == std::numeric_limits<gain_t>::max()) {
          continue;
        }
        auto regret_cost = std::numeric_limits<gain_t>::max();
        if (i == smallest_idx) {
          regret_cost = second_smallest;
        } else {
          regret_cost = smallest;
        }

        double eval = static_cast<double>(addition_cost) -
                      regret_coeff * static_cast<double>(regret_cost);

        if (eval < best_cost) {
          best_cost = eval;
          best_job = j;
          best_route = routes[i];
          best_rank = best_ranks[i];
        }
      }
    }

    job_added = (best_cost < std::numeric_limits<double>::max());

    if (job_added) {
      _tw_sol[best_route].add(_input, best_job, best_rank);

      // Update amounts after addition.
      const auto& job_amount = _input._jobs[best_job].amount;
      auto& best_fwd_amounts = _sol_state.fwd_amounts[best_route];
      auto previous_cumul = (best_rank == 0) ? amount_t(_input.amount_size())
                                             : best_fwd_amounts[best_rank - 1];
      best_fwd_amounts.insert(best_fwd_amounts.begin() + best_rank,
                              previous_cumul + job_amount);
      std::for_each(best_fwd_amounts.begin() + best_rank + 1,
                    best_fwd_amounts.end(),
                    [&](auto& a) { a += job_amount; });

      auto& best_bwd_amounts = _sol_state.bwd_amounts[best_route];
      best_bwd_amounts.insert(best_bwd_amounts.begin() + best_rank,
                              amount_t(_input.amount_size())); // dummy init
      auto total_amount = _sol_state.fwd_amounts[best_route].back();
      for (std::size_t i = 0; i <= best_rank; ++i) {
        _sol_state.bwd_amounts[best_route][i] =
          total_amount - _sol_state.fwd_amounts[best_route][i];
      }

      // Update cost after addition.
      _sol_state.update_route_cost(_tw_sol[best_route].route, best_route);

      _sol_state.unassigned.erase(best_job);
    }
  } while (job_added);
}

void vrptw_local_search::log_current_solution() {
  if (log) {
    write_to_json(format_solution(_input, _tw_sol),
                  false,
                  log_name + std::to_string(++log_iter) + "_sol.json");
  }
}

void vrptw_local_search::run() {
  log_current_solution();

  std::vector<std::vector<std::unique_ptr<ls_operator>>> best_ops(V);
  for (std::size_t v = 0; v < V; ++v) {
    best_ops[v] = std::vector<std::unique_ptr<ls_operator>>(V);
  }

  // List of source/target pairs we need to test (all at first).
  std::vector<std::pair<index_t, index_t>> s_t_pairs;
  for (unsigned s_v = 0; s_v < V; ++s_v) {
    for (unsigned t_v = 0; t_v < V; ++t_v) {
      if (s_v == t_v) {
        continue;
      }
      s_t_pairs.emplace_back(s_v, t_v);
    }
  }

  std::vector<std::vector<gain_t>> best_gains(V, std::vector<gain_t>(V, 0));

  gain_t best_gain = 1;

  while (best_gain > 0) {
    // Exchange stuff
    for (const auto& s_t : s_t_pairs) {
      if (s_t.second <= s_t.first or // This operator is symmetric.
          _tw_sol[s_t.first].route.size() == 0 or
          _tw_sol[s_t.second].route.size() == 0) {
        continue;
      }

      for (unsigned s_rank = 0; s_rank < _tw_sol[s_t.first].route.size();
           ++s_rank) {
        for (unsigned t_rank = 0; t_rank < _tw_sol[s_t.second].route.size();
             ++t_rank) {
          vrptw_exchange r(_input,
                           _sol_state,
                           _tw_sol,
                           s_t.first,
                           s_rank,
                           s_t.second,
                           t_rank);
          if (r.is_valid() and r.gain() > best_gains[s_t.first][s_t.second]) {
            best_gains[s_t.first][s_t.second] = r.gain();
            best_ops[s_t.first][s_t.second] =
              std::make_unique<vrptw_exchange>(r);
          }
        }
      }
    }

    // CROSS-exchange stuff
    for (const auto& s_t : s_t_pairs) {
      if (s_t.second <= s_t.first or // This operator is symmetric.
          _tw_sol[s_t.first].route.size() < 2 or
          _tw_sol[s_t.second].route.size() < 2) {
        continue;
      }

      for (unsigned s_rank = 0; s_rank < _tw_sol[s_t.first].route.size() - 1;
           ++s_rank) {
        for (unsigned t_rank = 0; t_rank < _tw_sol[s_t.second].route.size() - 1;
             ++t_rank) {
          vrptw_cross_exchange r(_input,
                                 _sol_state,
                                 _tw_sol,
                                 s_t.first,
                                 s_rank,
                                 s_t.second,
                                 t_rank);
          if (r.is_valid() and r.gain() > best_gains[s_t.first][s_t.second]) {
            best_gains[s_t.first][s_t.second] = r.gain();
            best_ops[s_t.first][s_t.second] =
              std::make_unique<vrptw_cross_exchange>(r);
          }
        }
      }
    }

    // 2-opt* stuff
    for (const auto& s_t : s_t_pairs) {
      if (s_t.second <= s_t.first) {
        // This operator is symmetric.
        continue;
      }
      for (unsigned s_rank = 0; s_rank < _tw_sol[s_t.first].route.size();
           ++s_rank) {
        auto s_free_amount = _input._vehicles[s_t.first].capacity;
        s_free_amount -= _sol_state.fwd_amounts[s_t.first][s_rank];
        for (int t_rank = _tw_sol[s_t.second].route.size() - 1; t_rank >= 0;
             --t_rank) {
          if (!(_sol_state.bwd_amounts[s_t.second][t_rank] <= s_free_amount)) {
            break;
          }
          vrptw_two_opt r(_input,
                          _sol_state,
                          _tw_sol,
                          s_t.first,
                          s_rank,
                          s_t.second,
                          t_rank);
          if (r.is_valid() and r.gain() > best_gains[s_t.first][s_t.second]) {
            best_gains[s_t.first][s_t.second] = r.gain();
            best_ops[s_t.first][s_t.second] =
              std::make_unique<vrptw_two_opt>(r);
          }
        }
      }
    }

    // Reverse 2-opt* stuff
    for (const auto& s_t : s_t_pairs) {
      for (unsigned s_rank = 0; s_rank < _tw_sol[s_t.first].route.size();
           ++s_rank) {
        auto s_free_amount = _input._vehicles[s_t.first].capacity;
        s_free_amount -= _sol_state.fwd_amounts[s_t.first][s_rank];
        for (unsigned t_rank = 0; t_rank < _tw_sol[s_t.second].route.size();
             ++t_rank) {
          if (!(_sol_state.fwd_amounts[s_t.second][t_rank] <= s_free_amount)) {
            break;
          }
          vrptw_reverse_two_opt r(_input,
                                  _sol_state,
                                  _tw_sol,
                                  s_t.first,
                                  s_rank,
                                  s_t.second,
                                  t_rank);
          if (r.is_valid() and r.gain() > best_gains[s_t.first][s_t.second]) {
            best_gains[s_t.first][s_t.second] = r.gain();
            best_ops[s_t.first][s_t.second] =
              std::make_unique<vrptw_reverse_two_opt>(r);
          }
        }
      }
    }

    // Relocate stuff
    for (const auto& s_t : s_t_pairs) {
      if (_tw_sol[s_t.first].route.size() == 0 or
          !(_sol_state.total_amount(s_t.second) + _amount_lower_bound <=
            _input._vehicles[s_t.second].capacity)) {
        // Don't try to put things in a full vehicle or from an empty
        // vehicle.
        continue;
      }
      for (unsigned s_rank = 0; s_rank < _tw_sol[s_t.first].route.size();
           ++s_rank) {
        if (_sol_state.node_gains[s_t.first][s_rank] <=
            best_gains[s_t.first][s_t.second]) {
          // Except if addition cost in route s_t.second is negative
          // (!!), overall gain can't exceed current known best gain.
          continue;
        }
        for (unsigned t_rank = 0; t_rank <= _tw_sol[s_t.second].route.size();
             ++t_rank) {
          vrptw_relocate r(_input,
                           _sol_state,
                           _tw_sol,
                           s_t.first,
                           s_rank,
                           s_t.second,
                           t_rank);
          if (r.is_valid() and r.gain() > best_gains[s_t.first][s_t.second]) {
            best_gains[s_t.first][s_t.second] = r.gain();
            best_ops[s_t.first][s_t.second] =
              std::make_unique<vrptw_relocate>(r);
          }
        }
      }
    }

    // Or-opt stuff
    for (const auto& s_t : s_t_pairs) {
      if (_tw_sol[s_t.first].route.size() < 2 or
          !(_sol_state.total_amount(s_t.second) + _double_amount_lower_bound <=
            _input._vehicles[s_t.second].capacity)) {
        // Don't try to put things in a full vehicle or from a
        // (near-)empty vehicle.
        continue;
      }
      for (unsigned s_rank = 0; s_rank < _tw_sol[s_t.first].route.size() - 1;
           ++s_rank) {
        if (_sol_state.edge_gains[s_t.first][s_rank] <=
            best_gains[s_t.first][s_t.second]) {
          // Except if addition cost in route s_t.second is negative
          // (!!), overall gain can't exceed current known best gain.
          continue;
        }
        for (unsigned t_rank = 0; t_rank <= _tw_sol[s_t.second].route.size();
             ++t_rank) {
          vrptw_or_opt r(_input,
                         _sol_state,
                         _tw_sol,
                         s_t.first,
                         s_rank,
                         s_t.second,
                         t_rank);
          if (r.is_valid() and r.gain() > best_gains[s_t.first][s_t.second]) {
            best_gains[s_t.first][s_t.second] = r.gain();
            best_ops[s_t.first][s_t.second] = std::make_unique<vrptw_or_opt>(r);
          }
        }
      }
    }

    // Find best overall gain.
    best_gain = 0;
    index_t best_source = 0;
    index_t best_target = 0;

    for (unsigned s_v = 0; s_v < V; ++s_v) {
      for (unsigned t_v = 0; t_v < V; ++t_v) {
        if (s_v == t_v) {
          continue;
        }
        if (best_gains[s_v][t_v] > best_gain) {
          best_gain = best_gains[s_v][t_v];
          best_source = s_v;
          best_target = t_v;
        }
      }
    }

    // Apply matching operator.
    if (best_gain > 0) {
      assert(best_ops[best_source][best_target] != nullptr);

      best_ops[best_source][best_target]->apply();

      // Update route costs.
      auto previous_cost = _sol_state.route_costs[best_source] +
                           _sol_state.route_costs[best_target];
      _sol_state.update_route_cost(_tw_sol[best_source].route, best_source);
      _sol_state.update_route_cost(_tw_sol[best_target].route, best_target);
      auto new_cost = _sol_state.route_costs[best_source] +
                      _sol_state.route_costs[best_target];

      assert(new_cost + best_gain == previous_cost);

      straighten_route(best_source);
      straighten_route(best_target);

      // We need to run update_amounts before try_job_additions to
      // correctly evaluate amounts. No need to run it again after
      // since try_before_additions will subsequently fix amounts upon
      // each addition.
      _sol_state.update_amounts(_tw_sol[best_source].route, best_source);
      _sol_state.update_amounts(_tw_sol[best_target].route, best_target);

      try_job_additions(best_ops[best_source][best_target]
                          ->addition_candidates(),
                        0);

      // Running update_costs only after try_job_additions is fine.
      _sol_state.update_costs(_tw_sol[best_source].route, best_source);
      _sol_state.update_costs(_tw_sol[best_target].route, best_target);

      _sol_state.update_skills(_tw_sol[best_source].route, best_source);
      _sol_state.update_skills(_tw_sol[best_target].route, best_target);

      // Update candidates.
      _sol_state.set_node_gains(_tw_sol[best_source].route, best_source);
      _sol_state.set_node_gains(_tw_sol[best_target].route, best_target);
      _sol_state.set_edge_gains(_tw_sol[best_source].route, best_source);
      _sol_state.set_edge_gains(_tw_sol[best_target].route, best_target);

      // Set gains to zero for what needs to be recomputed in the next
      // round.
      s_t_pairs.clear();
      best_gains[best_source] = std::vector<gain_t>(V, 0);
      best_gains[best_target] = std::vector<gain_t>(V, 0);

      s_t_pairs.emplace_back(best_source, best_target);
      s_t_pairs.emplace_back(best_target, best_source);

      for (unsigned v = 0; v < V; ++v) {
        if (v == best_source or v == best_target) {
          continue;
        }
        s_t_pairs.emplace_back(best_source, v);
        s_t_pairs.emplace_back(v, best_source);
        best_gains[v][best_source] = 0;
        best_gains[best_source][v] = 0;

        s_t_pairs.emplace_back(best_target, v);
        s_t_pairs.emplace_back(v, best_target);
        best_gains[v][best_target] = 0;
        best_gains[best_target][v] = 0;
      }
    }

    log_current_solution();
  }
}

void vrptw_local_search::straighten_route(index_t route_rank) {
  if (_tw_sol[route_rank].route.size() > 0) {
    auto before_cost = _sol_state.route_costs[route_rank];

    auto new_tw_r = single_route_heuristic(_input, _tw_sol[route_rank], true);

    auto other_tw_r =
      single_route_heuristic(_input, _tw_sol[route_rank], false);

    if (other_tw_r.route.size() > new_tw_r.route.size() or
        (other_tw_r.route.size() == new_tw_r.route.size() and
         _sol_state.route_cost_for_vehicle(route_rank, other_tw_r.route) <
           _sol_state.route_cost_for_vehicle(route_rank, new_tw_r.route))) {
      new_tw_r = std::move(other_tw_r);
    }

    if (new_tw_r.route.size() == _tw_sol[route_rank].route.size()) {
      auto after_cost =
        _sol_state.route_cost_for_vehicle(route_rank, new_tw_r.route);

      if (after_cost < before_cost) {
        log_current_solution();

        _tw_sol[route_rank] = std::move(new_tw_r);
        _sol_state.route_costs[route_rank] = after_cost;
      }
    }
  }
}

solution_indicators vrptw_local_search::indicators() const {
  solution_indicators si;

  si.unassigned = _sol_state.unassigned.size();
  si.cost = 0;
  for (std::size_t v = 0; v < V; ++v) {
    si.cost += _sol_state.route_costs[v];
  }
  si.used_vehicles =
    std::count_if(_tw_sol.begin(), _tw_sol.end(), [](const auto& tw_r) {
      return !tw_r.route.empty();
    });
  return si;
}
