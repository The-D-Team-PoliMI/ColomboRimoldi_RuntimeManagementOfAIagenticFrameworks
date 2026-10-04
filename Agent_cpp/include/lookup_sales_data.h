#pragma once
#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include <duckdb.h>
#include <agents-cpp/context.h>
#include <agents-cpp/llm_interface.h>
#include "schema.h"

namespace agent_cpp{

using JsonObject = nlohmann::json;

class LookupSalesData{
private:
    std::shared_ptr<agents::Context> context_;
    duckdb_database db_{nullptr};
    duckdb_connection con_{nullptr};
    DatabaseSchema schema_;

    static const std::string SQL_GENERATION_PROMPT;

    std::string clean_sql_output(const std::string& raw_output);
    std::string query_result_to_csv(duckdb_result& result);

public:
    LookupSalesData(std::shared_ptr<agents::Context> ctx, const DatabaseSchema& schema);
    virtual ~LookupSalesData();

    bool init_database();

    JsonObject run(JsonObject& state);
};

} 