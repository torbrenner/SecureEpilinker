#include "datahandler.h"
#include "configurationhandler.h"
#include "databasefetcher.h"
#include "localconfiguration.h"
#include <memory>
#include <mutex>
#include "clear_epilinker.h"

using namespace std;
namespace sel {

#ifdef DEBUG_SEL_REST
bool Debugger::all_values_set() const{
  return client_input && server_input && epilink_config;
}

void Debugger::compute_int() {
  int_result = clear_epilink::calc_integer({*client_input, *server_input},*epilink_config);
}

void Debugger::compute_double() {
  double_result = clear_epilink::calc_exact({*client_input, *server_input},*epilink_config);
}

void Debugger::reset() {
  client_input.reset(); server_input.reset(); epilink_config.reset();
  int_result = {}; double_result = {};
  run = false;
}
#endif

  DataHandler& DataHandler::get() {
    static DataHandler singleton;
    return singleton;
  }

  DataHandler const& DataHandler::cget() {
    return cref(get());
  }

size_t DataHandler::poll_database(const RemoteId& remote_id) {
  const auto& config_handler{ConfigurationHandler::cget()};
  const auto local_configuration{config_handler.get_local_config()};
  DatabaseFetcher database_fetcher{
      local_configuration, config_handler.get_algorithm_config(),
      local_configuration->get_data_service()+"/"+remote_id,
      local_configuration->get_local_authentication(),
      config_handler.get_server_config().default_page_size};
  auto data{database_fetcher.fetch_data()};
  lock_guard<mutex> lock(m_db_mutex);
  m_database = make_shared<const ServerData>(ServerData{
      move(data.data), move(data.ids), data.todate});
  return (m_database->data.begin()->second.size());
}

size_t DataHandler:: poll_database_diff() {
  // TODO(TK): Implement
  //return poll_database();
  return 0;
}

std::shared_ptr<const ServerData> DataHandler::get_database() const{
  lock_guard<mutex> lock(m_db_mutex);
  return m_database;
}
}  // namespace sel
