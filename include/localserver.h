/**
\file    localserver.h
\author  Tobias Kussel <kussel@cbs.tu-darmstadt.de>
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
\brief Performs ABY server computation
*/

#ifndef SEL_LOCALSERVER_H
#define SEL_LOCALSERVER_H
#pragma once

#include <memory>
#include "secure_epilinker.h"
#include "seltypes.h"
#include "resttypes.h"

#include "datahandler.h"

namespace sel {
class DataHandler;
class ConfigurationHandler;
struct ServerData;

class LocalServer {
 public:
  LocalServer() = default;
  LocalServer(RemoteId,
              std::string,
              Port,
              std::shared_ptr<DataHandler>,
              std::shared_ptr<ConfigurationHandler>);
  LocalServer(RemoteId,
              SecureEpilinker::ABYConfig,
              EpilinkConfig,
              std::shared_ptr<DataHandler>,
              std::shared_ptr<ConfigurationHandler>);
  RemoteId get_id() const;
  SecureEpilinker::Result run_server();
  SecureEpilinker::Result launch_comparison(std::shared_ptr<const ServerData>);
  Port get_port() const;
  std::string get_ip() const;
  SecureEpilinker& get_epilinker();


  std::vector<std::string> get_ids() const {return m_data->ids;}
  std::shared_ptr<DataHandler> get_data_handler() const {return m_data_handler;}

 private:
  RemoteId m_remote_id;
  std::string m_client_ip;
  Port m_client_port;
  std::shared_ptr<const ServerData> m_data;
  std::shared_ptr<DataHandler> m_data_handler;
  std::shared_ptr<ConfigurationHandler> m_config_handler;
  SecureEpilinker m_aby_server;
};

}  // namespace sel

#endif /* end of include guard: SEL_LOCALSERVER_H */
