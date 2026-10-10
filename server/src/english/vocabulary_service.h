#pragma once
#include "english/vocabulary_store.h"
#include <boost/json.hpp>
namespace Vocabulary
{
void Enqueue(Batch batch);
boost::json::object HandleRequest(const boost::json::object &data);
} // namespace Vocabulary
