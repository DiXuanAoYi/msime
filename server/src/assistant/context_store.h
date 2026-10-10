#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
namespace ReplyStudio
{
class ContextStore
{
  public:
    explicit ContextStore(std::filesystem::path path) : path_(std::move(path))
    {
    }
    nlohmann::json Apply(const nlohmann::json &request, int64_t now);

  private:
    std::filesystem::path path_;
};
} // namespace ReplyStudio
