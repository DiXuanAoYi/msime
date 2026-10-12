#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace ApiCredentialTest
{
struct Request
{
    std::string service;
    std::unordered_map<std::string, std::string> config;
};

struct Result
{
    bool ok = false;
    std::string message;
};

Result Run(const Request &request);
struct ModelListResult
{
    bool ok = false;
    std::string message;
    std::vector<std::string> models;
};
std::string ModelListEndpoint(const std::string &endpoint);
ModelListResult FetchModels(const Request &request);
} // namespace ApiCredentialTest
