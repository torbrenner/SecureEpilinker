#include "cxxopts.hpp"
#include "fmt/format.h"
#include "abycore/aby/abyparty.h"

#include "../include/logger.h"
#include "../include/util.h"
#include "../include/jsonutils.h"
#include "../include/secure_epilinker.h"
#include "../include/clear_epilinker.h"
#include "random_input_generator.h"

#include <filesystem>

using namespace std;
using fmt::print, fmt::format;
using nlohmann::json;

namespace fs = std::filesystem;

namespace sel::test {

// GLOBALS
// Whether to use run_as_{client/server}() or run_as_both()
bool run_both{false};
e_role role;
bool only_local{false};

constexpr auto BIN = FieldComparator::BINARY;
constexpr auto BM = FieldComparator::DICE;
constexpr double Threshold = 0.9;
constexpr double TThreshold = 0.7;
const fs::path CircDir = "../data/circ";

struct FieldData { FieldSpec field; Bitmask data; };

map<string, FieldData> make_test_data() {
  vector<FieldData> field_data = {
    {
      { "int_1", 1.0, FieldComparator::BINARY, FieldType::INTEGER, 29 },
      {0xde, 0xad, 0xbe, 0xef}
    },
    {
      { "int_2", 3.0, FieldComparator::BINARY, FieldType::INTEGER, 32 },
      {0xde, 0xce, 0xa5, 0xed}
    },
    {
      { "bm_1", 2.0, FieldComparator::DICE, FieldType::BITMASK, 8 },
      {1}
    },
    {
      { "bm_2", 4.0, FieldComparator::DICE, FieldType::BITMASK, 8 },
      {1}
    }
  };

  map<string, FieldData> ret;
  for (auto& fd : field_data) {
    ret.emplace(fd.field.name, move(fd));
  }
  return ret;
}

EpilinkConfig make_dkfz_cfg() {
  return {
    { // begin map<string, ML_Field>
      { "vorname",
        FieldSpec("vorname", 0.000235, 0.01, "dice", "bitmask", 500) },
      { "nachname",
        FieldSpec("nachname", 0.0000271, 0.008, "dice", "bitmask", 500) },
      { "geburtsname",
        FieldSpec("geburtsname", 0.0000271, 0.008, "dice", "bitmask", 500) },
      { "geburtstag",
        FieldSpec("geburtstag", 0.0333, 0.005, "binary", "integer", 5) },
      { "geburtsmonat",
        FieldSpec("geburtsmonat", 0.0833, 0.002, "binary", "integer", 4) },
      { "geburtsjahr",
        FieldSpec("geburtsjahr", 0.0286, 0.004, "binary", "integer", 11) },
      { "plz",
        FieldSpec("plz", 0.01, 0.04, "binary", "string", 40) },
      { "ort",
        FieldSpec("ort", 0.01, 0.04, "dice", "bitmask", 500) }
    }, // end map<string, ML_Field>
    { { "vorname", "nachname", "geburtsname" } }, // exchange groups
    Threshold, TThreshold
  };
}

auto set_inputs(SecureEpilinker& linker,
    const EpilinkClientInput& in_client, const EpilinkServerInput& in_server) {
  print("Calling set_{}_input()\n", run_both ? "both" : ((role==CLIENT) ? "client" : "server"));
  if (!run_both) {
    if (role==CLIENT) {
      linker.set_client_input(in_client);
    } else {
      linker.set_server_input(in_server);
    }
  }
#ifdef DEBUG_SEL_CIRCUIT
  else {
    linker.run_as_both(in_client, in_server);
  }
#else
  else {
    throw runtime_error("Not compiled with DEBUG_SEL_CIRCUIT, cannot set both inputs.");
  }
#endif
}

EpilinkInput input_simple(uint32_t dbsize) {
  auto td = make_test_data();
  auto& data_int_1 = td["int_1"].data;
  auto& f_int1 = td["int_1"].field;
  print("data_int_1: {}\n", data_int_1);

  // First test: only one bin field, single byte integer
  EpilinkConfig epi_cfg {
    { {"int_1", f_int1}, }, // fields
    {}, // exchange groups
    Threshold, TThreshold // size_bitmask, (tent.) thresholds
  };

  //Bitmask data_int_zero(data_int_1.size(), 0);

  EpilinkClientInput in_client {
    { {"int_1", data_int_1} }, // record
    dbsize // dbsize
  };

  EpilinkServerInput in_server {
    { {"int_1", vector<FieldEntry>(dbsize, data_int_1)} }, // db
    1 // num_records
  };

  return {move(epi_cfg), move(in_client), move(in_server)};
}

EpilinkInput input_simple_bm(uint32_t dbsize) {
  auto td = make_test_data();
  // Only one bm field, single byte integer
  EpilinkConfig epi_cfg {
    { {"bm_1", td["bm_1"].field}, }, // fields
    {}, // exchange groups
    Threshold, TThreshold // size_bitmask, (tent.) thresholds
  };

  //Bitmask data_int_zero(data_int_1.size(), 0);

  EpilinkClientInput in_client {
    { {"bm_1", Bitmask{0b01110111}} }, // record
    dbsize // dbsize
  };

  EpilinkServerInput in_server {
    { {"bm_1", vector<FieldEntry>(dbsize, Bitmask{0b11101110})} }, // db
    1 // num_records
  };

  return {move(epi_cfg), move(in_client), move(in_server)};
}

EpilinkInput input_exchange_grp(uint32_t dbsize) {
  auto td = make_test_data();
  auto& data_int_1 = td["int_1"].data;
  auto& data_int_2 = td["int_2"].data;
  auto& f_int1 = td["int_1"].field;
  auto& f_int2 = td["int_2"].field;
  auto& f_bm1 = td["bm_1"].field;
  auto& f_bm2 = td["bm_2"].field;

  // First test: only one bin field, single byte bitmask
  EpilinkConfig epi_cfg {
    {
      {"int_1", f_int1},
      {"int_2", f_int2},
      {"bm_1", f_bm1},
      {"bm_2", f_bm2},
    }, // fields
    {{"bm_1", "bm_2"}}, // exchange groups
    Threshold, TThreshold // size_bitmask, (tent.) thresholds
  };

  EpilinkClientInput in_client {
    {
      {"bm_1", Bitmask{0x33}},
      {"bm_2", Bitmask{0x43}},
      {"int_1", data_int_1},
      {"int_2", data_int_2}
    }, // record
    dbsize // dbsize
  };

  EpilinkServerInput in_server {
    {
      {"bm_1", vector<FieldEntry>(dbsize, Bitmask{0x44})}, // 2-bit mismatch
      {"bm_2", vector<FieldEntry>(dbsize, Bitmask{0x35})}, // 1-bit mismatch
      {"int_1", vector<FieldEntry>(dbsize, data_int_1)},
      {"int_2", vector<FieldEntry>(dbsize, data_int_2)}
    }, // db
    1 // num_records
  };

  return {move(epi_cfg), move(in_client), move(in_server)};
}

EpilinkInput input_empty() {
  auto td = make_test_data();
  auto& f_bm1 = td["bm_1"].field;
  auto& f_bm2 = td["bm_2"].field;

  // First test: only one bin field, single byte bitmask
  EpilinkConfig epi_cfg {
    {
      {"bm_1", f_bm1},
      {"bm_2", f_bm2},
    }, // fields
    {}, // exchange groups
    Threshold, TThreshold // (tent.) thresholds
  };

  EpilinkClientInput in_client {
    {
      {"bm_1", nullopt},
      {"bm_2", Bitmask{0x44}},
    }, // record
    2 // dbsize
  };

  EpilinkServerInput in_server {
    {
      {"bm_1", { nullopt, Bitmask{0x31} }}, // 1-bit mismatch for #1
      {"bm_2", { Bitmask{0x43}, Bitmask{0x44} }}, // 2-bit mismatch for #0
    }, // db
    1 // num_records
  };

  return {move(epi_cfg), move(in_client), move(in_server)};
}

EpilinkInput input_dkfz_random(size_t dbsize) {
  RandomInputGenerator random_input(make_dkfz_cfg());
  random_input.set_client_empty_fields({"ort"});
  return random_input.generate(dbsize);
}

EpilinkConfig read_config_file(const fs::path& cfg_path) {
  auto config_json = read_json_from_disk(cfg_path).at("algorithm");
  return parse_json_epilink_config(config_json);
}

auto read_database_file(const fs::path& db_path, const EpilinkConfig& epi_cfg) {
  auto db_json = read_json_from_disk(db_path);
  return parse_json_fields_array(epi_cfg.fields, db_json.at("records"));
}

auto read_database_dir(const fs::path& dir_path, const EpilinkConfig& epi_cfg) {
  VRecord db;
  for (auto& f: fs::directory_iterator(dir_path)) {
    if (f.path().extension() == ".json") {
      auto temp_db = read_database_file(f, epi_cfg);
      append_to_map_of_vectors(temp_db, db);
    }
  }
  return db;
}

EpilinkInput input_json(const fs::path& local_config_file_path,
    const fs::path& record_file_path,
    const fs::path& database_file_or_dir_path) {

  auto epi_cfg = read_config_file(local_config_file_path);

  auto record_json = read_json_from_disk(record_file_path).at("fields");
  auto record = parse_json_fields(epi_cfg.fields, record_json);

  auto db = fs::is_directory(database_file_or_dir_path) ?
      read_database_dir(database_file_or_dir_path, epi_cfg) :
      read_database_file(database_file_or_dir_path, epi_cfg);

  EpilinkServerInput server_in{make_shared<VRecord>(db), 1};
  EpilinkClientInput client_in{record, server_in.database_size};
  return {move(epi_cfg), move(client_in), move(server_in)};
}

string test_scripts_dir_path() {
  auto dirp = getenv("SEL_test_scripts");
  return dirp ? dirp : "../test_scripts/";
}

EpilinkInput input_test_json() {
  string dir = test_scripts_dir_path();

  return input_json(
      dir + "configurations/local_init_tuda1.json"s,
      dir + "configurations/validlink.json"s,
      dir + "database"s);
}

EpilinkInput input_json_multi_request(
    const fs::path& local_config_file_path,
    const fs::path& requests_file_path,
    const fs::path& database_file_or_dir_path) {

  auto epi_cfg = read_config_file(local_config_file_path);

  auto db = fs::is_directory(database_file_or_dir_path) ?
    read_database_dir(database_file_or_dir_path, epi_cfg) :
    read_database_file(database_file_or_dir_path, epi_cfg);

  auto requests_json = read_json_from_disk(requests_file_path).at("requests");
  Records records;
  for (auto& record_json : requests_json) {
    auto record = parse_json_fields(epi_cfg.fields, record_json.at("fields"));
    records.push_back(record);
  }

  EpilinkServerInput server_in{make_shared<VRecord>(db), records.size()};
  EpilinkClientInput client_in{make_unique<Records>(records), server_in.database_size};
  return {move(epi_cfg), move(client_in), move(server_in)};
}

auto input_multi_test_0824() {
  string dir = test_scripts_dir_path() + "inputs/2018-08-24/"s;
  return input_json_multi_request(
      dir + "local_init.json",
      dir + "requests.json",
      dir + "db.json");
}

auto input_single_test_0824() {
  string dir = test_scripts_dir_path() + "inputs/2018-08-24/"s;
  return input_json(
      dir + "local_init.json",
      dir + "request_1.json",
      dir + "db_1.json");
}

auto run_sel_linkage(SecureEpilinker& linker, const EpilinkInput& in) {
  linker.build_linkage_circuit(in.client.num_records, in.client.database_size);
  linker.run_setup_phase();
  set_inputs(linker, in.client, in.server);
  const auto res = linker.run_linkage();
  linker.reset();
  return res;
}

auto run_sel_count(SecureEpilinker& linker, const EpilinkInput& in) {
  linker.build_count_circuit(in.client.num_records, in.client.database_size);
  linker.run_setup_phase();
  set_inputs(linker, in.client, in.server);
  const auto res = linker.run_count();
  linker.reset();
  return res;
}

template <typename T>
auto resized_config(const CircuitConfig& cfg) {
  if constexpr (is_integral_v<T>) {
    return make_unique<CircuitConfig>(cfg.epi, CircDir, true, sizeof(T)*8);
  } else {
    return make_unique<CircuitConfig>(cfg);
  }
}

template <typename T>
auto run_local_linkage(const EpilinkInput& in) {
  const auto cfg = resized_config<T>(in.cfg);
  return clear_epilink::calc<T>(*in.client.records, *in.server.database, *cfg);
}

template <typename T>
auto run_local_count(const EpilinkInput& in) {
  const auto cfg = resized_config<T>(in.cfg);
  return clear_epilink::calc_count<T>(*in.client.records, *in.server.database, *cfg);
}

template <typename T, typename U>
double deviation_perc(const Result<T>& l, const Result<U>& r) {
  return ( 1.0 - ((double)(r.sum_field_weights * l.sum_weights))
    / (r.sum_weights * l.sum_field_weights) ) * 100;
}

template <typename T>
void print_local_result(Result<CircUnit>* sel, const Result<T>& local, const string& name) {
  string dev = "";
  if (sel) {
    const auto dev_perc = deviation_perc(*sel, local);
    if (dev_perc) dev = format("{:+.3f}%", dev_perc);
  }
  print("------ {} ------\n{} {}\n", name, local, dev);
}

string test_str(bool test) {
  return test ? "✅" : "💥";
}

void run_and_print_linkage(SecureEpilinker& linker, const EpilinkInput& in) {
  vector<Result<CircUnit>> results;
  if (!only_local) results = run_sel_linkage(linker, in);
  const auto results_32 = run_local_linkage<uint32_t>(in);
  const auto results_64 = run_local_linkage<uint64_t>(in);
  const auto results_double = run_local_linkage<double>(in);

  bool all_good = true;
  for (size_t i = 0; i != in.client.num_records; ++i) {
    print("********************* {} ********************\n", i);
    Result<CircUnit>* resp = nullptr;
    if (!only_local) {
      resp = &results[i];
      bool correct = *resp == results_32[i];
      all_good &= correct;
      print("------ Secure Epilinker -------\n{} {}\n", *resp, test_str(correct));
    }
    print_local_result(resp, results_32[i], "32 Bit");
    print_local_result(resp, results_64[i], "64 Bit");
    print_local_result(resp, results_double[i], "Double");
  }

  if (all_good) {
    print ("🎉🎉🎉 All good! 🎉🎉🎉\n");
  } else {
    print ("💩💩💩 Errors occured! 💩💩💩\n");
  }
}

void run_and_print_counting(SecureEpilinker& linker, const EpilinkInput& in) {
  vector<pair<string, CountResult<size_t>>> results;
  if (!only_local) {
    const auto sel_result = run_sel_count(linker, in);
    results.emplace_back("SEL",
        CountResult<size_t>{sel_result.matches, sel_result.tmatches});
  }
  results.emplace_back("32 Bit", run_local_count<uint32_t>(in));
  results.emplace_back("64 Bit", run_local_count<uint64_t>(in));
  results.emplace_back("Double", run_local_count<double>(in));

  const auto& first_result = results.cbegin()->second;
  vector<bool> same_count = {true, true};
  print("\tmatches\ttmatches\n");
  for (const auto& resp : results) {
    const auto& res = resp.second;
    print("{}\t{}\t{}\n", resp.first, res.matches, res.tmatches);
    same_count[0] = same_count[0] && (first_result.matches == res.matches);
    same_count[1] = same_count[1] && (first_result.tmatches == res.tmatches);
  }
  for (const auto good : same_count) {
    print("\t{}", test_str(good));
  }
  print("\n");
}

} /* END namespace sel::test */

using namespace sel;
using namespace sel::test;

int main(int argc, char *argv[])
{
  bool role_server = false;
  unsigned int sharing = S_YAO;
  uint32_t dbsize = 1;
  uint32_t nthreads = 1;
  string remote_host = "127.0.0.1";
  bool match_counting = false;

  cxxopts::Options options{"test_aby", "Test ABY related components"};
  options.add_options()
    ("S,server", "Run as server. Default to client", cxxopts::value(role_server))
    ("R,remote-host", "Remote host. Default 127.0.0.1", cxxopts::value(remote_host))
    ("s,sharing", "Boolean sharing to use. 0: GMW, 1: YAO (default)", cxxopts::value(sharing))
    ("n,dbsize", "Database size", cxxopts::value(dbsize))
    ("r,run-both", "Use set_both_inputs()", cxxopts::value(run_both))
    ("L,local-only", "Only run local calculations on clear values."
        " Doesn't initialize the SecureEpilinker.", cxxopts::value(only_local))
    ("m,match-count", "Run match counting instead of linkage.", cxxopts::value(match_counting))
    ("v,verbose", "Set verbosity. May be specified multiple times to log on "
      "info/debug/trace level. Default level is warning.")
    ("h,help", "Print help");
  auto op = options.parse(argc, argv);

  if (op["help"].as<bool>()) {
    cout << options.help() << endl;
    return 0;
  }

  create_terminal_logger();
  switch(op.count("verbose")){
    case 0: spdlog::set_level(spdlog::level::warn); break;
    case 1: spdlog::set_level(spdlog::level::info); break;
    case 2: spdlog::set_level(spdlog::level::debug); break;
    default: spdlog::set_level(spdlog::level::trace); break;
  }

  role = role_server ? SERVER : CLIENT;

  SecureEpilinker::ABYConfig aby_cfg {
    role, (e_sharing)sharing, remote_host, 5676, nthreads
  };

  //const auto in = input_dkfz_random(dbsize);
  const auto in = input_multi_test_0824();

  CircuitConfig circ_cfg{in.cfg, CircDir, true};
  SecureEpilinker linker{aby_cfg, circ_cfg};
  if(!only_local) linker.connect();
  if(match_counting) run_and_print_counting(linker, in);
  else run_and_print_linkage(linker, in);

  return 0;
}
