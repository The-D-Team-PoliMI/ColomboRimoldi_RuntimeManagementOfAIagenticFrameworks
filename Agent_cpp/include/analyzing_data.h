#pragma once
#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include <agents-cpp/context.h>

namespace agent_cpp{

using JsonObject = nlohmann::json;

class AnalyzingData{
private:
    std::shared_ptr<agents::Context> context_;
    static const std::string DATA_ANALYSIS_PROMPT;

public:
    explicit AnalyzingData(std::shared_ptr<agents::Context> ctx);
    virtual ~AnalyzingData() = default;

    JsonObject run(JsonObject& state);
};

} 