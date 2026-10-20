#pragma once

#include <string>
#include <vector>

namespace ccbus {

int cmd_hook();
int cmd_serve();
int cmd_install_hooks(const std::string& settings_path, bool uninstall);
int cmd_new(const std::string& name, const std::string& cwd, const std::vector<std::string>& args);
int cmd_ls();

}  // namespace ccbus
