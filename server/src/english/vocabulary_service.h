#pragma once
#include "english/vocabulary_store.h"
#include <boost/json.hpp>
namespace Vocabulary
{
void Enqueue(Batch batch);
// Memory-only on the candidate path; refreshed by the writer thread.
bool IsFamiliar(const std::string &language, const std::string &word);
boost::json::object HandleRequest(const boost::json::object &data);
} // namespace Vocabulary
