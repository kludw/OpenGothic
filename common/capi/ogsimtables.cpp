#include "ogsimtables.h"

#include <string>

#include <Tempest/Log>

#include "capi/ogsim.h"
#include "game/questlog.h"

struct OgSimQuestAccess {
  static std::vector<QuestLog::Quest>& quests(QuestLog& l) { return l.quests; }
  };

namespace {
// Called from Rust: nothing may unwind out of it.
void adopt(void* ctx, const og_sim_quest* rows, uint32_t count) noexcept try {
  auto& quests = *static_cast<std::vector<QuestLog::Quest>*>(ctx);
  std::vector<QuestLog::Quest> next(count);
  for(uint32_t i=0; i<count; ++i) {
    auto& r = rows[i];
    auto& q = next[i];
    q.name    = std::string(reinterpret_cast<const char*>(r.name.data), r.name.len);
    q.section = QuestLog::Section(r.section);
    q.status  = QuestLog::Status(r.status);
    for(uint32_t k=0; k<r.entry_count; ++k)
      q.entry.emplace_back(reinterpret_cast<const char*>(r.entries[k].data), r.entries[k].len);
    }
  quests = std::move(next);
  }
catch(...) {
  Tempest::Log::e("og-sim questlog: adopting Rust's quest log failed; C++'s is kept");
  }
}

void OgSimTables::quest(QuestLog& log, const og_sim_table_op& op) {
  if(!OgSim::on(OG_SIM_MOD_QUESTLOG))
    return;
  auto& quests = OgSimQuestAccess::quests(log);
  std::vector<og_sim_quest>            rows;
  std::vector<std::vector<og_sim_str>> entries;
  if(op.op!=OG_SIM_QUEST_DROP) {
    rows.reserve(quests.size());
    entries.resize(quests.size());
    for(size_t i=0; i<quests.size(); ++i) {
      auto& q = quests[i];
      for(auto& e:q.entry)
        entries[i].push_back(str(e));
      rows.push_back({str(q.name), uint32_t(q.section), uint32_t(q.status),
                      entries[i].empty() ? nullptr : entries[i].data(), uint32_t(entries[i].size())});
      }
    }
  auto& t = OgSim::table();
  t.quest(t.user, reinterpret_cast<uint64_t>(&log), &op, rows.empty() ? nullptr : rows.data(),
          uint32_t(rows.size()), adopt, &quests);
  }

void OgSimTables::changed(uint32_t module, const void* key, const og_sim_table_op& op, int32_t* table, uint32_t len) {
  if(!OgSim::on(module))
    return;
  auto& t = OgSim::table();
  t.table(t.user, module, reinterpret_cast<uint64_t>(key), &op, table, len);
  }

int32_t OgSimTables::query(uint32_t module, const void* key, const og_sim_table_op& op, int32_t cpp) {
  if(!OgSim::on(module))
    return cpp;
  auto& t = OgSim::table();
  int32_t r = t.query(t.user, module, reinterpret_cast<uint64_t>(key), &op, cpp);
  return OgSim::mode(module)==OG_SIM_USE ? r : cpp;
  }
