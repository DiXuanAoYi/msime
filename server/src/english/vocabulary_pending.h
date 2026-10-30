#pragma once
#include "english/vocabulary_store.h"
#include <map>
#include <optional>
#include <tuple>

namespace Vocabulary
{
// Worker-thread-only ledger. No text crosses IPC; an acknowledgement identifies one staged selection.
class Pending
{
  public:
    using Key = std::tuple<uint64_t, uint64_t, uint64_t>;
    void Arm(Key key, Batch batch, uint64_t now)
    {
        Expire(now);
        if (std::get<0>(key) && std::get<1>(key) && std::get<2>(key) && batches_.size() < 64)
            batches_.emplace(key, std::make_pair(now, std::move(batch)));
    }
    std::optional<Batch> Confirm(Key key, uint64_t now)
    {
        Expire(now);
        const auto it = batches_.find(key);
        if (it == batches_.end())
            return std::nullopt;
        auto batch = std::move(it->second.second);
        batches_.erase(it);
        return batch;
    }
    void Reset()
    {
        batches_.clear();
    }

  private:
    void Expire(uint64_t now)
    {
        for (auto it = batches_.begin(); it != batches_.end();)
            if (now - it->second.first > 10000)
                it = batches_.erase(it);
            else
                ++it;
    }
    std::map<Key, std::pair<uint64_t, Batch>> batches_;
};
} // namespace Vocabulary
