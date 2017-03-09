/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/program_options.hpp>
#include <string>
#include <map>
#include <vector>

#include "DexStore.h"
#include "ToolRegistry.h"

namespace po = boost::program_options;

class DexStore;
class DexClass;
using DexClasses = std::vector<DexClass*>;
using DexClassesVector = std::vector<DexClasses>;
using DexStoresVector = std::vector<DexStore>;

class Tool {
 public:

  Tool(const std::string& name, const std::string& desc)
     : m_name(name),
       m_desc(desc) {
    ToolRegistry::get().register_tool(this);
  }

  virtual ~Tool() {}

  virtual void run(const po::variables_map& options) = 0;

  virtual void add_options(po::options_description& options) const {}

  const std::string& name() const { return m_name; }
  const std::string& desc() const { return m_desc; }

 protected:

  DexStoresVector init(
    const std::string& system_jar_paths,
    const std::string& apk_dir,
    const std::string& dexen_dir);

  void add_standard_options(po::options_description& options) const;

 private:
  std::string m_name;
  std::string m_desc;
};
