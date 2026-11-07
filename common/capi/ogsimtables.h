#pragma once

// opengothic-rs: the small session tables' og-sim delegation (task 5.17, og_sim.h modules
// OG_SIM_MOD_QUESTLOG, ATTITUDE, PERC_RANGES, ROOMS). C++ changes and reads its tables as
// stock; after each change Rust gets the op and C++'s table, applies the op to its own
// and compares (USE: C++ takes Rust's table); every read hands Rust C++'s answer (USE:
// Rust's is used).

#include <cstdint>
#include <string_view>
#include <vector>

#include "../../game/capi/og_sim.h"

class QuestLog;

namespace OgSimTables {
  inline og_sim_str str(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), uint32_t(s.size())};
    }
  // `log` changed by `op` (OG_SIM_QUEST_*; nothing for an off module).
  void    quest(QuestLog& log, const og_sim_table_op& op);
  // A table of `module` changed: `table` holds C++'s entries (NULL/0 for DROP).
  void    changed(uint32_t module, const void* key, const og_sim_table_op& op, int32_t* table, uint32_t len);
  inline void changed(uint32_t module, const void* key, const og_sim_table_op& op, std::vector<int32_t>& table) {
    changed(module, key, op, table.data(), uint32_t(table.size()));
    }
  // A read of a table of `module`: the answer to use.
  int32_t query(uint32_t module, const void* key, const og_sim_table_op& op, int32_t cpp);
  }
