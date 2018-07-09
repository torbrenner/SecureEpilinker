/**
 \file    secure_epilinker.cpp
 \author  Sebastian Stammler <sebastian.stammler@cysec.de>
 \copyright SEL - Secure EpiLinker
      Copyright (C) 2018 Computational Biology & Simulation Group TU-Darmstadt
      This program is free software: you can redistribute it and/or modify
      it under the terms of the GNU Affero General Public License as published
      by the Free Software Foundation, either version 3 of the License, or
      (at your option) any later version.
      This program is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
      GNU Affero General Public License for more details.
      You should have received a copy of the GNU Affero General Public License
      along with this program. If not, see <http://www.gnu.org/licenses/>.
 \brief Encapsulation class for the secure epilink s2PC process
*/

#include <stdexcept>
#include <algorithm>
#include "fmt/format.h"
#ifdef DEBUG_SEL_CIRCUIT
#include "fmt/ostream.h"
#endif
using fmt::format, fmt::print;
#include "util.h"
#include "secure_epilinker.h"
#include "math.h"
#include "aby/Share.h"
#include "aby/gadgets.h"
#include "seltypes.h"

using namespace std;

namespace sel {

constexpr auto BIN = FieldComparator::BINARY;
constexpr auto BM = FieldComparator::NGRAM;

// hammingweight of bitmasks
CircUnit hw(const Bitmask& bm) {
  CircUnit n = 0;
  for (auto& b : bm) {
    n += __builtin_popcount(b);
  }
  return n;
}

// hammingweight over vectors
vector<CircUnit> hw(const vector<Bitmask>& v_bm) {
  vector<CircUnit> res(v_bm.size());
  transform(v_bm.cbegin(), v_bm.cend(), res.begin(),
      [] (const Bitmask& x) -> CircUnit { return hw(x); });
  return res;
}

/***************** Circuit gadgets *******************/
/**
  * Return type of weight_compare_*()
  * fw - field weight = weight * comparison * empty-flags
  * w - weight for weight sum = weight * empyt-flags
  */
struct FieldWeight { ArithShare fw, w; };

#ifdef DEBUG_SEL_CIRCUIT
void print_share(const FieldWeight& q, const string& msg) {
  print_share(q.fw, msg + "(field-w)");
  print_share(q.w, msg + "(weight)");
}
#endif

/**
 * Sums all fw's and w's in given vector and returns sums as ArithQuotient
 */
ArithQuotient sum(const vector<FieldWeight>& fweights) {
  size_t size = fweights.size();
  vector<ArithShare> fws, ws;
  fws.reserve(size);
  ws.reserve(size);
  for (const auto& fweight : fweights) {
    fws.emplace_back(fweight.fw);
    ws.emplace_back(fweight.w);
  }
  return {sum(fws), sum(ws)};
}

/**
 * Calculates dice coefficient of given bitmasks and their hamming weights, up
 * to the specified precision
 */
BoolShare dice_coefficient(const BoolShare& x, const BoolShare& y,
    const BoolShare& hw_x, const BoolShare& hw_y, size_t prec) {
  // calc HW of AND and bit-shift to multiply with 2 and get dice precision
  // for integer-divivision
  BoolShare hw_and = hammingweight(x & y);
  BoolShare hw_and_shifted = hw_and << (prec + 1);
#ifdef DEBUG_SEL_CIRCUIT
  print_share(hw_and, "hw_and");
  print_share(hw_and_shifted, "hw_and_shifted");
#endif

  // Add single HWs
  BoolShare hw_plus = hw_x + hw_y;
#ifdef DEBUG_SEL_CIRCUIT
  print_share(hw_plus, "hw_plus");
#endif

  // int-divide
  BoolShare dice = apply_file_binary(hw_and_shifted, hw_plus, 16, 16, "circ/int_div_16.aby");
#ifdef DEBUG_SEL_CIRCUIT
  print_share(dice, "dice coeff");
#endif
  return dice;
}

/*
 * Binary-compares two shares
 */
BoolShare equality(const BoolShare& x, const BoolShare& y) {
  BoolShare cmp{(x == y)};
#ifdef DEBUG_SEL_CIRCUIT
  print_share(cmp, "equality");
#endif
  return cmp;
}

/******************** Circuit Builder ********************/
class SecureEpilinker::SELCircuit {
public:
  SELCircuit(EpilinkConfig cfg_,
    BooleanCircuit* bcirc, BooleanCircuit* ccirc, ArithmeticCircuit* acirc) :
    cfg{cfg_}, bcirc{bcirc}, ccirc{ccirc}, acirc{acirc},
    ins{transform_map(cfg.fields, [](auto){ return InputShares{}; })},
    to_bool_closure{[this](auto x){return to_bool(x);}},
    to_arith_closure{[this](auto x){return to_arith(x);}}
  {}

  void set_client_input(const EpilinkClientInput& input) {
    set_constants(input.nvals);
    set_real_client_input(input);
    set_dummy_server_input();

    is_input_set = true;
  }

  void set_server_input(const EpilinkServerInput& input) {
    set_constants(input.nvals);
    set_real_server_input(input);
    set_dummy_client_input();

    is_input_set = true;
  }

#ifdef DEBUG_SEL_CIRCUIT
  /**
   * Debugging test_both_inputs() to test exactly mirrored inputs.
   */
  void set_both_inputs(const EpilinkClientInput& in_client, const EpilinkServerInput& in_server) {
    set_constants(in_client.nvals);
    set_real_client_input(in_client);
    set_real_server_input(in_server);

    is_input_set = true;
  }
#endif

  struct ResultShares {
    OutShare index, match, tmatch;
#ifdef DEBUG_SEL_RESULT
    OutShare score_numerator, score_denominator;
#endif
  };

  /*
  * Builds the shared component of the circuit after initial input shares of
  * client and server have been created.
  */
  ResultShares build_circuit() {
    // Where we store all group and individual comparison weights as ariths
    vector<FieldWeight> field_weights;
    //field_weights.reserve(cfg.bm_exchange_groups.size() + cfg.bin_exchange_groups.size());

    // 1. Field weights of individual fields
    // 1.1 For all exchange groups, find the permutation with the highest score
    // Where we collect indices not already used in an exchange group
    IndexSet no_x_group;
    // fill with field names, remove later
    for (const auto& field : cfg.fields) no_x_group.emplace(field.first);
    // for each group, store the best permutation's weight into field_weights
    for (const auto& group : cfg.exchange_groups) {
      // add this group's field weight to vector
      field_weights.emplace_back(best_group_weight(group));
      // remove all indices that were covered by this index group
      for (const auto& i : group) {
        size_t r = no_x_group.erase(i);
        // TODO better check this already in EpilinkConfig constructor
        if (!r) throw runtime_error("Exchange groups must be distinct!");
      }
    }
    // 1.2 Remaining indices
    for (const auto& i : no_x_group) {
      field_weights.emplace_back(field_weight(i, i));
    }

    // 2. Sum up all field weights.
    ArithQuotient sum_field_weights{sum(field_weights)};
#ifdef DEBUG_SEL_CIRCUIT
    print_share(sum_field_weights, "sum_field_weights");
#endif

    // 4. Determine index of max score of all nvals calculations
    // Create targets vector with const_idx copy to pass to
    // split_select_quotient_target().
    vector<BoolShare> max_idx{1, const_idx};
    assert((bool)const_idx); // check that it was copied, not moved
    split_select_quotient_target(sum_field_weights, max_idx,
        make_max_selector(to_bool_closure), to_arith_closure);

    // 5. Left side: sum up all weights and multiply with threshold
    //  Since weights and threshold are public inputs, this can be computed
    //  locally. This is done in set_constants().
    //
    // 6. Set two comparison bits, whether > (tentative) threshold
    BoolShare threshold_weight = to_bool(const_threshold * sum_field_weights.den);
    BoolShare tthreshold_weight = to_bool(const_tthreshold * sum_field_weights.den);
    BoolShare b_sum_field_weight = to_bool(sum_field_weights.num);
    BoolShare match = threshold_weight < b_sum_field_weight;
    BoolShare tmatch = tthreshold_weight < b_sum_field_weight;
#ifdef DEBUG_SEL_CIRCUIT
    print_share(sum_field_weights, "best score");
    print_share(max_idx[0], "index of best score");
    print_share(threshold_weight, "T*W");
    print_share(tthreshold_weight, "Tt*W");
    print_share(match, "match?");
    print_share(tmatch, "tentative match?");
#endif

#ifdef DEBUG_SEL_RESULT
      //TODO Actually return W = sum of non-empty field weights
    return {out(max_idx[0], ALL), out(match, ALL), out(tmatch, ALL),
      out(sum_field_weights.num, ALL),
      out(sum_field_weights.den, ALL)};
#else
    return {out_shared(max_idx[0]), out_shared(match), out_shared(tmatch)};
#endif
  }

private:
  // Epilink config
  const EpilinkConfig cfg;
  // Circuits
  BooleanCircuit* bcirc; // boolean circuit for boolean parts
  BooleanCircuit* ccirc; // intermediate conversion circuit
  ArithmeticCircuit* acirc;
  // Input shares
  struct ValueShare {
    BoolShare val; // value as bool
    BoolShare empty; // empty flag - will probably be kicked out
    ArithShare delta; // 1 if non-empty, 0 o/w
    BoolShare hw; // precomputed HW of val - not used for bin
  };
  struct InputShares {
    ValueShare client, server;
  };
  /**
   * All input shares are stored in this map, keyed by fieldname
   * Value is a vector over all fields, and each field holds the ValueShare for
   * client and server. A ValueShare holds the value of the field itself, a
   * delta flag, which is 1 if the field is non-empty, and the precalculated
   * hammingweight for bitmasks.
   */
  map<FieldName, InputShares> ins;
  // Constant shares
  BoolShare const_idx;
  // Left side of inequality: T * sum(weights)
  ArithShare const_threshold, const_tthreshold;

  // state variables
  uint32_t nvals{0};
  bool is_input_set{false};

  // Dynamic converters, dependent on main bool sharing
  BoolShare to_bool(const ArithShare& s) {
    return (bcirc->GetContext() == S_YAO) ? a2y(bcirc, s) : a2b(bcirc, ccirc, s);
  }

  ArithShare to_arith(const BoolShare& s_) {
    BoolShare s{s_.zeropad(BitLen)}; // fix for aby issue #46
    return (bcirc->GetContext() == S_YAO) ? y2a(acirc, ccirc, s) : b2a(acirc, s);
  }

  // closures
  const A2BConverter to_bool_closure;
  const B2AConverter to_arith_closure;

  void set_constants(uint32_t nvals) {
    this->nvals = nvals;
    // build constant index vector
    vector<BoolShare> numbers;
    numbers.reserve(nvals);
    for (CircUnit i = 0; i != nvals; ++i) {
      // TODO Make true SIMD constants available in ABY and implement offline
      // AND with constant
      numbers.emplace_back(constant(bcirc, i, ceil_log2_min1(nvals)));
    }
    const_idx = vcombine_bool(numbers);
    assert(const_idx.get_nvals() == nvals);


    CircUnit T = cfg.threshold * (1 << cfg.dice_prec);
    CircUnit Tt = cfg.tthreshold * (1 << cfg.dice_prec);
#ifdef DEBUG_SEL_INPUT
    cout << " T: " << hex << T << " Tt: " << hex << Tt << endl;
#endif

    const_threshold = constant(acirc, T, BitLen);
    const_tthreshold = constant(acirc, Tt, BitLen);
#ifdef DEBUG_SEL_CIRCUIT
    print_share(const_idx, "const_idx");
    print_share(const_threshold , "const_threshold ");
    print_share(const_tthreshold , "const_tthreshold ");
#endif
  }

  void set_real_client_input(const EpilinkClientInput& input) {
    assert (nvals > 0 && "call set_constants() before set_*_input()");

    for (const auto& _f : cfg.fields) {
      const FieldName& i = _f.first;
      const auto& f = _f.second;
      const FieldEntry& entry = input.record.at(i);
      size_t bytesize = bitbytes(f.bitsize);
      Bitmask value = entry.value_or(Bitmask(bytesize));
      check_vector_size(value, bytesize, "client input byte vector "s + i);
      ValueShare& sin = ins[i].client;

      // value
      sin.val = BoolShare(bcirc,
          repeat_vec(value, nvals).data(),
          f.bitsize, CLIENT, nvals);

      // Set hammingweight input share only for bitmasks
      if (f.comparator == BM) {
        sin.hw = BoolShare(bcirc,
            vector<CircUnit>(nvals, hw(value)).data(),
            cfg.size_hw, CLIENT, nvals);
      }

      // delta
      sin.delta = ArithShare(acirc,
          vector<CircUnit>(nvals, entry.has_value()).data(),
          BitLen, CLIENT, nvals);

#ifdef DEBUG_SEL_CIRCUIT
      print_share(sin.val, format("client val[{}]", i));
      print_share(sin.hw, format("client hw[{}]", i));
      print_share(sin.delta, format("client delta[{}]", i));
#endif
    }
  }

  void set_real_server_input(const EpilinkServerInput& input) {
    assert (nvals > 0 && "call set_constants() before set_*_input()");

    vector<CircUnit> db_delta(nvals);
    for (const auto& _f : cfg.fields) {
      const FieldName& i = _f.first;
      const auto& f = _f.second;
      const VFieldEntry& entries = input.database.at(i);
      size_t bytesize = bitbytes(f.bitsize);
      VBitmask values = transform_vec(entries,
          [&bytesize](auto e){return e.value_or(Bitmask(bytesize));});
      check_vectors_size(values, bytesize, "server input byte vector "s + i);
      ValueShare& sin = ins[i].server;

      // value
      sin.val = BoolShare(bcirc,
          concat_vec(values).data(), f.bitsize, SERVER, nvals);

      // Set hammingweight input share only for bitmasks
      if (f.comparator == BM) {
        sin.hw = BoolShare(bcirc,
            hw(values).data(), cfg.size_hw, SERVER, nvals);
      }

      // delta
      for (size_t j=0; j!=nvals; ++j) db_delta[j] = entries[j].has_value();
      sin.delta = ArithShare(acirc, db_delta.data(), BitLen, SERVER, nvals);

#ifdef DEBUG_SEL_CIRCUIT
      print_share(sin.val, format("server val[{}]", i));
      print_share(sin.hw, format("server hw[{}]", i));
      print_share(sin.delta, format("server delta[{}]", i));
#endif
    }
  }

  void set_dummy_client_input() {
    assert (nvals > 0 && "call set_constants() before set_*_input()");

    for (const auto& _f : cfg.fields) {
      const FieldName& i = _f.first;
      const auto& f = _f.second;
      ValueShare& sin = ins[i].client;
      sin.val = BoolShare(bcirc, f.bitsize, nvals); //dummy

      if (f.comparator == BM) {
        sin.hw = BoolShare(bcirc, cfg.size_hw, nvals); //dummy
      }

      sin.delta = ArithShare(acirc, BitLen, nvals); // dummy
#ifdef DEBUG_SEL_CIRCUIT
      print_share(sin.val, format("dummy client val[{}]", i));
      print_share(sin.hw, format("dummy client hw[{}]", i));
      print_share(sin.delta, format("dummy client delta[{}]", i));
#endif
    }
  }

  void set_dummy_server_input() {
    assert (nvals > 0 && "call set_constants() before set_*_input()");

    for (const auto& _f : cfg.fields) {
      const FieldName& i = _f.first;
      const auto& f = _f.second;
      ValueShare& sin = ins[i].server;
      sin.val = BoolShare(bcirc, f.bitsize, nvals); //dummy

      if (f.comparator == BM) {
        sin.hw = BoolShare(bcirc, cfg.size_hw, nvals); //dummy
      }

      sin.delta = ArithShare(acirc, BitLen, nvals); // dummy
#ifdef DEBUG_SEL_CIRCUIT
      print_share(sin.val, format("dummy server val[{}]", i));
      print_share(sin.hw, format("dummy server hw[{}]", i));
      print_share(sin.delta, format("dummy server delta[{}]", i));
#endif
    }
  }

  FieldWeight best_group_weight(const IndexSet& group_set) {
    vector<FieldName> group{begin(group_set), end(group_set)};
    // copy group to store permutations
    vector<FieldName> groupPerm{group.begin(), group.end()};
    size_t size = group.size();
    vector<ArithQuotient> perm_weights; // where we store all weights before max
    perm_weights.reserve(factorial<size_t>(size));
    // iterate over all group permutations and calc field-weight
    do {
      vector<FieldWeight> field_weights;
      field_weights.reserve(size);
      for (size_t i = 0; i != size; ++i) {
        const auto& ileft = group[i];
        const auto& iright = groupPerm[i];
        field_weights.emplace_back(field_weight(ileft, iright));
      }
      // sum all field-weights for this permutation
      ArithQuotient sum_perm_weight{sum(field_weights)};
#ifdef DEBUG_SEL_CIRCUIT
      print_share(sum_perm_weight,
          //format("sum_perm_weight ({}|{})", group, groupPerm )); // TODO#19
          "sum_perm_weight"); // TODO#19
#endif
      // collect for later max
      perm_weights.emplace_back(sum_perm_weight);
    } while(next_permutation(groupPerm.begin(), groupPerm.end()));
    // return max of all perm_weights
    ArithQuotient max_perm_weight{max(perm_weights, to_bool_closure, to_arith_closure)};
#ifdef DEBUG_SEL_CIRCUIT
    print_share(max_perm_weight,
        //format("max_perm_weight ({})", group)); // TODO#19
        "max_perm_weight"); // TODO#19
#endif
    // Treat quotient as FieldWeight
    return {max_perm_weight.num, max_perm_weight.den};
  }

  /**
   * Calculates the field weight and addend to the total weight.
   * - Set rescaled weight as arithmetic constant
   * - Set weight to 0 if any field on either side is empty
   * - Run comparison, dependent on field type:
   *   - Bitmasks: Dice coefficient with precision dice_prec
   *   - Binary: Simple Equality, but 0/1 shifted to the left by same dice_prec
   * - Multiply result of comparison with weight -> field weight
   * - Return field weight and weight
   */
  FieldWeight field_weight(const FieldName& ileft, const FieldName& iright) {
    const ML_Field& fleft = cfg.fields.at(ileft), fright = cfg.fields.at(iright);
    const FieldComparator ftype = fleft.comparator;
    if (ftype != fright.comparator) {
      throw runtime_error{fmt::format(
          "Cannot compare field '{}' of type {} with field '{}' of type {}",
          ileft, ftype, iright, fright.comparator)};
    }

    // Check that bitsizes of both fields match
    if (fleft.bitsize != fright.bitsize) {
      throw runtime_error{fmt::format(
          "Cannot compare field '{}' of bitsize {} with field '{}' of bitsize {}",
          ileft, fleft.bitsize, iright, fright.comparator)};
    }

    // 1. Calculate weight * delta(i,j)
    // If indices match, use precomputed rescaled weights. Otherwise take
    // arithmetic average of both weights
    CircUnit weight_r = rescale_weight(
        (fleft.weight + fright.weight)/2,
        cfg.weight_prec, cfg.max_weight);

    ArithShare a_weight{constant_simd(acirc, weight_r, BitLen, nvals)};

    const auto& client_entry = ins[ileft].client;
    const auto& server_entry = ins[iright].server;
    ArithShare delta = client_entry.delta * server_entry.delta;
    ArithShare weight = a_weight * delta; // free constant multiplication

    // 2. Compare values (with dice precision) and multiply with weight
    BoolShare b_comp;
    ArithShare comp;
    switch(ftype) {
      case BM: {
        b_comp = dice_coefficient(client_entry.val, server_entry.val,
          client_entry.hw, server_entry.hw, cfg.dice_prec);
        comp = to_arith(b_comp);
        break;
      }
      case BIN: {
        b_comp = equality(client_entry.val, server_entry.val);
        // Instead of left-shifting the bool share, it is cheaper to first do a
        // single-bit conversion into an arithmetic share and then a free
        // multiplication with a constant 2^dice_prec
        comp = to_arith(b_comp);
        comp *= constant_simd(acirc, (1 << cfg.dice_prec), BitLen, nvals);
        break;
      }
    }

    ArithShare field_weight = weight * comp;

#ifdef DEBUG_SEL_CIRCUIT
    print_share(weight, format("weight({},{},{})", ftype, ileft, iright));
    print_share(b_comp, format("bool comp({},{},{})", ftype, ileft, iright));
    print_share(comp, format("arith comp({},{},{})", ftype, ileft, iright));
    print_share(field_weight, format("^^^^ field weight({},{},{}) ^^^^", ftype, ileft, iright));
#endif

    return {field_weight, weight};
  }

};

/******************** Public Epilinker Interface ********************/
SecureEpilinker::SecureEpilinker(ABYConfig config, EpilinkConfig epi_config) :
  party{config.role, config.remote_host.c_str(), config.port, LT, BitLen, config.nthreads},
  bcirc{dynamic_cast<BooleanCircuit*>(party.GetSharings()[config.bool_sharing]->GetCircuitBuildRoutine())},
  ccirc{dynamic_cast<BooleanCircuit*>(party.GetSharings()[(config.bool_sharing==S_YAO)?S_BOOL:S_YAO]->GetCircuitBuildRoutine())},
  acirc{dynamic_cast<ArithmeticCircuit*>(party.GetSharings()[S_ARITH]->GetCircuitBuildRoutine())},
  epicfg{epi_config}, selc{make_unique<SELCircuit>(epicfg, bcirc, ccirc, acirc)} {}
// TODO when ABY can separate circuit building/setup/online phases, we create
// different SELCircuits per build_circuit()...

// Need to _declare_ in header but _define_ here because we use a unique_ptr
// pimpl.
SecureEpilinker::~SecureEpilinker() = default;

void SecureEpilinker::build_circuit(const uint32_t) {
  // TODO When separation of setup, online phase and input setting is done in
  // ABY, call selc->build_circuit() here instead of in run()
  // nvals is currently ignored as nvals is just taken from EpilinkInputs
  is_built = true;
}

void SecureEpilinker::run_setup_phase() {
  if (!is_built) {
    throw runtime_error("Circuit must first be built with build_circuit() before running setup phase.");
  }
  is_setup = true;
}

SecureEpilinker::Result SecureEpilinker::run_as_client(
    const EpilinkClientInput& input) {
  if (!is_setup) {
    cerr << "Warning: Implicitly running setup phase." << endl;
    run_setup_phase();
  }
  selc->set_client_input(input);
  return run_circuit();
}

SecureEpilinker::Result SecureEpilinker::run_as_server(
    const EpilinkServerInput& input) {
  if (!is_setup) {
    cerr << "Warning: Implicitly running setup phase." << endl;
    run_setup_phase();
  }
  selc->set_server_input(input);
  return run_circuit();
}

#ifdef DEBUG_SEL_CIRCUIT
SecureEpilinker::Result SecureEpilinker::run_as_both(
    const EpilinkClientInput& in_client, const EpilinkServerInput& in_server) {
  if (!is_setup) {
    cerr << "Warning: Implicitly running setup phase." << endl;
    run_setup_phase();
  }
  selc->set_both_inputs(in_client, in_server);
  return run_circuit();
}
#endif

SecureEpilinker::Result SecureEpilinker::run_circuit() {
  SELCircuit::ResultShares res = selc->build_circuit();
  party.ExecCircuit();

  is_setup = false; // need to run new setup phase

  return {
    res.index.get_clear_value<CircUnit>(),
    res.match.get_clear_value<bool>(),
    res.tmatch.get_clear_value<bool>()
#ifdef DEBUG_SEL_RESULT
    ,res.score_numerator.get_clear_value<CircUnit>()
    // shift by dice-precision to account for precision of threshold, i.e.,
    // get denominator and numerator to same scale
    ,(res.score_denominator.get_clear_value<CircUnit>() << epicfg.dice_prec)
#endif
  };
}

void SecureEpilinker::reset() {
  party.Reset();
  is_built = false;
  is_setup = false;
}

} // namespace sel
